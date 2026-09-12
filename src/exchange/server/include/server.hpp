//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include "config.hpp"

#include <runtime.h>
#include <packet_factory.hpp>
#include <ring_buffer.hpp>
#include <unordered_map>

// TODO soupbintcp

/*
The server's mental model should be:

    DPDK rx queue -> deserialize packet -> validate order/req -> matching engine rx ring
    DPDP tx queue <- serialize packet <- matching engine tx ring

Naturally, we don't have to worry about routing because the packet factory builds IP headers for us!!
However, internally we have to store packet information for each client ID

naturally we could do client ID -> addressing information
*/
class Server {
public:
    Server() = delete;
    explicit Server(std::shared_ptr<RingBuffer<OUCH>> inbound_ring,
                    std::shared_ptr<RingBuffer<OUCH>> outbound_ring);

    Server(const Server &) = delete;
    Server(Server &&) = delete;
    Server &operator=(const Server &) = delete;
    Server &operator=(Server &&) = delete;

    ~Server() = default; // for now

    void run();
private:
    struct ClientEndpoint {
        PacketEndpoint remote;
        TCPHeader tcp;
    };

    dpdk::runtime dpdk_runtime_;
    dpdk::port dpdk_port_;

    std::shared_ptr<RingBuffer<OUCH>> inbound_ring_;
    std::shared_ptr<RingBuffer<OUCH>> outbound_ring_;

    std::unordered_map<ClientId, ClientEndpoint> client_endpoints_;

    void inbound_();
    void outbound_();
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP