//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include "config.hpp"
#include "order_entry.hpp"
#include "ouch_responder.hpp"
#include "soupbintcp_proto.hpp"

#include <runtime.h>
#include <packet_factory.hpp>
#include <ring_buffer.hpp>
#include <absl/container/flat_hash_map.h>

#include <array>
#include <atomic>
#include <memory>
#include <thread>

/*
The server's mental model:

    DPDK rx queue -> SoupBinTCP -> decode OUCH -> OrderEntry (validate, translate) -> engine inbound ring
                                         |
                                         +-> notes_ (records, gateway rejects)
                                                 |
    DPDK tx queue <- SoupBinTCP <- encode OUCH <- OuchResponder <- engine outbound ring

The SoupBinTCP layer is a stub until the DPDK lib terminates TCP: each TCP
segment is taken to hold whole SoupBinTCP packets (any number of them, but
none split across segments), a session is the sender's (ip, port), there
is no retransmission or re-login replay, and client heartbeat timeouts
aren't enforced. Login, logout, Sequenced/Unsequenced Data and server
heartbeats are real.

Two threads, each owning one half: inbound_ owns OrderEntry and the
session table, outbound_ owns OuchResponder and the tx queue. The only
state they share is the rings and endpoints_, which inbound_ writes once
per client before anything about that client reaches outbound_.

Routing is free -- packet_factory builds the Ethernet/IP headers -- so the
only per-client addressing kept is the endpoint pair a client's first
packet arrived on.
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

    ~Server();

    // Starts the inbound and outbound threads and returns; stop() (or the
    // destructor) ends and joins them.
    void run();
    void stop();

private:
    // Where a client's packets come from and go back to: remote is the
    // client, local is the address/port it reached.
    struct ClientEndpoint {
        PacketEndpoint remote;
        PacketEndpoint local;
        uint16_t       remote_port;
        uint16_t       local_port;
    };

    dpdk::runtime dpdk_runtime_;
    dpdk::port dpdk_port_;
    std::shared_ptr<dpdk::rx_queue> rx_queue_;
    std::shared_ptr<dpdk::tx_queue> tx_queue_;
#if TESTING
    // See the queue-claiming comment in the constructor.
    std::shared_ptr<dpdk::tx_queue> test_client_tx_queue_;
#endif

    std::shared_ptr<InboundRing> inbound_ring_;
    std::shared_ptr<OutboundRing> outbound_ring_;

    // inbound_ -> outbound_ side channel; see GatewayNote.
    RingBuffer<GatewayNote> notes_{COMMUNICATION_RING_COUNT};

    // Inbound-thread only.
    OrderEntry order_entry_{validation::PriceBand{.min = MIN_PRICE, .max = MAX_PRICE}};
    // Outbound-thread only.
    OuchResponder responder_;

    // Indexed by ClientId. Written by inbound_ exactly once per client, on
    // its first packet, and before anything from that client is pushed to
    // notes_/the engine -- so the release/acquire on those rings is what
    // makes an entry visible to outbound_ by the time it's needed.
    std::array<ClientEndpoint, MAX_CLIENTS + 1> endpoints_{};

    // (src ip << 16 | src port) -> ClientId, assigned the first time a given
    // endpoint is ever seen. There's no TCP stack terminating connections
    // here yet (packets are handled directly off the DPDK rx queue -- see
    // server.cpp), so this endpoint pair is the closest thing to "session
    // identity" this server has to key a client on. Inbound-thread only.
    absl::flat_hash_map<uint64_t, ClientId> client_ids_;
    ClientId next_client_id_ = 1; // 0 reserved as "no client"

    // 0 if this is a new endpoint and MAX_CLIENTS are already taken.
    ClientId resolve_client_id_(const PacketEndpoint &src, const PacketEndpoint &dst, const TCPHeader &tcp);

    // SoupBinTCP session state, split by which thread owns it.
    struct InboundSession {
        bool logged_in = false;
    };
    struct OutboundSession {
        bool      logged_in = false;
        uint64_t  next_sequence_number = 1; // of the next Sequenced Data packet
        Timestamp last_sent_tsc = 0;        // for server heartbeats
    };
    std::array<InboundSession, MAX_CLIENTS + 1> inbound_sessions_{};   // inbound-thread only
    std::array<OutboundSession, MAX_CLIENTS + 1> outbound_sessions_{}; // outbound-thread only

    struct InboundStamps {
        Timestamp read_begin_tsc;
        Timestamp recv_tsc;
    };
    void handle_soup_packet_(ClientId client_id, const SoupPacket &packet, const InboundStamps &stamps);
    void handle_ouch_(ClientId client_id, const OuchInboundFrame &msg, const InboundStamps &stamps);
    void push_note_(const GatewayNote &note);

    std::atomic<bool> shutdown_{false};
    std::jthread inbound_thread_;
    std::jthread outbound_thread_;

    void inbound_();
    void outbound_();
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP
