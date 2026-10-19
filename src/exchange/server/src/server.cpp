//
// Created by charl on 1/13/2026.
//

#include "../include/server.hpp"
#include "perf.hpp"
#include "tsc_clock.hpp"

#include <tap_interface.h>
#include <rte_ip.h>

#include <immintrin.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <span>
#include <utility>

namespace {

// RTE_IPV4 over one of config.hpp's comma-separated addresses (the extra
// level of expansion splits it into RTE_IPV4's four arguments).
#define SERVER_IPV4(...) RTE_IPV4(__VA_ARGS__)

// How often the inbound thread checks connections for timeouts, and the
// outbound thread checks which are due a heartbeat.
constexpr uint64_t kScanIntervalNs = 100'000'000;

// Engine events the outbound thread takes before sending what they produced:
// replies to one client in a batch share TCP segments, and the first reply
// waits at most this many events for its send.
constexpr size_t kOutboundBatch = 32;

constexpr uint64_t kIdleTimeoutNs  = uint64_t{SOUP_IDLE_TIMEOUT_MS} * 1'000'000;
constexpr uint64_t kLoginTimeoutNs = uint64_t{SOUP_LOGIN_TIMEOUT_MS} * 1'000'000;

#if TESTING
// rte_eal_init pins the calling thread to the main lcore and every thread
// created afterwards inherits that one-CPU mask; tests don't pin to cores,
// so undo it rather than time-slice both server threads on one CPU.
void unpin_this_thread() {
    cpu_set_t all;
    CPU_ZERO(&all);
    for (int c = 0; c < CPU_SETSIZE; ++c) CPU_SET(c, &all);
    pthread_setaffinity_np(pthread_self(), sizeof(all), &all);
}
#endif

dpdk::port &bring_up_port(dpdk::runtime &runtime, const ServerNetwork &network) {
    dpdk::port &port = runtime.add_port(/*port_id=*/0, network.rx_queues, network.tx_queues);
    if (network.tap) {
        // Each tap queue pair is its own tun fd, and tun steers a flow to the
        // queue it was last written on -- which, with control and data on
        // different tx queues, reorders the flow's segments across rx queues.
        // Steer everything to one queue; every queue is still polled in case
        // the rule is refused.
        try {
            dpdk::steer_rx_to_queue(port, network.polled_rx.front());
        } catch (const std::exception &e) {
            std::cerr << "warning: steer_rx_to_queue failed (" << e.what() << "); expect reordering" << std::endl;
        }
        dpdk::configure_kernel_interface(network.tap->iface, network.tap->kernel_ipv4, network.tap->prefix_len);
    }
    port.set_ipv4_address(network.ipv4);
    return port;
}

} // namespace

//=============================================================================
// ServerNetwork
//=============================================================================

ServerNetwork ServerNetwork::nic() {
    return ServerNetwork{
        .eal = dpdk::runtime_params{.program_name = "exchange-server"},
        .rx_queues = 1,
        .tx_queues = 2,
        .polled_rx = {0},
        .control_tx = 0,
        .data_tx = 1,
        .ipv4 = SERVER_IPV4(EXCHANGE_IPV4),
        .listen_port = EXCHANGE_PORT,
        .tap = std::nullopt,
    };
}

ServerNetwork ServerNetwork::tap_interface(std::string iface) {
    std::string vdev = "net_tap0,iface=" + iface;
    return ServerNetwork{
        // No hugepages or PCI needed: the tap PMD is a kernel interface.
        .eal = dpdk::runtime_params{
            .program_name = "exchange-server",
            .legacy_mem_mb = 256,
            .no_huge = true,
            .no_pci = true,
            .vdevs = {std::move(vdev)},
        },
        // net_tap needs as many rx queues as tx queues.
        .rx_queues = 2,
        .tx_queues = 2,
        .polled_rx = {0, 1},
        .control_tx = 0,
        .data_tx = 1,
        .ipv4 = SERVER_IPV4(EXCHANGE_IPV4),
        .listen_port = EXCHANGE_PORT,
        .tap = Tap{.iface = std::move(iface), .kernel_ipv4 = SERVER_IPV4(EXCHANGE_KERNEL_IPV4), .prefix_len = 24},
    };
}

ServerNetwork ServerNetwork::from_config() {
#if EXCHANGE_NETWORK_TAP
    return tap_interface();
#else
    return nic();
#endif
}

ServerNetwork ServerNetwork::loopback_test() {
    return ServerNetwork{
        .eal = dpdk::runtime_params{
            .program_name = "exchange-server",
            .legacy_mem_mb = 128,
            .no_huge = true,
            .no_pci = true,
            .vdevs = {"net_ring0"},
        },
        .rx_queues = 3,
        .tx_queues = 3,
        .polled_rx = {0},
        .control_tx = 1,
        .data_tx = 2,
        .ipv4 = SERVER_IPV4(EXCHANGE_IPV4),
        .listen_port = EXCHANGE_PORT,
        .tap = std::nullopt,
    };
}

//=============================================================================
// Lifecycle
//=============================================================================

// dpdk::runtime/port/tcp_stack and the tcp handles have no default
// constructor, so they're built directly, in declaration order, right here.
// A constructor function-try-block only gets to log a failure; it can't
// swallow it; the exception is always rethrown once the handler exits.
Server::Server(std::shared_ptr<InboundRing> inbound_ring,
               std::shared_ptr<OutboundRing> outbound_ring,
               ServerNetwork network)
try :
    network_(std::move(network)),
    dpdk_runtime_(network_.eal),
    dpdk_port_(bring_up_port(dpdk_runtime_, network_)),
    tcp_(dpdk_port_, dpdk::tcp_params{.max_connections = kMaxConnections}),
    // Queues are claimed here, on the setup thread, before either worker exists.
    tcp_rx_(tcp_.receiver(dpdk_port_.claim_tx_queue(network_.control_tx))),
    tcp_tx_(tcp_.sender(dpdk_port_.claim_tx_queue(network_.data_tx))),
    inbound_ring_(std::move(inbound_ring)),
    outbound_ring_(std::move(outbound_ring))
{
    tcp_.listen(network_.listen_port);
    for (const uint16_t queue : network_.polled_rx) {
        rx_queues_.push_back(dpdk_port_.claim_rx_queue(queue));
    }
    dirty_.reserve(kMaxConnections);
} catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
}

Server::~Server() {
    stop();
}

void Server::run() {
    inbound_thread_ = std::jthread([this] {
#if TESTING
        unpin_this_thread();
#else
        pin_to_core(INBOUND_CORE);
#endif
        inbound_();
    });
    outbound_thread_ = std::jthread([this] {
#if TESTING
        unpin_this_thread();
#else
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

//=============================================================================
// inbound: DPDK rx -> TCP -> SoupBinTCP -> OrderEntry -> notes_ / engine
//=============================================================================

void Server::inbound_() {
    const TscClock &clock = TscClock::instance();
    Timestamp last_scan_tsc = __rdtsc();

    while (!shutdown_.load()) {
        for (const std::shared_ptr<dpdk::rx_queue> &rx_queue : rx_queues_) {
            const Timestamp read_begin_tsc = __rdtsc();
            dpdk::packet_burst burst = rx_queue->receive_burst();
            if (burst.empty()) continue;

            const InboundStamps stamps{.read_begin_tsc = read_begin_tsc, .recv_tsc = __rdtsc()};
            // Data events point into burst, which outlives the loop.
            for (const dpdk::tcp_event &event : tcp_rx_.poll(burst)) {
                on_tcp_event_(event, stamps);
            }
        }

        if (const Timestamp now = __rdtsc(); clock.ticks_to_ns(now - last_scan_tsc) >= kScanIntervalNs) {
            last_scan_tsc = now;
            check_timeouts_(now);
        }
    }
}

void Server::on_tcp_event_(const dpdk::tcp_event &event, const InboundStamps &stamps) {
    InboundConnection &connection = inbound_connections_[event.conn.slot];

    if (event.type == dpdk::tcp_event_type::accepted) {
        connection = InboundConnection{};
        connection.conn = event.conn;
        connection.open = true;
        connection.opened_tsc = connection.last_recv_tsc = stamps.recv_tsc;
        return;
    }
    if (connection.conn != event.conn) return; // not the connection this slot holds now

    switch (event.type) {
        case dpdk::tcp_event_type::data:
            if (!connection.open || connection.closing) return;
            connection.last_recv_tsc = stamps.recv_tsc;
            on_data_(connection, event.data, stamps);
            return;

        // However the connection ends -- the client's FIN (SoupBinTCP has no
        // half-close), its RST, or the close we started finishing -- it's over.
        case dpdk::tcp_event_type::peer_closed:
        case dpdk::tcp_event_type::closed:
        case dpdk::tcp_event_type::reset:
            disconnect_(connection);
            return;

        case dpdk::tcp_event_type::accepted:
            return; // handled above
    }
}

// SoupBinTCP packets don't line up with TCP segments: a segment can hold
// several, and one can be split across segments. Whole packets are parsed
// straight out of the segment; only a packet's start that a segment cut off
// is copied, into the connection's partial buffer, to be finished by the next.
void Server::on_data_(InboundConnection &connection, std::span<const std::byte> bytes, const InboundStamps &stamps) {
    const auto packet_len = [](const std::span<const std::byte> header) {
        return size_t{2} + wire::WireReader::load<uint16_t>(header.data());
    };
    const auto too_long = [&](const std::span<const std::byte> header) {
        return packet_len(header) > kMaxInboundSoupPacket;
    };

    if (connection.partial_len > 0) {
        // Finish the length field first, then the packet it announces.
        if (connection.partial_len < 2) {
            const size_t take = std::min<size_t>(2 - connection.partial_len, bytes.size());
            std::memcpy(connection.partial.data() + connection.partial_len, bytes.data(), take);
            connection.partial_len += take;
            bytes = bytes.subspan(take);
            if (connection.partial_len < 2) return;
            if (too_long(connection.partial)) {
                disconnect_(connection);
                return;
            }
        }

        const size_t total = packet_len(connection.partial);
        const size_t take = std::min(total - connection.partial_len, bytes.size());
        std::memcpy(connection.partial.data() + connection.partial_len, bytes.data(), take);
        connection.partial_len += take;
        bytes = bytes.subspan(take);
        if (connection.partial_len < total) return;

        connection.partial_len = 0;
        const SoupParseResult parsed = parse_soup_packet(std::span{connection.partial}.first(total));
        if (parsed.status != SoupParseStatus::PACKET) {
            disconnect_(connection); // a zero length
            return;
        }
        if (!handle_soup_packet_(connection, parsed.packet, stamps)) return;
    }

    while (!bytes.empty()) {
        const SoupParseResult parsed = parse_soup_packet(bytes);
        switch (parsed.status) {
            case SoupParseStatus::PACKET:
                if (!handle_soup_packet_(connection, parsed.packet, stamps)) return;
                bytes = bytes.subspan(parsed.consumed);
                continue;

            case SoupParseStatus::MALFORMED:
                disconnect_(connection); // the stream can't be resynchronized
                return;

            case SoupParseStatus::INCOMPLETE:
                if (bytes.size() >= 2 && too_long(bytes)) {
                    disconnect_(connection);
                    return;
                }
                std::memcpy(connection.partial.data(), bytes.data(), bytes.size());
                connection.partial_len = static_cast<uint16_t>(bytes.size());
                return;
        }
    }
}

bool Server::handle_soup_packet_(InboundConnection &connection, const SoupPacket &packet, const InboundStamps &stamps) {
    switch (packet.type) {
        case SoupPacketType::LOGIN_REQUEST:
            if (!connection.logged_in) login_(connection, packet.payload); // a repeat login is ignored
            break;

        case SoupPacketType::UNSEQUENCED_DATA:
            // Only allowed after Login Accepted; ignored before.
            if (connection.logged_in) {
                handle_ouch_(connection.client_id, decode_inbound_ouch(packet.payload), stamps);
            }
            break;

        case SoupPacketType::LOGOUT_REQUEST:
            disconnect_(connection); // "the server will immediately terminate the connection"
            break;

        default:
            break; // client heartbeats (any input resets the idle timer), debug, and anything else
    }
    return connection.open && !connection.closing;
}

void Server::login_(InboundConnection &connection, const std::span<const std::byte> payload) {
    const std::optional<SoupLoginRequest> login = decode_soup_login_request(payload);
    if (!login) {
        reject_login_(connection, soup_reject::kNotAuthorized);
        return;
    }

    // Blank means "the current session". The spec only says how Login
    // Accepted pads a session id, so accept the name padded either way.
    const SoupSessionId &requested = login->requested_session;
    if (requested != wire::alpha<10>("") && requested != soup_session_id(SOUP_SESSION) &&
        requested != wire::alpha<10>(SOUP_SESSION)) {
        reject_login_(connection, soup_reject::kSessionNotAvailable);
        return;
    }

    // Usernames are case-insensitive and right-padded with spaces.
    uint64_t username = 0;
    bool blank = true;
    for (char ch : login->username) {
        ch = ch == '\0' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        blank = blank && ch == ' ';
        username = (username << 8) | static_cast<uint8_t>(ch);
    }
    if (blank) {
        reject_login_(connection, soup_reject::kNotAuthorized);
        return;
    }

    ClientId client_id;
    if (const auto it = usernames_.find(username); it != usernames_.end()) {
        client_id = it->second;
    } else if (next_client_id_ <= MAX_CLIENTS) {
        client_id = next_client_id_++;
        usernames_.emplace(username, client_id);
    } else {
        reject_login_(connection, soup_reject::kNotAuthorized); // no room for another session today
        return;
    }

    InboundSession &session = inbound_sessions_[client_id];
    if (session.connected) {
        reject_login_(connection, soup_reject::kNotAuthorized); // one connection per session
        return;
    }
    // 0 asks for the next new message; anything older than the log still holds
    // can't be replayed, and the client would silently miss it.
    const uint64_t sequence = login->requested_sequence_number;
    if (sequence != 0 && sequence < retained_from_[client_id].load(std::memory_order_acquire)) {
        reject_login_(connection, soup_reject::kSessionNotAvailable);
        return;
    }

    session.connected = true;
    connection.logged_in = true;
    connection.client_id = client_id;
    push_note_(ServerNote{.kind = ServerNote::Kind::LOGIN_ACCEPTED, .reject_reason = 0, .client_id = client_id,
                          .conn = connection.conn, .sequence = sequence, .gateway = {}});
}

// The server closes the connection after Login Rejected; until it's gone,
// nothing more from it is read.
void Server::reject_login_(InboundConnection &connection, const char reason) {
    connection.closing = true;
    push_note_(ServerNote{.kind = ServerNote::Kind::LOGIN_REJECTED, .reject_reason = reason, .client_id = 0,
                          .conn = connection.conn, .sequence = 0, .gateway = {}});
}

void Server::handle_ouch_(const ClientId client_id, const OuchInboundFrame &msg, const InboundStamps &stamps) {
    if (msg.message_type == InboundOuchMessageType::NONE) {
        return; // malformed or not an inbound OUCH message
    }

    Translation translation = order_entry_.translate(client_id, msg);

    // Note before request: the outbound thread must already have an order's
    // record when the engine's event for it arrives.
    if (translation.note) {
        push_note_(ServerNote{.kind = ServerNote::Kind::GATEWAY, .reject_reason = 0, .client_id = client_id,
                              .conn = {}, .sequence = 0, .gateway = *translation.note});
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

void Server::disconnect_(InboundConnection &connection) {
    if (!connection.open) return;
    if (connection.logged_in) inbound_sessions_[connection.client_id].connected = false;
    push_note_(ServerNote{.kind = ServerNote::Kind::DISCONNECTED, .reject_reason = 0,
                          .client_id = connection.logged_in ? connection.client_id : ClientId{0},
                          .conn = connection.conn, .sequence = 0, .gateway = {}});
    connection.open = false;
    connection.logged_in = false;
    connection.closing = true;
    connection.partial_len = 0;
}

void Server::check_timeouts_(const Timestamp now) {
    const TscClock &clock = TscClock::instance();
    for (InboundConnection &connection : inbound_connections_) {
        if (!connection.open) continue;
        // Logged in: anything at all (data or heartbeats) keeps it alive. Not
        // yet: it has a while to log in, then it's dropped (as is one that was
        // turned away and never went).
        const bool expired = connection.logged_in
            ? clock.ticks_to_ns(now - std::min(now, connection.last_recv_tsc)) > kIdleTimeoutNs
            : clock.ticks_to_ns(now - std::min(now, connection.opened_tsc)) > kLoginTimeoutNs;
        if (expired) disconnect_(connection);
    }
}

// The outbound thread never blocks, so a full ring always drains.
void Server::push_note_(const ServerNote &note) {
    while (!notes_.push(note) && !shutdown_.load()) {
        // ring full; spin until there's room or we're shutting down
    }
}

//=============================================================================
// outbound: notes_ / engine -> OuchResponder -> SessionLog -> TCP -> DPDK tx
//=============================================================================

void Server::outbound_() {
    const TscClock &clock = TscClock::instance();
    Timestamp last_scan_tsc = __rdtsc();

    const auto drain_notes = [&] {
        while (const auto note = notes_.pop()) on_note_(*note);
    };

    while (!shutdown_.load()) {
        drain_notes(); // also how logins and gateway rejects go out with no engine event behind them

        for (size_t n = 0; n < kOutboundBatch; ++n) {
            const auto event = outbound_ring_->pop();
            if (!event) break;
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
                send_ouch_(*response, &latency);
            } else {
                send_ouch_(*response, nullptr);
            }
        }

        flush_();

        if (const Timestamp now = __rdtsc(); clock.ticks_to_ns(now - last_scan_tsc) >= kScanIntervalNs) {
            last_scan_tsc = now;
            heartbeat_(now);
        }
    }

    // A FIN to everyone still connected.
    for (const OutboundConnection &connection : outbound_connections_) {
        if (connection.open) close_(connection.conn);
    }
}

void Server::on_note_(const ServerNote &note) {
    switch (note.kind) {
        case ServerNote::Kind::GATEWAY:
            if (const auto response = responder_.on_note(note.gateway, TscClock::instance().ns_since_midnight())) {
                send_ouch_(*response, nullptr);
            }
            return;

        case ServerNote::Kind::LOGIN_ACCEPTED:
            accept_login_(note);
            return;

        case ServerNote::Kind::LOGIN_REJECTED: {
            OutboundConnection &connection = open_connection_(note.conn);
            std::array<std::byte, kSoupHeaderLen + 1> packet{};
            send_control_(connection, std::span{packet}.first(encode_soup_login_rejected(note.reject_reason, packet)));
            connection.close_when_flushed = true;
            return;
        }

        case ServerNote::Kind::DISCONNECTED:
            close_(note.conn);
            return;
    }
}

void Server::accept_login_(const ServerNote &note) {
    OutboundSession &session = outbound_sessions_[note.client_id];
    if (!session.log) {
        session.log = std::make_unique<SessionLog>(SOUP_REPLAY_BYTES);
        retained_from_[note.client_id].store(session.log->first_sequence(), std::memory_order_release);
    }
    const SessionLog &log = *session.log;
    OutboundConnection &connection = open_connection_(note.conn);

    // 0 -- or a number past anything sent yet -- starts with the next new message.
    const uint64_t start = note.sequence == 0 || note.sequence > log.next_sequence() ? log.next_sequence()
                                                                                       : note.sequence;
    const std::optional<uint64_t> offset = log.offset_of(start);
    if (!offset) {
        // Fell out of the log after inbound_ checked it (a narrow race: the
        // log has to wrap in between). Turn it away like inbound_ would have;
        // anything the client sent meanwhile was still entered.
        std::array<std::byte, kSoupHeaderLen + 1> packet{};
        send_control_(connection, std::span{packet}.first(
            encode_soup_login_rejected(soup_reject::kSessionNotAvailable, packet)));
        connection.close_when_flushed = true;
        return;
    }

    connection.client_id = note.client_id;
    session.slot = note.conn.slot;
    session.cursor = *offset; // replay from here, then carry on with new messages
    session.samples_count = 0;

    std::array<std::byte, kSoupHeaderLen + kSoupLoginAcceptedLen> packet{};
    const SoupLoginAccepted accepted{.session = soup_session_id(SOUP_SESSION), .sequence_number = start};
    send_control_(connection, std::span{packet}.first(encode_soup_login_accepted(accepted, packet)));
}

Server::OutboundConnection &Server::open_connection_(const dpdk::conn_id conn) {
    OutboundConnection &connection = outbound_connections_[conn.slot];
    // dirty and closed_generation describe the slot, not the connection.
    const bool dirty = connection.dirty;
    const uint32_t closed_generation = connection.closed_generation;
    connection = OutboundConnection{};
    connection.conn = conn;
    connection.open = true;
    connection.dirty = dirty;
    connection.closed_generation = closed_generation;
    connection.last_sent_tsc = __rdtsc();
    return connection;
}

void Server::send_ouch_(const OuchResponse &response, const PendingLatency *latency) {
    const size_t len = encode_soup_sequenced(response.ouch, scratch_);
    if (len == 0) return;
    send_sequenced_(response.client_id, std::span{scratch_}.first(len), latency);
}

void Server::send_control_(OutboundConnection &connection, const std::span<const std::byte> packet) {
    std::memcpy(connection.control.data(), packet.data(), packet.size());
    connection.control_len = static_cast<uint8_t>(packet.size());
    connection.control_sent = 0;
    mark_dirty_(connection);
}

void Server::send_sequenced_(const ClientId client_id, const std::span<const std::byte> packet,
                             const PendingLatency *latency) {
    OutboundSession &session = outbound_sessions_[client_id];
    if (!session.log) return; // can't happen: a session's first order follows its first login
    SessionLog &log = *session.log;

    // A connected client whose unsent backlog this would overwrite has fallen
    // a whole log behind: drop it rather than lose messages under it. It can
    // log back in and replay whatever the log still holds.
    if (session.slot && log.would_overwrite(session.cursor, packet.size())) {
        close_(outbound_connections_[*session.slot].conn);
    }

    const uint64_t first_before = log.first_sequence();
    log.append(packet);
    if (log.first_sequence() != first_before) {
        retained_from_[client_id].store(log.first_sequence(), std::memory_order_release);
    }

    if (!session.slot) return; // disconnected: it's kept for replay
    if (latency != nullptr && session.samples_count < OutboundSession::kMaxPendingSamples) {
        const size_t i = (session.samples_head + session.samples_count++) % OutboundSession::kMaxPendingSamples;
        session.samples[i] = {.end_offset = log.end_offset(), .latency = *latency};
    }
    mark_dirty_(outbound_connections_[*session.slot]);
}

void Server::mark_dirty_(OutboundConnection &connection) {
    if (connection.dirty) return;
    connection.dirty = true;
    dirty_.push_back(connection.conn.slot);
}

void Server::flush_() {
    size_t kept = 0;
    for (const uint32_t slot : dirty_) {
        OutboundConnection &connection = outbound_connections_[slot];
        if (connection.open && !flush_connection_(connection)) {
            dirty_[kept++] = slot;
        } else {
            connection.dirty = false;
        }
    }
    dirty_.resize(kept);
}

// A connection's pending control packet goes first (it's only ever queued
// at a packet boundary of the log), then its session's log from the cursor.
// send() takes what the peer's window allows; the rest waits for next time.
bool Server::flush_connection_(OutboundConnection &connection) {
    const Timestamp t6 = __rdtsc();
    bool sent_any = false;
    bool done = true;

    if (connection.control_sent < connection.control_len) {
        const size_t n = tcp_tx_.send(connection.conn, std::span{connection.control}.subspan(
            connection.control_sent, connection.control_len - connection.control_sent));
        connection.control_sent += static_cast<uint8_t>(n);
        sent_any = n > 0;
        if (connection.control_sent == connection.control_len) {
            connection.control_len = connection.control_sent = 0;
        } else {
            done = false;
        }
    }

    if (done && connection.client_id != 0) {
        OutboundSession &session = outbound_sessions_[connection.client_id];
        const SessionLog &log = *session.log;
        while (session.cursor < log.end_offset()) {
            const size_t n = tcp_tx_.send(connection.conn, log.contiguous_from(session.cursor));
            if (n == 0) break;
            session.cursor += n;
            sent_any = true;
        }
        done = session.cursor == log.end_offset();

        const Timestamp t7 = __rdtsc();
        while (session.samples_count > 0 && session.samples[session.samples_head].end_offset <= session.cursor) {
            const PendingLatency &l = session.samples[session.samples_head].latency;
            latency_.record({l.t0, l.t1, l.t2, l.t3, l.t4, l.t5, t6, t7});
            session.samples_head = (session.samples_head + 1) % OutboundSession::kMaxPendingSamples;
            --session.samples_count;
        }
    }

    if (sent_any) connection.last_sent_tsc = __rdtsc();
    if (!done) {
        // A dead connection will never take the rest; inbound_ is about to
        // report it (DISCONNECTED), and it's closed then.
        return !tcp_tx_.sendable(connection.conn);
    }
    if (connection.close_when_flushed) close_(connection.conn);
    return true;
}

// close() exactly once per accepted connection, however many times it's
// asked for: inbound_'s DISCONNECTED, a flushed Login Rejected, a slow
// consumer, shutdown.
void Server::close_(const dpdk::conn_id conn) {
    OutboundConnection &connection = outbound_connections_[conn.slot];
    if (connection.closed_generation == conn.generation) return;
    connection.closed_generation = conn.generation;
    tcp_tx_.close(conn);

    if (connection.conn != conn || !connection.open) return; // outbound_ never sent on it
    if (connection.client_id != 0) {
        OutboundSession &session = outbound_sessions_[connection.client_id];
        if (session.slot == conn.slot) {
            session.slot.reset();
            session.samples_count = 0;
        }
    }
    connection.open = false;
    connection.client_id = 0;
    connection.control_len = connection.control_sent = 0;
    connection.close_when_flushed = false;
}

// SoupBinTCP: a Server Heartbeat to any logged-in client that's been sent
// nothing for over a second. Only when it has nothing else queued -- a
// heartbeat mustn't land in the middle of a packet.
void Server::heartbeat_(const Timestamp now) {
    const TscClock &clock = TscClock::instance();
    std::array<std::byte, kSoupHeaderLen> packet{};
    encode_soup_empty(SoupPacketType::SERVER_HEARTBEAT, packet);

    for (OutboundConnection &connection : outbound_connections_) {
        if (!connection.open || connection.client_id == 0 || connection.control_len != 0) continue;
        if (clock.ticks_to_ns(now - std::min(now, connection.last_sent_tsc)) <= kSoupHeartbeatIntervalNs) continue;
        const OutboundSession &session = outbound_sessions_[connection.client_id];
        if (session.cursor != session.log->end_offset()) continue;
        send_control_(connection, packet);
    }
}
