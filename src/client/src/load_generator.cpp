//
// Created by cniew on 5/24/26.
//

#include "../include/load_generator.hpp"
#include "../include/order_factory.hpp"
#include "validation.hpp"

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <span>
#include <string_view>

static uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

static std::span<const std::byte> as_bytes(const std::string_view view) {
    return {reinterpret_cast<const std::byte*>(view.data()), view.size()};
}

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
                continue;
            }
            if (!cs.logged_in) continue;
            // SoupBinTCP: never go more than a second without sending something.
            min_deadline = std::min(min_deadline, cs.last_sent_ns + kSoupHeartbeatIntervalNs);
            if (inter_arrival_ns_ > 0 && cs.acks_pending < PIPELINE_DEPTH && cs.write_buf.length() == 0) {
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
                if (cs.fd != -1 && cs.logged_in && cs.acks_pending < PIPELINE_DEPTH && cs.write_buf.length() == 0)
                    buildBatch(cs);
            }
        }

        // Heartbeat any logged-in client that's been quiet for a second.
        for (auto& cs : clients_) {
            if (cs.fd != -1 && cs.logged_in && cs.write_buf.length() == 0
                    && after >= cs.last_sent_ns + kSoupHeartbeatIntervalNs)
                sendHeartbeat(cs);
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

    // A new connection is a new session on the exchange's side: orders from
    // the old one can't be targeted any more. next_user_ref_num carries on.
    cs.fd           = fd;
    cs.connecting   = true;
    cs.logged_in    = false;
    cs.write_buf.clear();
    cs.read_buf.clear();
    cs.acks_pending = 0;
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

// EPOLLOUT while connecting — check for async error, then log in. Orders
// wait for Login Accepted (see handleSoupPacket).
void LoadGenerator::handleConnect(ClientState& cs) {
    int err = 0; socklen_t len = sizeof(err);
    getsockopt(cs.fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) { reconnect(cs); return; }

    cs.connecting = false;
    #if LOGGING
    std::fprintf(stderr, "[CONN]  cid:%-6u :: connected, logging in\n",
                 static_cast<unsigned>(cs.client_id));
    #endif
    epollMod(cs, EPOLLIN);   // switch to EPOLLIN only; writes go direct via send()

    char username[7];
    std::snprintf(username, sizeof(username), "%s%04u", SOUP_USERNAME_PREFIX,
                  static_cast<unsigned>(cs.client_id % 10'000));
    const SoupLoginRequest login{
        .username                  = wire::alpha<6>(username),
        .password                  = wire::alpha<10>(SOUP_PASSWORD),
        .requested_session         = wire::alpha<10>(""),
        .requested_sequence_number = cs.next_sequence,
    };
    std::array<std::byte, kSoupHeaderLen + kSoupLoginRequestLen> packet{};
    encode_soup_login_request(login, packet);
    cs.write_buf.append(reinterpret_cast<const char*>(packet.data()), packet.size());
    flushWrite(cs);
}

// EPOLLIN — recv() and handle every complete SoupBinTCP packet, then top up
// the pipeline. SoupBinTCP packets are length-prefixed, so wherever TCP splits
// the stream, an incomplete packet just waits in read_buf for the next recv().
void LoadGenerator::handleReadable(ClientState& cs) {
    const ssize_t n = cs.read_buf.read_from(cs.fd);
    if (n == 0) { reconnect(cs); return; }
    if (n <  0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        reconnect(cs); return;
    }

    while (cs.read_buf.length() > 0) {
        const SoupParseResult parsed = parse_soup_packet(as_bytes(cs.read_buf.view()));
        if (parsed.status == SoupParseStatus::INCOMPLETE) break;
        if (parsed.status == SoupParseStatus::MALFORMED || !handleSoupPacket(cs, parsed.packet)) {
            reconnect(cs);
            return;
        }
        cs.read_buf.advance(parsed.consumed);
    }

    buildBatch(cs);
}

bool LoadGenerator::handleSoupPacket(ClientState& cs, const SoupPacket& packet) {
    switch (packet.type) {
        case SoupPacketType::LOGIN_ACCEPTED: {
            const std::optional<SoupLoginAccepted> accepted = decode_soup_login_accepted(packet.payload);
            if (!accepted) return false;
            // Where the exchange starts sending from: what was asked for, or
            // its next new message if that's sooner.
            cs.next_sequence = accepted->sequence_number;
            cs.logged_in = true;
            // Only a successful login resets backoff: a server that accepts the
            // TCP connection but rejects every login must not get a reconnect spin.
            cs.reconnect_backoff_ms = 0;
            #if LOGGING
            std::fprintf(stderr, "[LOGIN] cid:%-6u :: accepted\n", static_cast<unsigned>(cs.client_id));
            #endif
            return true;
        }

        case SoupPacketType::LOGIN_REJECTED:
            #if LOGGING
            std::fprintf(stderr, "[LOGIN] cid:%-6u :: rejected '%c'\n", static_cast<unsigned>(cs.client_id),
                         packet.payload.empty() ? '?' : static_cast<char>(packet.payload[0]));
            #endif
            return false; // the server closes the session; retry after backoff

        case SoupPacketType::SEQUENCED_DATA: {
            if (!cs.logged_in) return false; // data before Login Accepted violates the protocol
            ++cs.next_sequence;
            const OuchOutboundFrame msg = decode_outbound_ouch(packet.payload);
            if (msg.message_type == OutboundOuchMessageType::NONE) {
                #if LOGGING
                std::fprintf(stderr, "[WARN]  cid:%-6u :: undecodable OUCH message\n",
                             static_cast<unsigned>(cs.client_id));
                #endif
                return false;
            }
            return handleOuch(cs, msg);
        }

        case SoupPacketType::SERVER_HEARTBEAT:
        case SoupPacketType::DEBUG:
            return true;

        case SoupPacketType::END_OF_SESSION:
            return false;

        default:
            // A client-side packet type from the server violates the protocol.
            #if LOGGING
            std::fprintf(stderr, "[WARN]  cid:%-6u :: unexpected packet type '%c'\n",
                         static_cast<unsigned>(cs.client_id), static_cast<char>(packet.type));
            #endif
            return false;
    }
}

bool LoadGenerator::handleOuch(ClientState& cs, const OuchOutboundFrame& msg) {
    // Accepted/Rejected answer exactly one Enter each, freeing its slot.
    const auto answer_enter = [&] {
        if (cs.acks_pending > 0) {
            --cs.acks_pending;
            --cs.orders_in_flight;
        }
    };

    switch (msg.message_type) {
        case OutboundOuchMessageType::ORDER_ACCEPTED: {
            const OuchOrderAccepted& a = msg.order_accepted;
            answer_enter();
            ++cs.responses_ack;
            if (const auto side = validation::to_engine_side(a.side)) rememberOrder(cs, a.user_ref_num, *side);
            #if LOGGING
            std::fprintf(stderr, "[ACK]   cid:%-6u urn:%u ref:%llu qty:%u\n",
                         static_cast<unsigned>(cs.client_id), a.user_ref_num,
                         static_cast<unsigned long long>(a.order_reference_number), a.quantity);
            #endif
            return true;
        }
        case OutboundOuchMessageType::ORDER_REJECTED:
            answer_enter();
            ++cs.responses_err;
            #if LOGGING
            std::fprintf(stderr, "[ERR]   cid:%-6u urn:%u reason:0x%04x\n",
                         static_cast<unsigned>(cs.client_id), msg.order_rejected.user_ref_num,
                         static_cast<unsigned>(msg.order_rejected.reason));
            #endif
            return true;

        case OutboundOuchMessageType::ORDER_EXECUTED:
            ++cs.responses_match;
            #if LOGGING
            std::fprintf(stderr, "[FILL]  cid:%-6u urn:%u qty:%u match:%llu\n",
                         static_cast<unsigned>(cs.client_id), msg.order_executed.user_ref_num,
                         msg.order_executed.executed_shares,
                         static_cast<unsigned long long>(msg.order_executed.match_number));
            #endif
            return true;

        case OutboundOuchMessageType::ORDER_CANCELED:
            ++cs.responses_canceled;
            return true;

        case OutboundOuchMessageType::ORDER_REPLACED: {
            const OuchOrderReplaced& r = msg.order_replaced;
            ++cs.responses_replaced;
            // The replacement is the live order now; the original is dead.
            if (const auto side = validation::to_engine_side(r.side)) rememberOrder(cs, r.user_ref_num, *side);
            return true;
        }

        case OutboundOuchMessageType::NONE:
            return false;
    }
    return false;
}

void LoadGenerator::rememberOrder(ClientState& cs, const uint32_t user_ref_num, const Side side) {
    cs.active_orders[cs.active_write % MAX_ACTIVE_ORDERS] = {user_ref_num, side};
    ++cs.active_write;
    if (cs.active_count < MAX_ACTIVE_ORDERS) ++cs.active_count;
}

//=============================================================================
// Pipeline
//=============================================================================

// Pack messages into write_buf and send in one syscall.
// Rate-limited mode: one message per inter-arrival slot.
// Unlimited mode: fills the pipeline up to PIPELINE_DEPTH Enters, with any
// Cancels/Replaces the factory rolls along the way (at most
// MAX_BATCH_MESSAGES messages per batch).
void LoadGenerator::buildBatch(ClientState& cs) {
    if (!cs.logged_in) return;                // nothing but the login until Login Accepted
    if (cs.write_buf.length() > 0) return;    // batch still in flight
    if (cs.acks_pending >= PIPELINE_DEPTH) return;  // pipeline saturated
    if (inter_arrival_ns_ > 0 && now_ns() < cs.next_send_ns) return;  // inter-arrival slot not yet elapsed

    auto& factory = OrderFactory::instance();
    cs.write_buf.clear();

    const uint32_t max_messages = (inter_arrival_ns_ > 0) ? 1 : MAX_BATCH_MESSAGES;
    std::array<std::byte, 128> packet{};
    for (uint32_t i = 0; i < max_messages && cs.acks_pending < PIPELINE_DEPTH; ++i) {
        std::optional<OrderFactory::Target> target;
        if (cs.active_count > 0) {
            const ClientState::ActiveOrder& active = cs.active_orders[cs.active_read % MAX_ACTIVE_ORDERS];
            target = OrderFactory::Target{active.user_ref_num, active.side};
        }

        OuchInboundFrame msg{};
        const OrderFactory::FrameKind kind = factory.build_message(cs.next_user_ref_num, target, msg);
        const size_t len = encode_soup_unsequenced(msg, packet);
        if (len == 0 || !cs.write_buf.has_room(len)) break;
        cs.write_buf.append(reinterpret_cast<const char*>(packet.data()), len);

        switch (kind) {
            case OrderFactory::FrameKind::ENTER:
            case OrderFactory::FrameKind::INVALID:
                ++cs.next_user_ref_num;
                ++cs.acks_pending;
                ++cs.orders_in_flight;
                break;
            case OrderFactory::FrameKind::REPLACE:
                ++cs.next_user_ref_num;
                ++cs.active_read; // each CANCEL/REPLACE targets a fresh slot
                break;
            case OrderFactory::FrameKind::CANCEL:
                ++cs.active_read;
                break;
        }
        ++cs.requests_sent;
    }

    if (inter_arrival_ns_ > 0)
        cs.next_send_ns = now_ns() + inter_arrival_ns_;

    flushWrite(cs);
}

void LoadGenerator::sendHeartbeat(ClientState& cs) {
    std::array<std::byte, kSoupHeaderLen> packet{};
    encode_soup_empty(SoupPacketType::CLIENT_HEARTBEAT, packet);
    cs.write_buf.append(reinterpret_cast<const char*>(packet.data()), packet.size());
    flushWrite(cs);
}

// Drain write_buf to the kernel.  On success the socket stays armed EPOLLIN
// only — no epoll_ctl needed.  On EAGAIN we arm EPOLLOUT and let the event
// loop retry via the EPOLLOUT path (rare on loopback).
void LoadGenerator::flushWrite(ClientState& cs) {
    while (cs.write_buf.remaining_to_send() > 0) {
        const ssize_t n = cs.write_buf.write_to(cs.fd);
        if (n > 0) { cs.last_sent_ns = now_ns(); continue; }
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
    cs.logged_in = false;
    // Answers to the Enters that reached the exchange come back in the
    // re-login's replay; the rest never will, and there's no telling which
    // is which. Start the pipeline afresh: a replayed answer to an older
    // Enter then finds no slot to free (see handleOuch).
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
        s.responses_canceled += cs.responses_canceled;
        s.responses_replaced += cs.responses_replaced;
        s.orders_in_flight += cs.orders_in_flight;
    }
    return s;
}