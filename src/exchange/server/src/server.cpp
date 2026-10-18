//
// Created by charl on 1/13/2026.
//

#include "../include/server.hpp"

#include <immintrin.h>
#include <iostream>
#include <thread>
#include <utility>

namespace {

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
    dpdk_port_(std::move(dpdk_runtime_.add_port(/*port_id=*/0, /*n_rx_queues=*/1, /*n_tx_queues=*/1))),
    inbound_ring_(std::move(inbound_ring)),
    outbound_ring_(std::move(outbound_ring))
{
} catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
}

void Server::run() {
    std::jthread inbound_thread(&Server::inbound_, this);

    std::jthread outbound_thread(&Server::outbound_, this);
}

// (src ip << 16 | src port) as a lookup key -- see the client_ids_ comment
// in server.hpp for why this stands in for a real session identity here.
namespace {
uint64_t endpoint_key(const PacketEndpoint &src, const TCPHeader &tcp) {
    return (static_cast<uint64_t>(src.ip) << 16) | tcp.src_port;
}
} // namespace

ClientId Server::resolve_client_id_(const PacketEndpoint &src, const TCPHeader &tcp) {
    const auto [it, inserted] = client_ids_.try_emplace(endpoint_key(src, tcp), next_client_id_);
    if (inserted) {
        ++next_client_id_;
    }
    return it->second;
}

void Server::inbound_() {
    const auto rx_queue = dpdk_port_.claim_rx_queue();

    while (!shutdown_.load()) {
        const Timestamp read_begin_tsc = __rdtsc();
        dpdk::packet_burst burst = rx_queue->receive_burst();

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
            const ClientId client_id = resolve_client_id_(deserialized.src, deserialized.transport);
            client_endpoints_[client_id] = ClientEndpoint{deserialized.src, deserialized.transport};

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

void Server::outbound_() {
    // TODO: keep OrderRecords from notes_, turn each EngineEvent into OUCH
    // and send it (plus gateway rejects) to client_endpoints_[client_id]
    // via dpdk_port_'s tx queue. Until then both rings are drained and
    // discarded so neither producer stalls.
    while (!shutdown_.load()) {
        while (const auto note = notes_.pop()) {
        }
        while (const auto event = outbound_ring_->pop()) {
        }
    }
}


