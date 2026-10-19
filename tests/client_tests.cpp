//
// Created by cniew on 10/18/26.
//
// The real LoadGenerator against a minimal in-process SoupBinTCP + OUCH
// exchange over kernel loopback (EXCHANGE_HOST:EXCHANGE_PORT, so that port
// must be free while this runs; client-tests points it at loopback). The
// mock checks the client's side of both protocols -- login first, data only
// after Login Accepted, UserRefNums strictly increasing, nothing
// undecodable, a re-login asking for the next unseen sequence number -- and
// answers like the real gateway would, keeping each username's Sequenced
// Data for replay.
//

#include "load_generator.hpp"
#include "soupbintcp_proto.hpp"
#include "validation.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

class MockExchange {
public:
    enum class Mode {
        NORMAL,      // accept logins, answer every message
        SILENT,      // accept logins, never answer orders
        REJECT_ALL,  // reject every login
        FLAKY,       // like NORMAL, but drops each connection after kFlakyEnters Enters
    };
    static constexpr int kFlakyEnters = 50;

    explicit MockExchange(const Mode mode) : mode_(mode) {
        listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        constexpr int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(EXCHANGE_PORT);
        inet_pton(AF_INET, EXCHANGE_HOST, &addr.sin_addr);
        listening_ = bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0 &&
                     listen(listen_fd_, 64) == 0;
        thread_ = std::jthread([this](const std::stop_token &stop) { run(stop); });
    }

    ~MockExchange() {
        thread_.request_stop();
        thread_.join();
        for (const Connection &c : connections_) ::close(c.fd);
        ::close(listen_fd_);
    }

    [[nodiscard]] bool listening() const { return listening_; }

    std::atomic<int> login_requests{0};
    std::atomic<int> enters{0};
    std::atomic<int> cancels{0};
    std::atomic<int> replaces{0};
    std::atomic<int> client_heartbeats{0};
    std::atomic<int> violations{0};
    std::atomic<int> usernames{0};       // distinct usernames that logged in
    std::atomic<int> resumed_logins{0};  // logins asking to start past sequence 1

private:
    struct Connection {
        int fd;
        std::vector<std::byte> buffer;
        bool logged_in = false;
        std::string username;
        int enters = 0;
        uint32_t last_user_ref_num = 0;
        uint64_t next_order_ref = 1;
    };

    void run(const std::stop_token &stop) {
        while (!stop.stop_requested()) {
            std::vector<pollfd> fds{{listen_fd_, POLLIN, 0}};
            for (const Connection &c : connections_) fds.push_back({c.fd, POLLIN, 0});
            if (poll(fds.data(), fds.size(), 10) <= 0) continue;

            if (fds[0].revents & POLLIN) {
                const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd >= 0) {
                    constexpr int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    connections_.push_back({fd, {}});
                }
            }
            for (size_t i = 1; i < fds.size(); ++i) {
                if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) read(connections_[i - 1]);
            }
            std::erase_if(connections_, [](const Connection &c) { return c.fd < 0; });
        }
    }

    void read(Connection &c) {
        std::byte chunk[4096];
        const ssize_t n = recv(c.fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            ::close(c.fd);
            c.fd = -1;
            return;
        }
        c.buffer.insert(c.buffer.end(), chunk, chunk + n);

        size_t offset = 0;
        while (offset < c.buffer.size()) {
            const SoupParseResult parsed = parse_soup_packet(std::span{c.buffer}.subspan(offset));
            if (parsed.status == SoupParseStatus::INCOMPLETE) break;
            if (parsed.status == SoupParseStatus::MALFORMED) {
                ++violations;
                c.buffer.clear();
                return;
            }
            handle(c, parsed.packet);
            offset += parsed.consumed;
        }
        c.buffer.erase(c.buffer.begin(), c.buffer.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    void handle(Connection &c, const SoupPacket &packet) {
        switch (packet.type) {
            case SoupPacketType::LOGIN_REQUEST: {
                ++login_requests;
                const auto login = decode_soup_login_request(packet.payload);
                if (c.logged_in || !login) {
                    ++violations;
                    return;
                }
                if (mode_ == Mode::REJECT_ALL) {
                    send(c, [](auto out) { return encode_soup_login_rejected(soup_reject::kNotAuthorized, out); });
                    return;
                }
                c.logged_in = true;
                c.username.assign(login->username.begin(), login->username.end());
                const auto [it, is_new] = logs_.try_emplace(c.username);
                if (is_new) ++usernames;
                const std::vector<std::vector<std::byte>> &log = it->second;

                // A client asks for the next message it hasn't seen: never 0
                // (it always knows), never past what was sent.
                const uint64_t sequence = login->requested_sequence_number;
                if (sequence == 0 || sequence > log.size() + 1) {
                    ++violations;
                    return;
                }
                if (sequence > 1) ++resumed_logins;
                send(c, [&](auto out) {
                    return encode_soup_login_accepted({.session = soup_session_id("SIM"), .sequence_number = sequence},
                                                      out);
                });
                for (size_t i = sequence - 1; i < log.size(); ++i) {
                    ::send(c.fd, log[i].data(), log[i].size(), MSG_NOSIGNAL);
                }
                return;
            }
            case SoupPacketType::UNSEQUENCED_DATA:
                if (!c.logged_in) ++violations;
                order(c, decode_inbound_ouch(packet.payload));
                return;
            case SoupPacketType::CLIENT_HEARTBEAT:
                ++client_heartbeats;
                return;
            default:
                ++violations;
                return;
        }
    }

    void order(Connection &c, const OuchInboundFrame &msg) {
        const auto sequence = [&](const uint32_t user_ref_num) {
            if (user_ref_num <= c.last_user_ref_num) ++violations;
            c.last_user_ref_num = user_ref_num;
        };

        OuchOutboundFrame reply{};
        switch (msg.message_type) {
            case InboundOuchMessageType::ENTER_ORDER: {
                ++enters;
                if (mode_ == Mode::FLAKY && ++c.enters > kFlakyEnters) {
                    ::shutdown(c.fd, SHUT_RDWR); // gone mid-stream; read() reaps it
                    return;
                }
                const OuchEnterOrder &e = msg.enter_order;
                sequence(e.user_ref_num);
                if (validation::check_enter(e, {.min = MIN_PRICE, .max = MAX_PRICE}).reject_reason != 0) {
                    ++violations; // the load generator only builds valid orders
                }
                if (mode_ == Mode::SILENT) return;
                reply.order_accepted = OuchOrderAccepted{
                    .timestamp = 0, .user_ref_num = e.user_ref_num, .side = e.side, .quantity = e.quantity,
                    .symbol = e.symbol, .price = e.price, .time_in_force = e.time_in_force,
                    .display = e.display, .order_reference_number = c.next_order_ref++,
                    .capacity = e.capacity, .intermarket_sweep_eligibility = e.intermarket_sweep_eligibility,
                    .cross_type = e.cross_type, .order_state = 'L', .cl_ord_id = e.cl_ord_id,
                };
                break;
            }
            case InboundOuchMessageType::CANCEL_ORDER:
                ++cancels;
                if (mode_ == Mode::SILENT) return;
                reply.order_canceled = OuchOrderCanceled{
                    .timestamp = 0, .user_ref_num = msg.cancel_order.user_ref_num,
                    .decrement_shares = 1, .reason = 'U',
                };
                break;
            case InboundOuchMessageType::REPLACE_ORDER: {
                ++replaces;
                const OuchReplaceOrder &r = msg.replace_order;
                sequence(r.user_ref_num);
                if (mode_ == Mode::SILENT) return;
                reply.order_replaced = OuchOrderReplaced{
                    .timestamp = 0, .orig_user_ref_num = r.orig_user_ref_num, .user_ref_num = r.user_ref_num,
                    .side = 'B', .quantity = r.quantity, .symbol = wire::alpha<8>("SIM"), .price = r.price,
                    .time_in_force = r.time_in_force, .display = r.display,
                    .order_reference_number = c.next_order_ref++, .capacity = 'A',
                    .intermarket_sweep_eligibility = 'N', .cross_type = 'N', .order_state = 'L',
                    .cl_ord_id = r.cl_ord_id,
                };
                break;
            }
            case InboundOuchMessageType::NONE:
                ++violations;
                return;
        }
        std::vector<std::byte> packet(256);
        packet.resize(encode_soup_sequenced(reply, packet));
        ::send(c.fd, packet.data(), packet.size(), MSG_NOSIGNAL);
        logs_[c.username].push_back(std::move(packet));
    }

    template <typename Encode>
    static void send(const Connection &c, Encode &&encode) {
        std::array<std::byte, 256> out{};
        const size_t len = encode(std::span<std::byte>{out});
        ::send(c.fd, out.data(), len, MSG_NOSIGNAL);
    }

    Mode mode_;
    int listen_fd_ = -1;
    bool listening_ = false;
    std::vector<Connection> connections_;
    // Username -> every Sequenced Data packet sent it, for replay.
    std::map<std::string, std::vector<std::vector<std::byte>>> logs_;
    std::jthread thread_;
};

// Runs a LoadGenerator for duration and returns its stats.
LoadGenerator::Stats RunFor(const int num_clients, const std::chrono::milliseconds duration) {
    LoadGenerator generator{num_clients, /*rng_seed=*/42};
    std::jthread runner([&] { generator.run(); });
    std::this_thread::sleep_for(duration);
    generator.stop();
    runner.join();
    return generator.stats();
}

} // namespace

TEST(Client, TradesOverSoupBinTcpWithoutProtocolViolations) {
    MockExchange exchange{MockExchange::Mode::NORMAL};
    ASSERT_TRUE(exchange.listening()) << "port " << EXCHANGE_PORT << " is taken";

    const LoadGenerator::Stats stats = RunFor(2, std::chrono::milliseconds(300));

    EXPECT_EQ(exchange.violations.load(), 0);
    EXPECT_EQ(exchange.login_requests.load(), 2);
    EXPECT_EQ(exchange.usernames.load(), 2); // a session each
    EXPECT_GT(exchange.enters.load(), 100);
    EXPECT_GT(exchange.cancels.load(), 0);
    EXPECT_GT(exchange.replaces.load(), 0);

    EXPECT_GT(stats.requests_sent, 0u);
    EXPECT_GT(stats.responses_ack, 0u);
    EXPECT_GT(stats.responses_canceled, 0u);
    EXPECT_GT(stats.responses_replaced, 0u);
    EXPECT_EQ(stats.responses_err, 0u);
    // Every Enter was accepted; at most a pipeline's worth per client is still unanswered.
    EXPECT_LE(stats.responses_ack, static_cast<uint64_t>(exchange.enters.load()));
    EXPECT_GE(stats.responses_ack + 2 * PIPELINE_DEPTH, static_cast<uint64_t>(exchange.enters.load()));
}

TEST(Client, OnlyEntersFillThePipelineAndIdleSessionsHeartbeat) {
    MockExchange exchange{MockExchange::Mode::SILENT};
    ASSERT_TRUE(exchange.listening()) << "port " << EXCHANGE_PORT << " is taken";

    RunFor(2, std::chrono::milliseconds(1500));

    EXPECT_EQ(exchange.violations.load(), 0);
    // Nothing is ever accepted, so there's nothing to cancel/replace, and each
    // client stops at exactly one pipeline of unanswered Enters.
    EXPECT_EQ(exchange.enters.load(), 2 * PIPELINE_DEPTH);
    EXPECT_EQ(exchange.cancels.load() + exchange.replaces.load(), 0);
    EXPECT_GE(exchange.client_heartbeats.load(), 2);
}

TEST(Client, RejectedLoginBacksOffAndSendsNoOrders) {
    MockExchange exchange{MockExchange::Mode::REJECT_ALL};
    ASSERT_TRUE(exchange.listening()) << "port " << EXCHANGE_PORT << " is taken";

    RunFor(1, std::chrono::milliseconds(400));

    EXPECT_EQ(exchange.violations.load(), 0);
    EXPECT_GE(exchange.login_requests.load(), 2); // retried
    EXPECT_LE(exchange.login_requests.load(), 6); // ...with backoff, not in a spin
    EXPECT_EQ(exchange.enters.load(), 0);
}

TEST(Client, ReconnectsResumeTheSessionWhereTheyLeftOff) {
    MockExchange exchange{MockExchange::Mode::FLAKY};
    ASSERT_TRUE(exchange.listening()) << "port " << EXCHANGE_PORT << " is taken";

    const LoadGenerator::Stats stats = RunFor(1, std::chrono::milliseconds(800));

    EXPECT_EQ(exchange.violations.load(), 0);
    EXPECT_EQ(exchange.usernames.load(), 1);    // the same session every time
    EXPECT_GE(exchange.resumed_logins.load(), 1); // asking for what it hadn't seen, not from 1
    EXPECT_GT(exchange.enters.load(), MockExchange::kFlakyEnters);
    EXPECT_EQ(stats.responses_err, 0u);
}
