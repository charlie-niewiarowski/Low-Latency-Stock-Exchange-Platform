//
// Created by charl on 1/13/2026.
//

#include "../include/server.hpp"
#include "../include/latency.hpp"
#include "../include/validation.hpp"
#include "lib.hpp"

#include <cstring>
#include <immintrin.h>
#include <sys/socket.h>
#include <netdb.h>
#include <iostream>
#include <fcntl.h>

static LatencyHandler latency_handler;

#define ever (;;)

//=============================================================================
// Forward-Declared Helpers
//=============================================================================

static int epoll_ctrl(int epoll_fd, int fd, int op, int target);

//=============================================================================
// Special Member Functions
//=============================================================================

Server::Server(InboundRing& in, OutboundRing& out, std::atomic<bool>& stop)
    : in_ring_(in), out_ring_(out), stop_(stop) {
    client_map_.reserve(256);
}

Server::~Server() {
    if (epoll_in_  != -1) close(epoll_in_);
    if (epoll_out_ != -1) close(epoll_out_);
}

//=============================================================================
// run()
//=============================================================================

void Server::run() {
    if (!setupEpoll()) return;

    inbound_thread_ = std::jthread([&]() {
        #if DIAGNOSTICS
        {
            std::cerr << "inbound thread: " << gettid() << "\n";
        }
        #endif

        pin_to_core(INBOUND_CORE);
        handleRequests();
    });

    outbound_thread_ = std::jthread([&]() {
        #if DIAGNOSTICS
        {
            std::cerr << "outbound thread: " << gettid() << "\n";
        }
        #endif

        pin_to_core(OUTBOUND_CORE);
        serveResponses();
    });
}

//=============================================================================
// Setup
//=============================================================================

bool Server::setupEpoll() {
    epoll_in_  = epoll_create(MAX_CLIENTS);
    epoll_out_ = epoll_create(MAX_CLIENTS);

    if (epoll_in_ == -1 || epoll_out_ == -1) {
        std::cerr << "epoll_create error: " << strerror(errno) << std::endl;
        return false;
    }

    return true;
}

std::optional<EpollSocket> Server::setupListenSocket() const {
    struct addrinfo hints = {}, *result;

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (const int s = getaddrinfo(nullptr, PORT, &hints, &result); s != 0) {
        std::cerr << "getaddrinfo error: " << gai_strerror(s) << std::endl;
        return std::nullopt;
    }

    const int raw_fd = socket(result->ai_family, result->ai_socktype, 0);
    if (raw_fd == -1) {
        std::cerr << "socket() error: " << strerror(errno) << std::endl;
        freeaddrinfo(result);
        return std::nullopt;
    }

    EpollSocket listen_sock{raw_fd, epoll_in_};

    constexpr int opt = 1;
    setsockopt(raw_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(raw_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    if (bind(raw_fd, result->ai_addr, result->ai_addrlen) == -1) {
        std::cerr << "bind() error: " << std::strerror(errno) << std::endl;
        freeaddrinfo(result);
        return std::nullopt;
    }

    freeaddrinfo(result);

    if (listen(raw_fd, 32) != 0) {
        std::cerr << "listen() error: " << strerror(errno) << std::endl;
        return std::nullopt;
    }

    const int flags = fcntl(raw_fd, F_GETFL, 0);
    if (flags == -1) return std::nullopt;
    fcntl(raw_fd, F_SETFL, flags | O_NONBLOCK);

    // Level-triggered (NOT edge-triggered): the accept queue must be re-checked
    // whenever it is non-empty. With EPOLLET a single missed edge (e.g. the queue
    // never drains to empty) silences the listen socket permanently, refusing all
    // further connections once the backlog fills.
    {
        epoll_event lev{};
        lev.events  = EPOLLIN;
        lev.data.fd = raw_fd;
        if (epoll_ctl(epoll_in_, EPOLL_CTL_ADD, raw_fd, &lev) == -1) {
            std::cerr << "epoll_ctl (listen) error: " << strerror(errno) << std::endl;
            return std::nullopt;
        }
    }

    return listen_sock;
}

//=============================================================================
// handleRequests  (inbound thread)
//=============================================================================

void Server::handleRequests() {
    auto socket_setup{setupListenSocket()};
    if (!socket_setup.has_value()) {
        stop_.store(true, std::memory_order_relaxed);
        return;
    }
    const EpollSocket listen_sock{std::move(socket_setup.value())};

    while (!stop_.load(std::memory_order_relaxed)) {
        handleDisconnectsInbound();

        const int nfds = epoll_wait(epoll_in_, events_in_, MAX_CLIENTS, 0);

        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            stop_.store(true, std::memory_order_relaxed);
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            // Warm the next event's connection state (a ~2.3 KB fd-indexed object)
            // while we read/parse the current fd.
            if (i + 1 < nfds) prefetch_read(&inbound_map_[events_in_[i + 1].data.fd]);

            const int fd = events_in_[i].data.fd;

            if (fd == listen_sock.get()) {
                registerConnections(listen_sock.get());
            }
            else if (events_in_[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                epoll_ctrl(epoll_in_, fd, EPOLL_CTL_DEL, 0);
                condemned_inbound_[fd].store(true, std::memory_order_release);
            }
            else {
                readAndProcessBytes(fd);
            }
        }
    }
}

void Server::registerConnections(const int listen_fd) {
    int client_fd{-1};
    while ((client_fd = accept(listen_fd, nullptr, nullptr)) != -1) {
        const int flags{fcntl(client_fd, F_GETFL, 0)};
        if (flags == -1 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            close(client_fd);
            continue;  // not return — keep accepting other clients
        }

        EpollSocket client_sock{client_fd, epoll_in_};

        if (epoll_ctrl(epoll_in_, client_fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLRDHUP) == -1) {
            return;
        }

        // Clear condemned flags before publishing state so outbound never sees a
        // stale condemned flag for a freshly accepted fd.
        condemned_inbound_[client_fd].store(false, std::memory_order_relaxed);
        condemned_outbound_[client_fd].store(false, std::memory_order_relaxed);

        // outbound_map_ must be emplaced before inbound_map_ so that, when the
        // first message flows through the engine rings and outbound routes it, the
        // slot is guaranteed to be live.
        outbound_map_[client_fd].emplace();
        inbound_map_[client_fd].emplace(std::move(client_sock));
    }
}

void Server::readAndProcessBytes(const int client_fd) {
    // Skip if inbound already condemned this fd (epoll may still fire for it
    // on the current batch before the EPOLL_CTL_DEL takes effect).
    if (condemned_inbound_[client_fd].load(std::memory_order_relaxed)) return;
    if (condemned_outbound_[client_fd].load(std::memory_order_relaxed)) return;

    auto& istate = inbound_map_[client_fd];
    if (!istate.has_value()) return;

    // loop until socket drained or error
    while (readRequest(*istate));
}

bool Server::readRequest(InboundState& state) {
    const int client_fd{state.fd()};
    ReadBuffer& buf = state.read_buffer();

    state.set_read_begin_tsc(__rdtsc()); // t0: always — before recv syscall

    while (buf.remaining_capacity() > 0) {
        if (buf.read_from(client_fd) <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            condemnConnection(client_fd, true);
            return false;
        }
    }

    processRequest(state);
    // if the remaining capacity is > 0 then the socket was drained
    // and don't we want this function to be called again
    return buf.remaining_capacity() <= 0;
}

void Server::processRequest(InboundState& state) {
    ReadBuffer& buf = state.read_buffer();
    const int fd = state.fd();

    while (buf.length() >= INBOUND_BSIZE) {
#if DIAGNOSTICS
        const Timestamp recv_tsc = __rdtsc(); // t1: after recv syscall
#endif

        const char* data = buf.view().data();
        constexpr size_t header_len = sizeof("EXCHANGE\n") - 1;
        if (memcmp(data, "EXCHANGE\n", header_len) != 0) {
            error_channel_.push({fd, ServerError::MALFORMED_REQUEST});
            buf.advance(INBOUND_BSIZE);
            continue;
        }

        InboundMessage msg{};
        memcpy(&msg, data + header_len, sizeof(InboundMessage));
        msg.read_begin_tsc = state.read_begin_tsc(); // t0: always
#if DIAGNOSTICS
        msg.recv_tsc = recv_tsc; // t1
#endif
        buf.advance(INBOUND_BSIZE);

        if (!validate_message(msg)) {
            error_channel_.push({fd, ServerError::INVALID_ORDER});
            continue;
        }

#if DIAGNOSTICS
        msg.server_push = __rdtsc(); // t2
#endif
        state.set_inbound(msg);

        const ClientId cid = msg.client_id;
        auto [it, inserted] = client_map_.try_emplace(cid, fd);
        if (!inserted) it->second.store(fd, std::memory_order_relaxed);

        in_ring_.push(state.inbound());
    }
}

//=============================================================================
// serveResponses  (outbound thread)
//=============================================================================

void Server::serveResponses() {
    while (!stop_.load(std::memory_order_relaxed)) {
        handleDisconnectsOutbound();
        routeOutboundMessages();
        beginSends();

        const int nfds = epoll_wait(epoll_out_, events_out_, MAX_CLIENTS, 0);

        if (nfds == -1 && errno != EINTR) {
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            // Warm the next event's outbound state (several KB) while we send this one.
            if (i + 1 < nfds) prefetch_read(&outbound_map_[events_out_[i + 1].data.fd]);

            const int fd = events_out_[i].data.fd;

            if (condemned_inbound_[fd].load(std::memory_order_relaxed)) continue;

            if (events_out_[i].events & (EPOLLRDHUP | EPOLLHUP)) {
                if (writable_fds_.contains(fd)) writable_fds_.erase(fd);
                condemnConnection(fd, false);
            }
            else {
                auto& ostate = outbound_map_[fd];
                if (!ostate.has_value()) continue;

                OutboundState& out = *ostate;
                if (!sendResponse(fd, out)) {
                    condemnConnection(fd, false);
                } else if (epollout_armed_[fd] && out.write_buffer().remaining_to_send() == 0) {
                    epoll_ctrl(epoll_out_, fd, EPOLL_CTL_DEL, EPOLLIN | EPOLLOUT);
                    epollout_armed_[fd] = false;
                }
            }
        }
    }
}

void Server::routeOutboundMessages() {
    // Resolve a message's destination fd (or -1). One client_map_ lookup per msg.
    auto resolve = [this](const OutboundMessage& m) -> int {
        const auto it = client_map_.find(m.client_id);
        if (it == client_map_.end()) return -1;
        const int fd = it->second.load(std::memory_order_relaxed);
        return fd > 0 ? fd : -1;
    };

    // 1-ahead pipeline: while routing the current message, prefetch the next
    // destination's OutboundState (a multi-KB fd-indexed object). The overlap
    // window is the next message's client_map_ lookup.
    OutboundMessage cur{}, nxt{};
    bool have = out_ring_.pop(cur);
    int cur_fd = have ? resolve(cur) : -1;
    if (cur_fd > 0) prefetch_read(&outbound_map_[cur_fd]);

    while (have) {
        have = out_ring_.pop(nxt);
        int nxt_fd = -1;
        if (have) {
            nxt_fd = resolve(nxt);
            if (nxt_fd > 0) prefetch_read(&outbound_map_[nxt_fd]);
        }

        if (cur_fd > 0 && !condemned_inbound_[cur_fd].load(std::memory_order_relaxed)) {
            auto& ostate = outbound_map_[cur_fd];
            if (ostate.has_value()) {
                ostate->push_outbound(cur);
                writable_fds_.insert(cur_fd);
            }
        }

        cur = nxt;
        cur_fd = nxt_fd;
    }

    // Drain inbound-generated errors (SPSC: inbound produces, we consume).
    PendingError pe{};
    while (error_channel_.pop(pe)) {
        const int fd = pe.fd;
        if (condemned_inbound_[fd].load(std::memory_order_relaxed)) continue;

        auto& ostate = outbound_map_[fd];
        if (!ostate.has_value()) continue;

        ostate->push_outbound(OutboundMessage{
            .status       = Status::FAILURE,
            .server_error = pe.err
        });
        writable_fds_.insert(fd);
    }
}

void Server::beginSends() {
    for (auto it = writable_fds_.begin(); it != writable_fds_.end(); ) {
        // Warm the next writable fd's outbound state while we append/send this one.
        auto nxt = it; ++nxt;
        if (nxt != writable_fds_.end()) prefetch_read(&outbound_map_[*nxt]);

        const int fd = *it;

        if (condemned_inbound_[fd].load(std::memory_order_relaxed)) {
            it = writable_fds_.erase(it);
            continue;
        }

        auto& ostate = outbound_map_[fd];
        if (!ostate.has_value()) {
            it = writable_fds_.erase(it);
            continue;
        }

        OutboundState& out  = *ostate;
        WriteBuffer& wbuf = out.write_buffer();

        while (out.has_pending_outbound() && wbuf.has_room(OUTBOUND_BSIZE)) {
            appendResponse(out);
        }

        if (wbuf.remaining_to_send() > 0) {
            if (!sendResponse(fd, out)) {
                // Fatal send error: erase via iterator before condemning so
                // condemnConnection's writable_fds_.erase(fd) is a harmless no-op.
                it = writable_fds_.erase(it);
                condemnConnection(fd, false);
                continue;
            }
        }

        if (condemned_inbound_[fd].load(std::memory_order_relaxed)) {
            it = writable_fds_.erase(it);
        } else if (wbuf.remaining_to_send() == 0 && !out.has_pending_outbound()) {
            it = writable_fds_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::appendResponse(OutboundState& out) {
    const OutboundMessage msg = out.pop_outbound();
#if DIAGNOSTICS
    const Timestamp t5 = __rdtsc(); // t5: server popped outbound ring
#endif

    char frame[OUTBOUND_BSIZE]{};

    if (msg.message_type == MessageType::MATCH) {
        memcpy(frame, MATCH_STATUS, strlen(MATCH_STATUS));
        memcpy(frame + strlen(MATCH_STATUS),                       &msg.client_id, sizeof(ClientId));
        memcpy(frame + strlen(MATCH_STATUS) + sizeof(ClientId),    &msg.order_id,  sizeof(OrderId));
    }
    else if (msg.server_error == ServerError::NONE) {
        memcpy(frame, OK_STATUS, strlen(OK_STATUS));
        memcpy(frame + strlen(OK_STATUS),                    &msg.client_id, sizeof(ClientId));
        memcpy(frame + strlen(OK_STATUS) + sizeof(ClientId), &msg.order_id,  sizeof(OrderId));
    }
    else {
        memcpy(frame, ERROR_STATUS, strlen(ERROR_STATUS));
        const std::string_view err_str{server_error_string(msg.server_error)};
        memcpy(frame + strlen(ERROR_STATUS), err_str.data(), err_str.size());
        frame[strlen(ERROR_STATUS) + err_str.size()] = '\n';
    }

    out.write_buffer().append(frame, OUTBOUND_BSIZE);

    const bool record = (msg.server_error == ServerError::NONE &&
                         msg.message_type != MessageType::MATCH);
    PendingLatency e{};
    e.read_begin_tsc = msg.read_begin_tsc;
#if DIAGNOSTICS
    e.recv_tsc        = msg.recv_tsc;
    e.server_push_tsc = msg.server_push_tsc;
    e.engine_pop_tsc  = msg.engine_pop_tsc;
    e.engine_push_tsc = msg.engine_push_tsc;
    e.server_pop      = t5;
#endif
    e.record          = record;
    out.push_latency(e);
}

bool Server::sendResponse(const int fd, OutboundState& out) {
    WriteBuffer& buf = out.write_buffer();

    Timestamp t6 = 0;
#if DIAGNOSTICS
    t6 = __rdtsc(); // t6: before send syscall
#endif

    while (buf.remaining_to_send() > 0) {
        if (buf.write_to(fd) <= 0) {
            // EAGAIN/EWOULDBLOCK is not fatal: the socket send buffer is full
            // (slow client). Arm EPOLLOUT and report success so the caller keeps
            // the connection and retries when the socket is writable again.
            // Only a genuine send error should condemn the connection.
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!epollout_armed_[fd])
                    epollout_armed_[fd] = (epoll_ctrl(epoll_out_, fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLOUT) == 0);
                return true;
            }
            return false;
        }
    }

    finishBatch(out, t6);
    return true;
}

void Server::finishBatch(OutboundState& out, const Timestamp t6) {
    const Timestamp t7 = __rdtsc(); // t7: always — after send syscall

    for (int i = 0; i < out.latency_count(); ++i) {
        const PendingLatency& e = out.latency_entry(i);
        if (e.record) {
            LatencySample s{};
            s.t0 = e.read_begin_tsc;
            s.t7 = t7;
#if DIAGNOSTICS
            s.t1 = e.recv_tsc;
            s.t2 = e.server_push_tsc;
            s.t3 = e.engine_pop_tsc;
            s.t4 = e.engine_push_tsc;
            s.t5 = e.server_pop;
            s.t6 = t6;
#endif
            latency_handler.push_sample(s);
        }
    }

    out.clear_latency_batch();
    out.write_buffer().try_reset();
}

//=============================================================================
// handleDisconnects  — two-phase close protocol
//=============================================================================

// Outbound thread: scan condemned_inbound_ (set by inbound or condemnConnection).
// Clean up all outbound-private state for the fd, then signal inbound via
// condemned_outbound_ (release) so it can safely reset inbound_map_ and close.
void Server::handleDisconnectsOutbound() {
    for (int fd = 0; fd < static_cast<int>(condemned_inbound_.size()); ++fd) {
        if (!condemned_inbound_[fd].load(std::memory_order_acquire)) continue;

        auto& ostate = outbound_map_[fd];
        if (!ostate.has_value()) continue;

        writable_fds_.erase(fd);
        if (epollout_armed_[fd]) {
            epoll_ctrl(epoll_out_, fd, EPOLL_CTL_DEL, 0);
            epollout_armed_[fd] = false;
        }
        ostate.reset();

        // Release: all outbound accesses to outbound_map_[fd] happen-before
        // inbound's acquire of this flag, and therefore before its reset().
        condemned_outbound_[fd].store(true, std::memory_order_release);
    }
}

// Inbound thread: scan condemned_outbound_ (set by outbound or condemnConnection).
// Acquire load ensures all outbound writes to outbound_map_[fd] are visible
// here, so resetting inbound_map_[fd] (which closes the fd via EpollSocket
// RAII) is safe.  Both flags are cleared last so the fd slot can be reused.
void Server::handleDisconnectsInbound() {
    for (int fd = 0; fd < static_cast<int>(condemned_outbound_.size()); ++fd) {
        if (!condemned_outbound_[fd].load(std::memory_order_acquire)) continue;

        auto& istate = inbound_map_[fd];
        if (!istate.has_value()) continue;

        // Clear the client_map_ routing entry atomically so in-flight outbound
        // routing sees an invalid fd and skips rather than accessing freed state.
        const ClientId cid = istate->client_id();
        if (cid > 0) {
            const auto map_it = client_map_.find(cid);
            if (map_it != client_map_.end()) {
                map_it->second.store(-1, std::memory_order_relaxed);
            }
        }

        // Destructs EpollSocket: calls epoll_ctl(DEL, epoll_in_, fd) then close().
        // The DEL is idempotent if condemnConnection(true) already removed it.
        istate.reset();

        // Reset flags last; registerConnection relies on seeing false here.
        condemned_inbound_[fd].store(false, std::memory_order_relaxed);
        condemned_outbound_[fd].store(false, std::memory_order_relaxed);
    }
}

//=============================================================================
// condemnConnection
//=============================================================================

void Server::condemnConnection(const int fd, const bool caller_inbound) {
    if (caller_inbound) {
        epoll_ctrl(epoll_in_, fd, EPOLL_CTL_DEL, 0);
        condemned_inbound_[fd].store(true, std::memory_order_release);
    } else {
        writable_fds_.erase(fd);
        if (epollout_armed_[fd]) {
            epoll_ctrl(epoll_out_, fd, EPOLL_CTL_DEL, 0);
            epollout_armed_[fd] = false;
        }
        outbound_map_[fd].reset();
        condemned_outbound_[fd].store(true, std::memory_order_release);
    }
}

//=============================================================================
// Helpers
//=============================================================================

static int epoll_ctrl(const int epoll_fd, const int fd, const int op, const int target) {
    epoll_event ev{};
    // Level-triggered (no EPOLLET). The inbound/outbound threads poll with a zero
    // timeout, so there is no benefit to edge-triggering, and edge mode silently
    // drops readiness whenever a socket buffer is not drained to EAGAIN before we
    // stop reading, or when data/connections arrive between accept() and register.
    // Those missed edges wedge the connection (and, on the listen socket, refuse
    // all further clients) — reliably reproducible under emulation.
    ev.events  = target;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd, op, fd, &ev) == -1) {
        std::cerr << "epoll_ctl error: " << strerror(errno) << std::endl;
        return -1;
    }

    return 0;
}