//
// Created by cniew on 5/24/26.
//
// Single-threaded epoll event loop driving N pipelined client connections.
//
// Each connection speaks SoupBinTCP: it logs in on connect, then sends OUCH
// in Unsequenced Data packets and reads OUCH out of Sequenced Data packets.
// It keeps PIPELINE_DEPTH Enter orders in flight (Cancels/Replaces ride
// along without a slot). buildBatch() packs a batch into write_buf and
// issues a single send() syscall.  handleReadable() drains all coalesced
// responses from one recv() and immediately refills the pipeline.
//
// epoll_ctl on the write path is avoided entirely: sockets stay armed EPOLLIN
// only; EPOLLOUT is only added if send() hits EAGAIN (rare on loopback).
//

#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "client_state.hpp"
#include "soupbintcp_proto.hpp"
#include "../config/config.hpp"

#include <atomic>
#include <random>
#include <vector>
#include <sys/epoll.h>

class LoadGenerator {
public:
    explicit LoadGenerator(int num_clients, uint64_t rng_seed = std::random_device{}());

    LoadGenerator(const LoadGenerator&) = delete;
    LoadGenerator& operator=(const LoadGenerator&) = delete;
    LoadGenerator(LoadGenerator&&) = delete;
    LoadGenerator& operator=(LoadGenerator&&) = delete;

    ~LoadGenerator();

    void run();
    void stop() { running_.store(false, std::memory_order_relaxed); }

    struct Stats {
        uint64_t requests_sent;
        uint64_t responses_ack;
        uint64_t responses_match;
        uint64_t responses_err;
        uint64_t responses_canceled;
        uint64_t responses_replaced;
        int64_t orders_in_flight;
    };
    [[nodiscard]] Stats stats() const;

private:
    int num_clients_;
    int epoll_fd_ = -1;
    uint64_t inter_arrival_ns_ = 0;  // 0 = unlimited; set from EXPECTED_THROUGHPUT
    std::vector<ClientState> clients_;
    std::atomic<bool> running_{true};
    epoll_event events_[CLIENT_EPOLL_BATCH]{};

    [[nodiscard]] bool setupEpoll();
    bool connectClient(ClientState& cs);

    void handleConnect (ClientState& cs);  // EPOLLOUT: async connect completed -> log in
    void handleReadable(ClientState& cs);  // EPOLLIN:  responses ready
    void flushWrite (ClientState& cs);  // EPOLLOUT: finish a blocked send
    void buildBatch (ClientState& cs);  // pack + send up to PIPELINE_DEPTH Enters
    void sendHeartbeat(ClientState& cs);  // SoupBinTCP Client Heartbeat
    void reconnect (ClientState& cs);  // close + reconnect after backoff

    // One SoupBinTCP packet from the server; false if the connection has to go.
    bool handleSoupPacket(ClientState& cs, const SoupPacket& packet);
    bool handleOuch(ClientState& cs, const OuchOutboundFrame& msg);
    void rememberOrder(ClientState& cs, uint32_t user_ref_num, Side side);

    void epollAdd(ClientState& cs, uint32_t events);
    void epollMod(ClientState& cs, uint32_t events);
};

#endif // ORCHESTRATOR_H
