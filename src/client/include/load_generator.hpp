//
// Created by cniew on 5/24/26.
//
// Single-threaded epoll event loop driving N pipelined client connections.
//
// Each connection keeps PIPELINE_DEPTH requests in-flight simultaneously.
// buildBatch() packs PIPELINE_DEPTH frames into write_buf and issues a single
// send() syscall.  handleReadable() drains all coalesced responses from one
// recv() and immediately refills the pipeline.
//
// epoll_ctl on the write path is avoided entirely: sockets stay armed EPOLLIN
// only; EPOLLOUT is only added if send() hits EAGAIN (rare on loopback).
//

#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include "client_state.hpp"
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

    void handleConnect (ClientState& cs);  // EPOLLOUT: async connect completed
    void handleReadable(ClientState& cs);  // EPOLLIN:  responses ready
    void flushWrite (ClientState& cs);  // EPOLLOUT: finish a blocked send
    void buildBatch (ClientState& cs);  // pack + send PIPELINE_DEPTH frames
    void reconnect (ClientState& cs);  // close + reconnect immediately

    void epollAdd(ClientState& cs, uint32_t events);
    void epollMod(ClientState& cs, uint32_t events);
};

#endif // ORCHESTRATOR_H
