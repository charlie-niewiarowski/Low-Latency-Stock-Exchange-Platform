//
// Created by charl on 1/13/2026.
//

#include "../include/server.hpp"

#include <iostream>
#include <thread>
#include <utility>

// dpdk::runtime/dpdk::port have no default constructor, so they can't be
// left blank in the mem-initializer list and assigned in the body — they
// have to be constructed directly, in declaration order, right here.
// A constructor function-try-block only gets to log a failure; it can't
// swallow it; the exception is always rethrown once the handler exits,
// since a Server whose dpdk_runtime_/dpdk_port_ failed to construct can't
// be handed back as a valid object.
Server::Server(std::shared_ptr<RingBuffer<OUCH>> inbound_ring,
               std::shared_ptr<RingBuffer<OUCH>> outbound_ring)
try :
    dpdk_runtime_(dpdk::runtime_params{.program_name = "exchange-server"}),
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

void Server::inbound_() {
    for (;;) {
        dpdk::packet_burst burst = dpdk_port_.receive_burst(/*queue_id=*/0);

        if (burst.empty()) {
            continue;
        }

        for (const dpdk::packet& packet : burst) {
            std::cout << "Received packet with length: " << packet.length() << "\n";
        }
    }
}

void Server::outbound_() {
    for (;;) {

    }
}


