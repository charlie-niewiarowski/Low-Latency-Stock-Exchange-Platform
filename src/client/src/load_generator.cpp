//
// Created by cniew on 5/24/26.
//

#include "../include/load_generator.hpp"
#include "../include/order_factory.hpp"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

static uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

//=============================================================================
// Response framing constants
//=============================================================================
static constexpr size_t FRAME_SIZE = OUTBOUND_BSIZE;

//=============================================================================
// Special member functions
//=============================================================================

LoadGenerator::LoadGenerator(const int num_clients, const uint64_t rng_seed)
    : num_clients_(num_clients)
{
    if constexpr (EXPECTED_THROUGHPUT > 0) {
        inter_arrival_ns_ = static_cast<uint64_t>(
            static_cast<double>(num_clients_) * 1e9 / EXPECTED_THROUGHPUT);
    }
    OrderFactory::instance(rng_seed);  // seed the singleton before first build_frame
    clients_.reserve(static_cast<size_t>(num_clients_));
    for (int i = 0; i < num_clients_; ++i) {
        ClientState& cs = clients_.emplace_back();
        cs.client_id = static_cast<uint32_t>(i + 1);
    }
}

LoadGenerator::~LoadGenerator() {
    for (auto& cs : clients_) {
        if (cs.fd != -1) {
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
            ::close(cs.fd);
        }
    }
    if (epoll_fd_ != -1) ::close(epoll_fd_);
}

//=============================================================================
// run()
//=============================================================================

void LoadGenerator::run() {
    if (!setupEpoll()) return;
    for (auto& cs : clients_) connectClient(cs);

    while (running_.load(std::memory_order_relaxed)) {
        const uint64_t now = now_ns();

        // Bound the wait by the nearest future deadline: a rate-limited send slot
        // or a pending reconnect. Disconnected clients (fd == -1) contribute no
        // epoll events, so without this the loop could sleep past their retry.
        uint64_t min_deadline = UINT64_MAX;
        for (const auto& cs : clients_) {
            if (cs.fd == -1) {
                min_deadline = std::min(min_deadline, cs.reconnect_at_ns);
            } else if (inter_arrival_ns_ > 0 && !cs.connecting
                       && cs.acks_pending < PIPELINE_DEPTH && cs.write_buf.length() == 0) {
                min_deadline = std::min(min_deadline, cs.next_send_ns);
            }
        }
        int timeout_ms = -1;
        if (min_deadline != UINT64_MAX) {
            const int64_t wait_ns = static_cast<int64_t>(min_deadline) - static_cast<int64_t>(now);
            timeout_ms = wait_ns <= 0 ? 0 : static_cast<int>(wait_ns / 1'000'000);
        }

        const int nfds = epoll_wait(epoll_fd_, events_, CLIENT_EPOLL_BATCH, timeout_ms);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            std::fprintf(stderr, "epoll_wait: %s\n", std::strerror(errno));
            return;
        }

        for (int i = 0; i < nfds; ++i) {
            auto* cs = static_cast<ClientState*>(events_[i].data.ptr);
            const uint32_t ev = events_[i].events;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                reconnect(*cs);
                continue;
            }
            if (ev & EPOLLOUT) {
                if (cs->connecting) handleConnect(*cs);
                else                flushWrite(*cs);
                if (cs->fd == -1) continue;  // reconnect happened inside
            }
            if (ev & EPOLLIN) handleReadable(*cs);
        }

        // Reconnect clients whose backoff has elapsed.
        const uint64_t after = now_ns();
        for (auto& cs : clients_) {
            if (cs.fd == -1 && after >= cs.reconnect_at_ns)
                connectClient(cs);
        }

        // Unblock clients whose inter-arrival slot elapsed while they had no I/O events.
        if (inter_arrival_ns_ > 0) {
            for (auto& cs : clients_) {
                if (cs.fd != -1 && !cs.connecting && cs.acks_pending < PIPELINE_DEPTH && cs.write_buf.length() == 0)
                    buildBatch(cs);
            }
        }
    }
}

//=============================================================================
// Setup
//=============================================================================

bool LoadGenerator::setupEpoll() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        std::fprintf(stderr, "epoll_create1: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

bool LoadGenerator::connectClient(ClientState& cs) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd == -1) {
        std::fprintf(stderr, "socket: %s\n", std::strerror(errno));
        return false;
    }

    constexpr int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(EXCHANGE_PORT);
    inet_pton(AF_INET, EXCHANGE_HOST, &addr.sin_addr);

    cs.fd           = fd;
    cs.connecting   = true;
    cs.write_buf.clear();
    cs.read_buf.clear();
    cs.acks_pending = 0;
    cs.pending_head = 0;
    cs.pending_tail = 0;
    cs.active_write = 0;
    cs.active_count = 0;
    cs.active_read  = 0;

    const int rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));

    if (rc == 0 || errno == EINPROGRESS) {
        // Arm EPOLLOUT: fires immediately (rc==0, loopback) or on handshake
        // completion (EINPROGRESS).  handleConnect() switches to EPOLLIN.
        epollAdd(cs, EPOLLOUT);
        return true;
    }

    std::fprintf(stderr, "connect: %s\n", std::strerror(errno));
    ::close(fd); cs.fd = -1;
    return false;
}

//=============================================================================
// Event handlers
//=============================================================================

// EPOLLOUT while connecting — check for async error, then start the pipeline.
void LoadGenerator::handleConnect(ClientState& cs) {
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) { reconnect(cs); return; }

    cs.connecting = false;
    cs.reconnect_backoff_ms = 0;  // successful connect resets backoff
    #if LOGGING
    std::fprintf(stderr, "[CONN]  cid:%-6u :: connected\n",
                 static_cast<unsigned>(cs.client_id));
    #endif
    epollMod(cs, EPOLLIN);   // switch to EPOLLIN only; writes go direct via send()
    buildBatch(cs);
}

// EPOLLIN — recv() and parse every complete response, then top up the pipeline.
//
// The server pads every response to exactly FRAME_SIZE (OUTBOUND_BSIZE) bytes,
// so the client always advances by FRAME_SIZE per message.  This gives byte-
// level alignment: no matter where TCP splits a stream, we never mistake the
// tail bytes of one frame for the header of the next.
void LoadGenerator::handleReadable(ClientState& cs) {
    const ssize_t n = cs.read_buf.read_from(cs.fd);
    if (n == 0) { reconnect(cs); return; }
    if (n <  0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        reconnect(cs); return;
    }

    // Drain all complete FRAME_SIZE-aligned responses.
    while (cs.read_buf.length() >= FRAME_SIZE) {
        const char* frame = cs.read_buf.view().data();

        // Validate the "EXCHANGE\n" prefix before inspecting the type byte.
        // A mismatched prefix means the server sent protocol-violating bytes.
        if (memcmp(frame, "EXCHANGE\n", 9) != 0) {
            #if LOGGING
            std::fprintf(stderr, "[WARN]  cid:%-6u :: bad response header\n",
                         static_cast<unsigned>(cs.client_id));
            #endif
            reconnect(cs);
            return;
        }

        const char type = frame[9];   // 'O' = ACK, 'M' = MATCH fill, 'E' = ERROR
        if (type == 'O') {
            if (cs.acks_pending > 0) {
                // Consume the matching pending type to learn what request this ACK is for.
                const MessageType sent_type =
                    cs.pending_types[cs.pending_head % PIPELINE_DEPTH];
                ++cs.pending_head;
                // NEW ACKs carry the exchange-assigned order_id; store it for
                // future CANCEL/MODIFY targeting.
                if (sent_type == MessageType::NEW) {
                    OrderId oid; std::memcpy(&oid, frame + 16, sizeof(oid));
                    cs.active_orders[cs.active_write % MAX_ACTIVE_ORDERS] = oid;
                    ++cs.active_write;
                    if (cs.active_count < MAX_ACTIVE_ORDERS) ++cs.active_count;
                }
                --cs.acks_pending;
                --cs.orders_in_flight;
                ++cs.responses_ack;
                #if LOGGING
                {
                    ClientId log_cid; std::memcpy(&log_cid, frame + 12, sizeof(log_cid));
                    OrderId  log_oid; std::memcpy(&log_oid, frame + 16, sizeof(log_oid));
                    std::fprintf(stderr, "[OK]    cid:%-6u oid:%llu\n",
                                 static_cast<unsigned>(log_cid),
                                 static_cast<unsigned long long>(log_oid));
                }
                #endif
            }
        } else if (type == 'M') {
            ++cs.responses_match;
            #if LOGGING
            {
                // "EXCHANGE\nMATCH\n" is 15 bytes; ClientId @ 15, OrderId @ 19.
                ClientId log_cid; std::memcpy(&log_cid, frame + 15, sizeof(log_cid));
                OrderId  log_oid; std::memcpy(&log_oid, frame + 19, sizeof(log_oid));
                std::fprintf(stderr, "[MATCH] cid:%-6u oid:%llu\n",
                             static_cast<unsigned>(log_cid),
                             static_cast<unsigned long long>(log_oid));
            }
            #endif
        } else if (type == 'E') {
            if (cs.acks_pending > 0) {
                ++cs.pending_head;  // keep pending ring in sync with acks_pending
                --cs.acks_pending;
                --cs.orders_in_flight;
            }
            ++cs.responses_err;
            #if LOGGING
            {
                // Error string starts after "EXCHANGE\nERROR\n" (15 bytes), ends at '\n'.
                const char* err_start = frame + 15;
                const char* nl = static_cast<const char*>(
                    std::memchr(err_start, '\n', FRAME_SIZE - 15));
                const int err_len = nl ? static_cast<int>(nl - err_start)
                                       : static_cast<int>(FRAME_SIZE - 15);
                std::fprintf(stderr, "[ERR]   cid:%-6u :: %.*s\n",
                             static_cast<unsigned>(cs.client_id), err_len, err_start);
            }
            #endif
        } else {
            // Unknown type byte — server sent something that violates the protocol.
            #if LOGGING
            std::fprintf(stderr, "[WARN]  cid:%-6u :: unknown response type '\\x%02x'\n",
                         static_cast<unsigned>(cs.client_id),
                         static_cast<unsigned char>(type));
            #endif
            reconnect(cs);
            return;
        }

        cs.read_buf.advance(FRAME_SIZE);
    }
    // read_buf.length() < FRAME_SIZE -> incomplete frame; wait for the next recv().

    buildBatch(cs);
}

//=============================================================================
// Pipeline
//=============================================================================

// Pack frames into write_buf and send in one syscall.
// Rate-limited mode: one request at a time, gated by inter_arrival_ns_.
// Unlimited mode: fills the full PIPELINE_DEPTH pipeline.
void LoadGenerator::buildBatch(ClientState& cs) {
    if (cs.write_buf.length() > 0) return;  // batch still in flight

    if (inter_arrival_ns_ > 0) {
        if (cs.acks_pending >= PIPELINE_DEPTH) return;  // pipeline saturated
        if (now_ns() < cs.next_send_ns)        return;  // inter-arrival slot not yet elapsed
    } else {
        if (cs.acks_pending == PIPELINE_DEPTH) return;  // pipeline saturated
    }

    auto& factory = OrderFactory::instance();
    cs.write_buf.clear();

const int slots = (inter_arrival_ns_ > 0) ? 1 : PIPELINE_DEPTH;
    while (cs.acks_pending < static_cast<uint32_t>(slots)) {
        char frame[INBOUND_BSIZE];
        InboundMessage msg{};
        const OrderId active_oid = (cs.active_count > 0)
            ? cs.active_orders[cs.active_read % MAX_ACTIVE_ORDERS]
            : 0;
        factory.build_frame(cs.client_id, active_oid, frame, msg);
        cs.write_buf.append(frame, INBOUND_BSIZE);
        // Advance the read cursor only for CANCEL/MODIFY so each targets a fresh slot.
        if (msg.message_type == MessageType::CANCEL ||
            msg.message_type == MessageType::MODIFY)
            ++cs.active_read;
        cs.pending_types[cs.pending_tail % PIPELINE_DEPTH] = msg.message_type;
        ++cs.pending_tail;
        ++cs.acks_pending;
        ++cs.requests_sent;
        ++cs.orders_in_flight;
    }

    if (inter_arrival_ns_ > 0)
        cs.next_send_ns = now_ns() + inter_arrival_ns_;

    flushWrite(cs);
}

// Drain write_buf to the kernel.  On success the socket stays armed EPOLLIN
// only — no epoll_ctl needed.  On EAGAIN we arm EPOLLOUT and let the event
// loop retry via the EPOLLOUT path (rare on loopback).
void LoadGenerator::flushWrite(ClientState& cs) {
    while (cs.write_buf.remaining_to_send() > 0) {
        const ssize_t n = cs.write_buf.write_to(cs.fd);
        if (n > 0) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            epollMod(cs, EPOLLIN | EPOLLOUT);  // will retry via flushWrite()
            return;
        }
        reconnect(cs);
        return;
    }
    // Fully sent.  Reset buffer; leave socket armed EPOLLIN (no epoll_ctl).
    cs.write_buf.try_reset();
}

//=============================================================================
// Reconnect
//=============================================================================

void LoadGenerator::reconnect(ClientState& cs) {
    if (cs.fd == -1) return;
    #if LOGGING
    std::fprintf(stderr, "[RECONN] cid:%-6u :: reconnecting\n",
                 static_cast<unsigned>(cs.client_id));
    #endif
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cs.fd, nullptr);
    ::close(cs.fd);
    cs.fd = -1;
    // Pending acks will never arrive; correct the in-flight counter.
    cs.orders_in_flight -= static_cast<int64_t>(cs.acks_pending);
    cs.acks_pending = 0;

    // Linear backoff, capped at RECONNECT_DELAY_MS. Deferring the reconnect (vs.
    // calling connectClient() inline) prevents a refused/reset connection from
    // becoming a tight connect() spin that exhausts ephemeral ports and wedges
    // the whole netns. The event loop calls connectClient() once the delay is up.
    if (cs.reconnect_backoff_ms < RECONNECT_DELAY_MS)
        cs.reconnect_backoff_ms += 50;
    cs.reconnect_at_ns = now_ns() + static_cast<uint64_t>(cs.reconnect_backoff_ms) * 1'000'000ULL;
}

//=============================================================================
// epoll helpers
//=============================================================================

void LoadGenerator::epollAdd(ClientState& cs, const uint32_t events) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = &cs;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cs.fd, &ev);
}

void LoadGenerator::epollMod(ClientState& cs, const uint32_t events) {
    epoll_event ev{};
    ev.events   = events;
    ev.data.ptr = &cs;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, cs.fd, &ev);
}

//=============================================================================
// Stats
//=============================================================================

LoadGenerator::Stats LoadGenerator::stats() const {
    Stats s{};
    for (const auto& cs : clients_) {
        s.requests_sent    += cs.requests_sent;
        s.responses_ack    += cs.responses_ack;
        s.responses_match  += cs.responses_match;
        s.responses_err    += cs.responses_err;
        s.orders_in_flight += cs.orders_in_flight;
    }
    return s;
}