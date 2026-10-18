//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include <condition_variable>

#include "config.hpp"
#include "order_entry.hpp"

#include <runtime.h>
#include <packet_factory.hpp>
#include <ring_buffer.hpp>
#include <absl/container/flat_hash_map.h>
#include <atomic>

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
    friend class ServerTest;

public:
    Server() = delete;
    explicit Server(std::shared_ptr<InboundRing> inbound_ring,
                    std::shared_ptr<OutboundRing> outbound_ring);

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

    std::shared_ptr<InboundRing> inbound_ring_;
    std::shared_ptr<OutboundRing> outbound_ring_;

    // inbound_ -> outbound_ side channel; see GatewayNote.
    RingBuffer<GatewayNote> notes_{COMMUNICATION_RING_COUNT};

    // Inbound-thread only.
    OrderEntry order_entry_{validation::PriceBand{.min = MIN_PRICE, .max = MAX_PRICE}};

    absl::flat_hash_map<ClientId, ClientEndpoint> client_endpoints_;

    // (src ip << 16 | src port) -> ClientId, assigned the first time a given
    // endpoint is ever seen. There's no kernel TCP stack terminating
    // connections here (packets are handled directly off the DPDK rx
    // queue -- see server.cpp), so this endpoint pair is the closest thing
    // to "session identity" this server has to key a client on.
    absl::flat_hash_map<uint64_t, ClientId> client_ids_;
    ClientId next_client_id_ = 1; // 0 reserved as "no client"

    ClientId resolve_client_id_(const PacketEndpoint &src, const TCPHeader &tcp);

    std::atomic<bool> shutdown_;

    void inbound_();
    void outbound_();
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP