//
// Created by charl on 1/13/2026.
//

#include "../include/server.hpp"
#include "perf.hpp"
#include "tsc_clock.hpp"

#include <immintrin.h>
#include <iostream>
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
// inbound: DPDK rx -> OrderEntry -> notes_ / engine
//=============================================================================

void Server::inbound_() {
    while (!shutdown_.load()) {
        const Timestamp read_begin_tsc = __rdtsc();
        dpdk::packet_burst burst = rx_queue_->receive_burst();

        if (burst.empty()) {
            continue;
        }

        const Timestamp recv_tsc = __rdtsc();

        for (const dpdk::packet &packet : burst) {
            const auto deserialized =
                packet_factory<TCPHeader, OuchInboundFrame>::deserialize_packet(packet);

            // deserialize_packet zeroes src.ip (and leaves message_type at
            // its NONE default) on any parse failure -- too short, not
            // IPv4, IPv4 options present, or the IP protocol didn't match
            // TCP. Nothing downstream can trust an unparsed packet.
            if (deserialized.src.ip == 0 ||
                deserialized.payload.message_type == InboundOuchMessageType::NONE) {
                continue;
            }

            // Session stub: one TCP segment carries exactly one OUCH
            // message and the session is the sender's (ip, port). Everything
            // from here on only sees (ClientId, message), so a real
            // SoupBinTCP layer can replace this without touching it.
            const ClientId client_id =
                resolve_client_id_(deserialized.src, deserialized.dst, deserialized.transport);
            if (client_id == 0) {
                continue; // session table full
            }

            Translation translation = order_entry_.translate(client_id, deserialized.payload);

            // Note before request: the outbound thread must already have an
            // order's record when the engine's event for it arrives.
            if (translation.note) {
                while (!notes_.push(*translation.note) && !shutdown_.load()) {
                    // ring full; spin until there's room or we're shutting down
                }
            }
            if (translation.request) {
                EngineRequest &request = *translation.request;
                request.read_begin_tsc  = read_begin_tsc;
                request.recv_tsc        = recv_tsc;
                request.server_push_tsc = __rdtsc();
                while (!inbound_ring_->push(request) && !shutdown_.load()) {
                    // ring full; spin until there's room or we're shutting down
                }
            }
        }
    }
}

//=============================================================================
// outbound: notes_ / engine -> OuchResponder -> DPDK tx
//=============================================================================

void Server::outbound_() {
    const std::shared_ptr<dpdk::packet_pool> pool = dpdk_port_.get_pool();
    const TscClock &clock = TscClock::instance();

    std::array<dpdk::packet, kTxBurst> burst;
    size_t staged = 0;

    // Sends everything staged, retrying whatever the NIC didn't take. Only
    // shutdown can leave packets unsent; they're dropped (freed) then.
    const auto flush = [&] {
        std::span<dpdk::packet> pending{burst.data(), staged};
        while (!pending.empty() && !shutdown_.load()) {
            pending = pending.subspan(tx_queue_->send_burst(pending));
        }
        for (dpdk::packet &packet : std::span{burst.data(), staged}) {
            packet = dpdk::packet{};
        }
        staged = 0;
    };

    const auto stage = [&](const OuchResponse &response) {
        const ClientEndpoint &endpoint = endpoints_[response.client_id];
        const TCPHeader tcp{.src_port = endpoint.local_port, .dst_port = endpoint.remote_port,
                            .flags = RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG};
        const auto build = [&] {
            return packet_factory<TCPHeader, OuchOutboundFrame>::build_packet(
                *pool, endpoint.local, endpoint.remote, tcp, response.ouch);
        };

        dpdk::packet packet = build();
        // An empty pool means mbufs are still out with the NIC: hand it what's
        // staged (freeing those once sent) and try again rather than drop an ack.
        while (!packet && !shutdown_.load()) {
            flush();
            packet = build();
        }
        if (!packet) return;

        burst[staged++] = std::move(packet);
        if (staged == burst.size()) flush();
    };

    const auto drain_notes = [&] {
        while (const auto note = notes_.pop()) {
            if (const auto response = responder_.on_note(*note, clock.ns_since_midnight())) {
                stage(*response);
            }
        }
    };

    while (!shutdown_.load()) {
        drain_notes(); // also how gateway rejects go out with no engine event behind them

        while (const auto event = outbound_ring_->pop()) {
            // The note for this event's order may have landed after the
            // drain above (it's always pushed before the request, so it's
            // there by now).
            drain_notes();
            if (const auto response = responder_.on_event(*event, clock.ns_since_midnight(event->engine_push_tsc))) {
                stage(*response);
            }
        }

        if (staged > 0) flush();
    }
}
