//
// Created by charl on 1/13/2026.
//

#include "../include/server.hpp"
#include "perf.hpp"
#include "tsc_clock.hpp"

#include <immintrin.h>
#include <iostream>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace {

// Under TESTING the net_ring0 vdev needs a second queue pair so a test can
// play the client; see the constructor.
#if TESTING
constexpr uint16_t kRxQueues = 2;
constexpr uint16_t kTxQueues = 2;
#else
constexpr uint16_t kRxQueues = 1;
constexpr uint16_t kTxQueues = 1;
#endif

// Max responses staged before handing them to the NIC in one burst.
constexpr size_t kTxBurst = 32;

// Room for any one SoupBinTCP packet the server sends: the largest is a
// Sequenced Data packet around an Order Accepted carrying every appendage.
constexpr size_t kMaxSoupPacket = 256;

// How often outbound_ checks whether any session is due a heartbeat.
constexpr uint64_t kHeartbeatScanNs = 100'000'000;

// Production wants the real EAL defaults: probe the PCI bus, bind whatever
// NIC is bound to a DPDK PMD, use hugepages. Tests can't assume any of
// that -- no hugepage permissions, no NIC bound away from the kernel -- so
// server-tests (TESTING=1) instead brings up a software-only EAL: no
// hugepages (regular process memory instead), no PCI probing, and a single
// net_ring vdev standing in for port 0.
dpdk::runtime_params make_runtime_params() {
#if TESTING
    return dpdk::runtime_params{
        .program_name = "exchange-server",
        .legacy_mem_mb = 128,
        .no_huge = true,
        .no_pci = true,
        .vdevs = {"net_ring0"},
    };
#else
    return dpdk::runtime_params{.program_name = "exchange-server"};
#endif
}

} // namespace

// dpdk::runtime/dpdk::port have no default constructor, so they can't be
// left blank in the mem-initializer list and assigned in the body — they
// have to be constructed directly, in declaration order, right here.
// A constructor function-try-block only gets to log a failure; it can't
// swallow it; the exception is always rethrown once the handler exits,
// since a Server whose dpdk_runtime_/dpdk_port_ failed to construct can't
// be handed back as a valid object.
Server::Server(std::shared_ptr<InboundRing> inbound_ring,
               std::shared_ptr<OutboundRing> outbound_ring)
try :
    dpdk_runtime_(make_runtime_params()),
    dpdk_port_(std::move(dpdk_runtime_.add_port(/*port_id=*/0, kRxQueues, kTxQueues))),
    inbound_ring_(std::move(inbound_ring)),
    outbound_ring_(std::move(outbound_ring))
{
    // Queues are claimed here, on the setup thread, before either worker
    // exists (port's claim_* aren't meant to race). Under TESTING the
    // net_ring0 vdev loops tx queue i straight back into rx queue i, so the
    // server takes rx0 + tx1 -- it never hears its own replies -- and leaves
    // tx0 (into the server's rx0) and rx1 (out of its tx1) for a test to
    // play the client on.
    rx_queue_ = dpdk_port_.claim_rx_queue();
#if TESTING
    test_client_tx_queue_ = dpdk_port_.claim_tx_queue();
#endif
    tx_queue_ = dpdk_port_.claim_tx_queue();
} catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
}

Server::~Server() {
    stop();
}

void Server::run() {
    inbound_thread_ = std::jthread([this] {
#if !TESTING
        pin_to_core(INBOUND_CORE);
#endif
        inbound_();
    });
    outbound_thread_ = std::jthread([this] {
#if !TESTING
        pin_to_core(OUTBOUND_CORE);
#endif
        outbound_();
    });
}

void Server::stop() {
    shutdown_.store(true);
    if (inbound_thread_.joinable()) inbound_thread_.join();
    if (outbound_thread_.joinable()) outbound_thread_.join();
}

// (src ip << 16 | src port) as a lookup key -- see the client_ids_ comment
// in server.hpp for why this stands in for a real session identity here.
namespace {
uint64_t endpoint_key(const PacketEndpoint &src, const TCPHeader &tcp) {
    return (static_cast<uint64_t>(src.ip) << 16) | tcp.src_port;
}
} // namespace

ClientId Server::resolve_client_id_(const PacketEndpoint &src, const PacketEndpoint &dst, const TCPHeader &tcp) {
    const uint64_t key = endpoint_key(src, tcp);
    if (const auto it = client_ids_.find(key); it != client_ids_.end()) {
        return it->second;
    }
    if (next_client_id_ > MAX_CLIENTS) {
        return 0;
    }

    const ClientId client_id = next_client_id_++;
    client_ids_.emplace(key, client_id);
    // Written once, before this client's first message goes anywhere -- see
    // endpoints_ in server.hpp.
    endpoints_[client_id] = ClientEndpoint{
        .remote = src, .local = dst, .remote_port = tcp.src_port, .local_port = tcp.dst_port,
    };
    return client_id;
}

//=============================================================================
// inbound: DPDK rx -> SoupBinTCP -> OrderEntry -> notes_ / engine
//=============================================================================

void Server::inbound_() {
    while (!shutdown_.load()) {
        const Timestamp read_begin_tsc = __rdtsc();
        dpdk::packet_burst burst = rx_queue_->receive_burst();

        if (burst.empty()) {
            continue;
        }

        const InboundStamps stamps{.read_begin_tsc = read_begin_tsc, .recv_tsc = __rdtsc()};

        for (const dpdk::packet &packet : burst) {
            const auto deserialized = packet_factory<TCPHeader, RawPayload>::deserialize_packet(packet);

            // deserialize_packet zeroes src.ip on any parse failure -- too
            // short, not IPv4, IPv4 options present, or the IP protocol
            // didn't match TCP. Nothing downstream can trust an unparsed
            // packet. Payload-less segments (a bare ACK) carry nothing either.
            if (deserialized.src.ip == 0 || deserialized.payload.bytes.empty()) {
                continue;
            }

            const ClientId client_id =
                resolve_client_id_(deserialized.src, deserialized.dst, deserialized.transport);
            if (client_id == 0) {
                continue; // session table full
            }

            // Stub: the segment holds whole SoupBinTCP packets. Without TCP
            // reassembly a packet split across segments can't be recovered,
            // so whatever doesn't parse as complete is dropped.
            std::span<const std::byte> stream = deserialized.payload.bytes;
            while (!stream.empty()) {
                const SoupParseResult parsed = parse_soup_packet(stream);
                if (parsed.status != SoupParseStatus::PACKET) break;
                handle_soup_packet_(client_id, parsed.packet, stamps);
                stream = stream.subspan(parsed.consumed);
            }
        }
    }
}

void Server::handle_soup_packet_(const ClientId client_id, const SoupPacket &packet, const InboundStamps &stamps) {
    InboundSession &session = inbound_sessions_[client_id];
    const auto session_note = [&](const GatewayNote::Kind kind, const uint16_t reason = 0) {
        GatewayNote note{.kind = kind, .reject_reason = reason, .order_id = 0, .record = {}};
        note.record.client_id = client_id;
        push_note_(note);
    };

    switch (packet.type) {
        case SoupPacketType::LOGIN_REQUEST: {
            const std::optional<SoupLoginRequest> login = decode_soup_login_request(packet.payload);
            if (!login || session.logged_in) return;
            // Blank means "the current session". The spec only says how Login
            // Accepted pads a session id, so accept the name padded either way.
            const SoupSessionId &requested = login->requested_session;
            if (requested != wire::alpha<10>("") && requested != soup_session_id(SOUP_SESSION) &&
                requested != wire::alpha<10>(SOUP_SESSION)) {
                session_note(GatewayNote::Kind::LOGIN_REJECTED, soup_reject::kSessionNotAvailable);
                return;
            }
            session.logged_in = true;
            session_note(GatewayNote::Kind::LOGIN_ACCEPTED);
            return;
        }

        case SoupPacketType::UNSEQUENCED_DATA:
            // Only allowed after Login Accepted.
            if (session.logged_in) handle_ouch_(client_id, decode_inbound_ouch(packet.payload), stamps);
            return;

        case SoupPacketType::LOGOUT_REQUEST:
            if (!session.logged_in) return;
            session.logged_in = false;
            session_note(GatewayNote::Kind::LOGGED_OUT);
            return;

        default:
            return; // client heartbeats, debug, and anything else a client shouldn't send
    }
}

void Server::handle_ouch_(const ClientId client_id, const OuchInboundFrame &msg, const InboundStamps &stamps) {
    if (msg.message_type == InboundOuchMessageType::NONE) {
        return; // malformed or not an inbound OUCH message
    }

    Translation translation = order_entry_.translate(client_id, msg);

    // Note before request: the outbound thread must already have an order's
    // record when the engine's event for it arrives.
    if (translation.note) {
        push_note_(*translation.note);
    }
    if (translation.request) {
        EngineRequest &request = *translation.request;
        request.read_begin_tsc  = stamps.read_begin_tsc;
        request.recv_tsc        = stamps.recv_tsc;
        request.server_push_tsc = __rdtsc();
        while (!inbound_ring_->push(request) && !shutdown_.load()) {
            // ring full; spin until there's room or we're shutting down
        }
    }
}

void Server::push_note_(const GatewayNote &note) {
    while (!notes_.push(note) && !shutdown_.load()) {
        // ring full; spin until there's room or we're shutting down
    }
}

//=============================================================================
// outbound: notes_ / engine -> OuchResponder -> SoupBinTCP -> DPDK tx
//=============================================================================

void Server::outbound_() {
    const std::shared_ptr<dpdk::packet_pool> pool = dpdk_port_.get_pool();
    const TscClock &clock = TscClock::instance();

    std::array<dpdk::packet, kTxBurst> burst;
    // burst[i]'s latency sample, if it carries a request's answer (see latency.hpp).
    std::array<std::optional<PendingLatency>, kTxBurst> burst_latency;
    size_t staged = 0;

    // Sends everything staged, retrying whatever the NIC didn't take. Only
    // shutdown can leave packets unsent; they're dropped (freed) then, and
    // their latency samples with them.
    const auto flush = [&] {
        const Timestamp t6 = __rdtsc();
        std::span<dpdk::packet> pending{burst.data(), staged};
        while (!pending.empty() && !shutdown_.load()) {
            pending = pending.subspan(tx_queue_->send_burst(pending));
        }
        const Timestamp t7 = __rdtsc();

        for (size_t i = 0; i < staged; ++i) {
            if (burst_latency[i] && pending.empty()) {
                const PendingLatency &l = *burst_latency[i];
                latency_.record({l.t0, l.t1, l.t2, l.t3, l.t4, l.t5, t6, t7});
            }
            burst_latency[i].reset();
            burst[i] = dpdk::packet{};
        }
        staged = 0;
    };

    // One SoupBinTCP packet to one client, in its own TCP segment. latency is
    // the sample to record once it's on the wire, if it answers a request.
    const auto send = [&](const ClientId client_id, const std::span<const std::byte> soup,
                          const PendingLatency *latency = nullptr) {
        const ClientEndpoint &endpoint = endpoints_[client_id];
        const TCPHeader tcp{.src_port = endpoint.local_port, .dst_port = endpoint.remote_port,
                            .flags = RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG};
        const auto build = [&] {
            return packet_factory<TCPHeader, RawPayload>::build_packet(
                *pool, endpoint.local, endpoint.remote, tcp, RawPayload{soup});
        };

        dpdk::packet packet = build();
        // An empty pool means mbufs are still out with the NIC: hand it what's
        // staged (freeing those once sent) and try again rather than drop.
        while (!packet && !shutdown_.load()) {
            flush();
            packet = build();
        }
        if (!packet) return;

        outbound_sessions_[client_id].last_sent_tsc = __rdtsc();
        if (latency != nullptr) burst_latency[staged] = *latency;
        burst[staged++] = std::move(packet);
        if (staged == burst.size()) flush();
    };

    std::array<std::byte, kMaxSoupPacket> scratch{};

    const auto send_ouch = [&](const OuchResponse &response, const PendingLatency *latency = nullptr) {
        OutboundSession &session = outbound_sessions_[response.client_id];
        if (!session.logged_in) return; // no session to deliver into (no replay yet)
        const size_t len = encode_soup_sequenced(response.ouch, scratch);
        if (len == 0) return;
        ++session.next_sequence_number;
        send(response.client_id, std::span{scratch}.first(len), latency);
    };

    const auto handle_note = [&](const GatewayNote &note) {
        const ClientId client_id = note.record.client_id;
        OutboundSession &session = outbound_sessions_[client_id];
        switch (note.kind) {
            case GatewayNote::Kind::LOGIN_ACCEPTED: {
                session.logged_in = true;
                const SoupLoginAccepted accepted{.session = soup_session_id(SOUP_SESSION),
                                                 .sequence_number = session.next_sequence_number};
                send(client_id, std::span{scratch}.first(encode_soup_login_accepted(accepted, scratch)));
                return;
            }
            case GatewayNote::Kind::LOGIN_REJECTED:
                send(client_id, std::span{scratch}.first(
                    encode_soup_login_rejected(static_cast<char>(note.reject_reason), scratch)));
                return;
            case GatewayNote::Kind::LOGGED_OUT:
                session.logged_in = false;
                return;
            default:
                if (const auto response = responder_.on_note(note, clock.ns_since_midnight())) {
                    send_ouch(*response);
                }
                return;
        }
    };

    const auto drain_notes = [&] {
        while (const auto note = notes_.pop()) handle_note(*note);
    };

    // SoupBinTCP: a Server Heartbeat to any logged-in client that's heard
    // nothing for over a second. Scanned at most every kHeartbeatScanNs.
    Timestamp last_scan_tsc = __rdtsc();
    const auto heartbeat = [&] {
        const Timestamp now = __rdtsc();
        if (clock.ticks_to_ns(now - last_scan_tsc) < kHeartbeatScanNs) return;
        last_scan_tsc = now;

        const size_t len = encode_soup_empty(SoupPacketType::SERVER_HEARTBEAT, scratch);
        for (ClientId client_id = 1; client_id <= MAX_CLIENTS; ++client_id) {
            const OutboundSession &session = outbound_sessions_[client_id];
            if (session.logged_in && clock.ticks_to_ns(now - session.last_sent_tsc) > kSoupHeartbeatIntervalNs) {
                send(client_id, std::span{scratch}.first(len));
            }
        }
    };

    while (!shutdown_.load()) {
        drain_notes(); // also how gateway rejects and logins go out with no engine event behind them

        while (const auto event = outbound_ring_->pop()) {
            const Timestamp t5 = __rdtsc();
            // The note for this event's order may have landed after the
            // drain above (it's always pushed before the request, so it's
            // there by now).
            drain_notes();
            const auto response = responder_.on_event(*event, clock.ns_since_midnight(event->engine_push_tsc));
            if (!response) continue;

            if (event->answers_request && event->type != EngineEventType::REJECTED) {
                const PendingLatency latency{event->read_begin_tsc, event->recv_tsc, event->server_push_tsc,
                                             event->engine_pop_tsc, event->engine_push_tsc, t5};
                send_ouch(*response, &latency);
            } else {
                send_ouch(*response);
            }
        }

        heartbeat();
        if (staged > 0) flush();
    }
}
