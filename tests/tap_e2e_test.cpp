//
// Created by cniew on 10/18/26.
//
// The whole exchange -- Server on a net_tap port plus the real Engine --
// against kernel TCP sockets in this process: real Linux TCP (segmentation,
// windows, delayed ACKs) under SoupBinTCP, real matching behind it.
//
// Needs CAP_NET_ADMIN to create the tap interface, so it's its own binary;
// without it, it exits 77, which ctest reports as skipped. To run it:
//
//   sudo ./build/tests/server-tap-tests
//   # or without sudo, in a throwaway user + network namespace:
//   unshare -rnm sh -c 'mount -t tmpfs tmpfs /var/run && ip link set lo up &&
//                       ./build/tests/server-tap-tests'
//

#include "engine.hpp"
#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rte_ethdev.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr int kSkipExitCode = 77; // ctest SKIP_RETURN_CODE
const char *const kIface = "dpdktapsim0";

std::shared_ptr<InboundRing> g_in_ring;
std::shared_ptr<OutboundRing> g_out_ring;
std::unique_ptr<MarketDataRing> g_market_data_ring;
std::atomic<bool> g_engine_stop{false};
std::unique_ptr<Engine> g_engine;
std::unique_ptr<Server> g_server;

// The tap PMD raises real-time signals on the process as packets move; a
// blocking socket call on a thread that accepts them fails with EINTR. The
// DPDK threads keep accepting them (if nobody did, the kernel would fall back
// to SIGIO and kill the process), so only the test thread blocks them -- after
// starting the others, which would otherwise inherit the mask.
void block_tap_signals() {
    sigset_t rt;
    sigemptyset(&rt);
    for (int sig = SIGRTMIN; sig <= SIGRTMAX; ++sig) sigaddset(&rt, sig);
    pthread_sigmask(SIG_BLOCK, &rt, nullptr);
}

OuchInboundFrame Enter(const uint32_t user_ref_num, const char side, const uint32_t quantity, const uint64_t price) {
    OuchInboundFrame frame{};
    frame.enter_order = OuchEnterOrder{};
    OuchEnterOrder &m = frame.enter_order;
    m.user_ref_num = user_ref_num;
    m.side = side;
    m.quantity = quantity;
    m.symbol = wire::alpha<8>(BOOK_SYMBOL);
    m.price = price;
    m.time_in_force = '0';
    m.display = 'Y';
    m.capacity = 'A';
    m.intermarket_sweep_eligibility = 'N';
    m.cross_type = 'N';
    m.cl_ord_id = wire::alpha<14>("E2E");
    return frame;
}

struct SoupReply {
    SoupPacketType         type;
    std::vector<std::byte> payload;
    [[nodiscard]] OuchOutboundFrame ouch() const { return decode_outbound_ouch(payload); }
};

// A SoupBinTCP client on a kernel socket, with timeouts so a broken stack
// fails the test instead of hanging it.
class Client {
public:
    Client() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        timeval tv{3, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        constexpr int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(EXCHANGE_PORT);
        to.sin_addr.s_addr = htonl(RTE_IPV4(10, 77, 0, 2));
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr *>(&to), sizeof(to)) == 0;
    }
    ~Client() { close(); }
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    [[nodiscard]] bool connected() const { return connected_; }

    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool send_all(const std::span<const std::byte> bytes) const {
        for (size_t off = 0; off < bytes.size();) {
            const ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool login(const std::string_view username, const uint64_t sequence = 1) const {
        const SoupLoginRequest login{.username = wire::alpha<6>(username), .password = wire::alpha<10>("e2e"),
                                     .requested_session = wire::alpha<10>(""),
                                     .requested_sequence_number = sequence};
        std::array<std::byte, kSoupHeaderLen + kSoupLoginRequestLen> out{};
        return send_all(std::span{out}.first(encode_soup_login_request(login, out)));
    }

    bool send(const OuchInboundFrame &ouch) const {
        std::array<std::byte, 256> out{};
        return send_all(std::span{out}.first(encode_soup_unsequenced(ouch, out)));
    }

    // The next non-heartbeat packet, reassembled from the stream.
    std::optional<SoupReply> receive() {
        while (true) {
            const SoupParseResult parsed = parse_soup_packet(buffer_);
            if (parsed.status == SoupParseStatus::PACKET) {
                SoupReply reply{parsed.packet.type, {parsed.packet.payload.begin(), parsed.packet.payload.end()}};
                buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(parsed.consumed));
                if (reply.type == SoupPacketType::SERVER_HEARTBEAT) continue;
                return reply;
            }
            std::byte chunk[65536];
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return std::nullopt;
            buffer_.insert(buffer_.end(), chunk, chunk + n);
        }
    }

    std::optional<SoupLoginAccepted> login_accepted() {
        const auto reply = receive();
        if (!reply || reply->type != SoupPacketType::LOGIN_ACCEPTED) return std::nullopt;
        return decode_soup_login_accepted(reply->payload);
    }

private:
    int fd_ = -1;
    bool connected_ = false;
    std::vector<std::byte> buffer_;
};

std::optional<OuchOutboundFrame> NextOuch(Client &client) {
    const auto reply = client.receive();
    if (!reply || reply->type != SoupPacketType::SEQUENCED_DATA) return std::nullopt;
    return reply->ouch();
}

} // namespace

TEST(ExchangeOverTap, OrdersMatchBetweenTwoKernelClients) {
    Client seller, buyer;
    ASSERT_TRUE(seller.connected());
    ASSERT_TRUE(buyer.connected());
    ASSERT_TRUE(seller.login("SELL01"));
    ASSERT_TRUE(seller.login_accepted());
    ASSERT_TRUE(buyer.login("BUY001"));
    ASSERT_TRUE(buyer.login_accepted());

    ASSERT_TRUE(seller.send(Enter(1, 'S', 100, 1'500'000)));
    auto ack = NextOuch(seller);
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->message_type, OutboundOuchMessageType::ORDER_ACCEPTED);

    ASSERT_TRUE(buyer.send(Enter(1, 'B', 100, 1'500'000)));
    ack = NextOuch(buyer);
    ASSERT_TRUE(ack);
    EXPECT_EQ(ack->message_type, OutboundOuchMessageType::ORDER_ACCEPTED);

    for (Client *client : {&buyer, &seller}) {
        const auto fill = NextOuch(*client);
        ASSERT_TRUE(fill);
        ASSERT_EQ(fill->message_type, OutboundOuchMessageType::ORDER_EXECUTED);
        EXPECT_EQ(fill->order_executed.executed_shares, 100u);
        EXPECT_EQ(fill->order_executed.execution_price, 1'500'000u);
    }
}

TEST(ExchangeOverTap, ReconnectReplaysFillsMissedWhileAway) {
    {
        Client resting;
        ASSERT_TRUE(resting.connected());
        ASSERT_TRUE(resting.login("REST01"));
        ASSERT_TRUE(resting.login_accepted());
        ASSERT_TRUE(resting.send(Enter(1, 'S', 300, 1'600'000)));
        const auto ack = NextOuch(resting); // sequence 1
        ASSERT_TRUE(ack);
        ASSERT_EQ(ack->message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
    } // disconnected; the order keeps resting

    Client taker;
    ASSERT_TRUE(taker.connected());
    ASSERT_TRUE(taker.login("TAKE01"));
    ASSERT_TRUE(taker.login_accepted());
    ASSERT_TRUE(taker.send(Enter(1, 'B', 100, 1'600'000)));
    ASSERT_TRUE(taker.send(Enter(2, 'B', 100, 1'600'000)));
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(NextOuch(taker)); // two acks, two fills

    // The session is freed once the server has seen the old connection go.
    std::optional<SoupLoginAccepted> accepted;
    std::unique_ptr<Client> back;
    for (int attempt = 0; attempt < 20 && !accepted; ++attempt) {
        back = std::make_unique<Client>();
        ASSERT_TRUE(back->connected());
        ASSERT_TRUE(back->login("REST01", /*sequence=*/2));
        accepted = back->login_accepted();
        if (!accepted) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->sequence_number, 2u);
    for (int i = 0; i < 2; ++i) {
        const auto fill = NextOuch(*back);
        ASSERT_TRUE(fill);
        EXPECT_EQ(fill->message_type, OutboundOuchMessageType::ORDER_EXECUTED);
        EXPECT_EQ(fill->order_executed.executed_shares, 100u);
    }
}

TEST(ExchangeOverTap, PipelinedEntersAreAllAnswered) {
    // Thousands of messages in flight at once: the kernel coalesces them into
    // segments however it likes, and the replies outrun its receive window.
    Client client;
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.login("PIPE01"));
    ASSERT_TRUE(client.login_accepted());

    constexpr uint32_t kOrders = 5'000;
    std::vector<std::byte> batch;
    for (uint32_t ref = 1; ref <= kOrders; ++ref) {
        std::array<std::byte, 256> out{};
        // Far from the other tests' prices, so nothing trades.
        const size_t len = encode_soup_unsequenced(Enter(ref, 'B', 10, 100'000), out);
        batch.insert(batch.end(), out.begin(), out.begin() + static_cast<std::ptrdiff_t>(len));
    }
    ASSERT_TRUE(client.send_all(batch));

    for (uint32_t ref = 1; ref <= kOrders; ++ref) {
        const auto ack = NextOuch(client);
        ASSERT_TRUE(ack) << "after " << ref - 1 << " acks";
        ASSERT_EQ(ack->message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
        ASSERT_EQ(ack->order_accepted.user_ref_num, ref);
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    g_in_ring = std::make_shared<InboundRing>(COMMUNICATION_RING_COUNT);
    g_out_ring = std::make_shared<OutboundRing>(COMMUNICATION_RING_COUNT);
    g_market_data_ring = std::make_unique<MarketDataRing>(COMMUNICATION_RING_COUNT);
    // The engine's Orderbook calibrates the TscClock before any thread reads it.
    g_engine = std::make_unique<Engine>(*g_in_ring, *g_out_ring, *g_market_data_ring, g_engine_stop);

    ServerNetwork network = ServerNetwork::tap_interface(kIface);
    network.eal.file_prefix = "exchange_tap_test_" + std::to_string(::getpid());
    network.eal.log_level = "*:warning";
    try {
        g_server = std::make_unique<Server>(g_in_ring, g_out_ring, std::move(network));
    } catch (const std::exception &e) {
        std::printf("SKIP: couldn't bring up %s (needs root / CAP_NET_ADMIN): %s\n", kIface, e.what());
        return kSkipExitCode;
    }

    g_server->run();
    g_engine->run();
    block_tap_signals();

    const int rc = RUN_ALL_TESTS();

    g_server->stop();
    g_engine_stop.store(true);
    g_engine.reset();
    g_server.reset();
    return rc;
}
