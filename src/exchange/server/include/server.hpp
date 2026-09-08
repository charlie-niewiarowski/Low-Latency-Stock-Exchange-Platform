//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

 #include "connection_info.hpp"
#include "communication_types.hpp"
#include "config.hpp"
#include "ring_buffer.hpp"

#include <array>
#include <atomic>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <sys/epoll.h>

// Capacity of the server-level error channel (SPSC: inbound → outbound).
// Sized to absorb a full read batch of errors from every connected client.
inline constexpr int ERROR_CHANNEL_SIZE = MAX_CLIENTS * PIPELINE_DEPTH;

class Server {
public:
    Server(InboundRing& in, OutboundRing& out, std::atomic<bool>& stop);
    Server() = delete;

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    ~Server();

    void run();

private:
    //===== threads =====
    std::jthread inbound_thread_;
    std::jthread outbound_thread_;

    //===== connection state =====
    std::array<std::optional<InboundState>,  MAX_CLIENTS + 10> inbound_map_{};
    std::array<std::optional<OutboundState>, MAX_CLIENTS + 10> outbound_map_{};

    std::unordered_map<ClientId, std::atomic<int>> client_map_;

    // Two-phase close flags.
    //   condemned_inbound_[fd]  : inbound sets (release) when it is done with fd.
    //                             outbound scans (acquire) in handleDisconnectsOutbound.
    //   condemned_outbound_[fd] : outbound sets (release) when it is done with fd.
    //                             inbound scans (acquire) in handleDisconnectsInbound.
    // inbound_map_[fd].reset() happens only after condemned_outbound_[fd] is seen,
    // guaranteeing all outbound accesses to outbound_map_[fd] have completed.
    std::array<std::atomic<bool>, MAX_CLIENTS + 10> condemned_inbound_{};
    std::array<std::atomic<bool>, MAX_CLIENTS + 10> condemned_outbound_{};

    //===== intermodule rings =====
    InboundRing&  in_ring_;
    OutboundRing& out_ring_;

    // SPSC error channel: inbound pushes validation/IO errors, outbound drains.
    RingBuffer<PendingError> error_channel_{ERROR_CHANNEL_SIZE};

    //===== epoll =====
    int epoll_in_  = -1;
    epoll_event events_in_[MAX_CLIENTS]{};

    int epoll_out_ = -1;
    epoll_event events_out_[MAX_CLIENTS]{};

    //===== outbound-private state =====
    std::unordered_set<int> writable_fds_;
    std::array<bool, MAX_CLIENTS + 10> epollout_armed_{};

    //===== stop flag =====
    std::atomic<bool>& stop_;

    //===== setup =====
    bool setupEpoll();
    [[nodiscard]] std::optional<EpollSocket> setupListenSocket() const;

    //===== handleRequests (inbound thread) =====
    void handleRequests();
    void registerConnections(int listen_fd);
    void readAndProcessBytes(int client_fd);
    // Returns true if a disconnect was detected (condemned_inbound_ set).
    bool readRequest(InboundState& state);
    void processRequest(InboundState& state);

    //===== serveResponses (outbound thread) =====
    void serveResponses();
    void routeOutboundMessages();
    void beginSends();
    static void appendResponse(OutboundState& out);
    // Returns true if a fatal send error occurred.  The caller is responsible
    // for condemning the fd; sendResponse never calls condemnConnection itself.
    bool sendResponse(int fd, OutboundState& out);
    static void finishBatch(OutboundState& out, Timestamp t6);

    //===== disconnect handling =====
    // Two-phase close: each thread cleans up its own state and signals the other.
    // inbound scans condemned_outbound_ (acquire); when set, resets inbound_map_[fd].
    // outbound scans condemned_inbound_ (acquire); when set, resets outbound_map_[fd]
    //   and sets condemned_outbound_ (release) to unblock inbound's final teardown.
    void handleDisconnectsInbound();
    void handleDisconnectsOutbound();

    // Called when a thread detects a fatal condition on fd.
    // caller_inbound=true : removes fd from epoll_in_, sets condemned_inbound_ (release).
    // caller_inbound=false: removes fd from epoll_out_, resets outbound_map_[fd],
    //                       sets condemned_outbound_ (release).
    void condemnConnection(int fd, bool caller_inbound);
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP