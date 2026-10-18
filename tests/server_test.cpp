//
// Created by cniew on 9/11/26.
//
// End-to-end through a real Server over the TESTING net_ring0 vdev: the test
// plays the client (SoupBinTCP-framed OUCH in on the spare tx queue, replies
// out on the spare rx queue -- see Server's constructor) and the engine
// (popping EngineRequests, pushing EngineEvents).
//

#include "server.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr auto kTimeout = std::chrono::milliseconds(2000);

PacketEndpoint MakeEndpoint(const std::array<uint8_t, 6> mac, const uint32_t ip) {
    PacketEndpoint ep{};
    std::memcpy(ep.mac.addr_bytes, mac.data(), mac.size());
    ep.ip = ip;
    return ep;
}

const PacketEndpoint kClient = MakeEndpoint({0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01}, 0xC0A80101); // 192.168.1.1
const PacketEndpoint kServer = MakeEndpoint({0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03}, 0x0A000005); // 10.0.0.5
constexpr uint16_t kServerPort = 4000;

OuchInboundFrame Enter(const uint32_t user_ref_num, const char side, const uint32_t quantity, const uint64_t price) {
    OuchInboundFrame frame{};
    frame.enter_order = OuchEnterOrder{};
    OuchEnterOrder &m = frame.enter_order;
    m.user_ref_num = user_ref_num;
    m.side = side;
    m.quantity = quantity;
    m.symbol = wire::alpha<8>("AAPL");
    m.price = price;
    m.time_in_force = '0';
    m.display = 'Y';
    m.capacity = 'A';
    m.intermarket_sweep_eligibility = 'N';
    m.cross_type = 'N';
    m.cl_ord_id = wire::alpha<14>("ORDER");
    return frame;
}

template <typename F>
auto PollUntil(F &&poll, const std::chrono::milliseconds timeout = kTimeout) -> decltype(poll()) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = poll()) return result;
    }
    return std::nullopt;
}

} // namespace

// The EAL can only be brought up once per process, so the whole suite shares
// one Server. Each test talks from its own client port -- its own session,
// with UserRefNums starting at 1 -- and reads OrderIds/ClientIds off the
// requests rather than assuming them.
class ServerTest : public testing::Test {
protected:
    // One SoupBinTCP packet from the server, copied out of its mbuf.
    struct Reply {
        PacketEndpoint         src;
        PacketEndpoint         dst;
        TCPHeader              transport;
        SoupPacketType         type;
        std::vector<std::byte> payload;

        [[nodiscard]] OuchOutboundFrame ouch() const { return decode_outbound_ouch(payload); }
    };

    static void SetUpTestSuite() {
        inbound_ring_ = std::make_shared<InboundRing>(RINGBUF_SIZE);
        outbound_ring_ = std::make_shared<OutboundRing>(RINGBUF_SIZE);
        server_ = std::make_unique<Server>(inbound_ring_, outbound_ring_);
        client_tx_queue_ = server_->test_client_tx_queue_;
        client_rx_queue_ = server_->dpdk_port_.claim_rx_queue();
        pool_ = server_->dpdk_port_.get_pool();
        server_->run();
    }

    static void TearDownTestSuite() {
        server_->stop();
        client_rx_queue_.reset();
        client_tx_queue_.reset();
        pool_.reset();
        server_.reset();
        outbound_ring_.reset();
        inbound_ring_.reset();
    }

    // Nothing a previous test left in flight is this test's business.
    void SetUp() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        while (inbound_ring_->pop()) {}
        while (client_rx_queue_->receive_burst().size() > 0) {}
    }

    // One TCP segment from the client's (ip, client_port) to the server.
    static bool SendSegment(const std::span<const std::byte> bytes, const uint16_t client_port) {
        const TCPHeader tcp{.src_port = client_port, .dst_port = kServerPort, .flags = RTE_TCP_ACK_FLAG};
        dpdk::packet pkt = packet_factory<TCPHeader, RawPayload>::build_packet(
            *pool_, kClient, kServer, tcp, RawPayload{bytes});
        std::array<dpdk::packet, 1> pkts{std::move(pkt)};
        return client_tx_queue_->send_burst(pkts) == 1;
    }

    static std::vector<std::byte> LoginBytes(const char *session = "") {
        const SoupLoginRequest login{.username = wire::alpha<6>("test"), .password = wire::alpha<10>("test"),
                                     .requested_session = wire::alpha<10>(session),
                                     .requested_sequence_number = 1};
        std::vector<std::byte> out(64);
        out.resize(encode_soup_login_request(login, out));
        return out;
    }

    static std::vector<std::byte> DataBytes(const OuchInboundFrame &ouch) {
        std::vector<std::byte> out(256);
        out.resize(encode_soup_unsequenced(ouch, out));
        return out;
    }

    // Logs client_port in and returns the Login Accepted.
    static std::optional<Reply> Login(const uint16_t client_port) {
        if (!SendSegment(LoginBytes(), client_port)) return std::nullopt;
        return Receive();
    }

    static bool Send(const OuchInboundFrame &ouch, const uint16_t client_port) {
        return SendSegment(DataBytes(ouch), client_port);
    }

    static bool SendRaw(std::string_view payload) {
        dpdk::packet pkt = pool_->get();
        const auto *bytes = reinterpret_cast<const std::byte *>(payload.data());
        if (!pkt || pkt.append(dpdk::packet::Payload(bytes, payload.size())) == nullptr) return false;
        std::array<dpdk::packet, 1> pkts{std::move(pkt)};
        return client_tx_queue_->send_burst(pkts) == 1;
    }

    // Next packet from the server; heartbeats are skipped unless asked for.
    static std::optional<Reply> Receive(const bool skip_heartbeats = true,
                                        const std::chrono::milliseconds timeout = kTimeout) {
        return PollUntil([&]() -> std::optional<Reply> {
            dpdk::packet_burst burst = client_rx_queue_->receive_burst(1);
            for (const dpdk::packet &packet : burst) {
                const auto raw = packet_factory<TCPHeader, RawPayload>::deserialize_packet(packet);
                const SoupParseResult parsed = parse_soup_packet(raw.payload.bytes);
                if (parsed.status != SoupParseStatus::PACKET) return std::nullopt;
                if (skip_heartbeats && parsed.packet.type == SoupPacketType::SERVER_HEARTBEAT) return std::nullopt;
                return Reply{raw.src, raw.dst, raw.transport, parsed.packet.type,
                             {parsed.packet.payload.begin(), parsed.packet.payload.end()}};
            }
            return std::nullopt;
        }, timeout);
    }

    // The test as the engine.
    static std::optional<EngineRequest> EngineReceive() {
        return PollUntil([&] { return inbound_ring_->pop(); });
    }

    static void EngineSend(const EngineEvent &event) { ASSERT_TRUE(outbound_ring_->push(event)); }

    static inline std::shared_ptr<InboundRing> inbound_ring_;
    static inline std::shared_ptr<OutboundRing> outbound_ring_;
    static inline std::unique_ptr<Server> server_;
    static inline std::shared_ptr<dpdk::tx_queue> client_tx_queue_;
    static inline std::shared_ptr<dpdk::rx_queue> client_rx_queue_;
    static inline std::shared_ptr<dpdk::packet_pool> pool_;
};

TEST_F(ServerTest, DropsPacketsThatAreNotSoupBinTcp) {
    ASSERT_TRUE(client_tx_queue_);
    ASSERT_TRUE(SendRaw("some random bullshit"));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(inbound_ring_->pop());
    EXPECT_EQ(client_rx_queue_->receive_burst(1).size(), 0u);
}

//=============================================================================
// SoupBinTCP session
//=============================================================================

TEST_F(ServerTest, LoginIsAcceptedIntoTheSimSession) {
    const auto reply = Login(50001);
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::LOGIN_ACCEPTED);
    const auto accepted = decode_soup_login_accepted(reply->payload);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->session, soup_session_id(SOUP_SESSION));
    EXPECT_EQ(accepted->sequence_number, 1u);
}

TEST_F(ServerTest, LoginToAnotherSessionIsRejected) {
    ASSERT_TRUE(SendSegment(LoginBytes("OTHER"), 50011));

    const auto reply = Receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::LOGIN_REJECTED);
    ASSERT_EQ(reply->payload.size(), 1u);
    EXPECT_EQ(static_cast<char>(reply->payload[0]), soup_reject::kSessionNotAvailable);
}

TEST_F(ServerTest, DataBeforeLoginIsIgnored) {
    ASSERT_TRUE(Send(Enter(1, 'B', 500, 1'502'500), 50021));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(inbound_ring_->pop());
    EXPECT_FALSE(Receive(true, std::chrono::milliseconds(100)));
}

TEST_F(ServerTest, HandlesSeveralPacketsInOneSegment) {
    std::vector<std::byte> segment = LoginBytes();
    const std::vector<std::byte> data = DataBytes(Enter(1, 'B', 500, 1'502'500));
    segment.insert(segment.end(), data.begin(), data.end());

    ASSERT_TRUE(SendSegment(segment, 50031));

    const auto login = Receive();
    ASSERT_TRUE(login);
    EXPECT_EQ(login->type, SoupPacketType::LOGIN_ACCEPTED);
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->type, EngineRequestType::NEW);
}

TEST_F(ServerTest, IdleSessionGetsServerHeartbeats) {
    constexpr uint16_t kPort = 50041;
    ASSERT_TRUE(Login(kPort));

    // Earlier tests' sessions are still logged in and heartbeating too.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    std::optional<Reply> heartbeat;
    while (!heartbeat && std::chrono::steady_clock::now() < deadline) {
        auto reply = Receive(/*skip_heartbeats=*/false, std::chrono::milliseconds(100));
        if (reply && reply->transport.dst_port == kPort) heartbeat = std::move(reply);
    }
    ASSERT_TRUE(heartbeat);
    EXPECT_EQ(heartbeat->type, SoupPacketType::SERVER_HEARTBEAT);
}

TEST_F(ServerTest, NothingIsDeliveredAfterLogout) {
    constexpr uint16_t kPort = 50051;
    ASSERT_TRUE(Login(kPort));
    ASSERT_TRUE(Send(Enter(1, 'B', 500, 1'502'500), kPort));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);

    std::vector<std::byte> logout(8);
    logout.resize(encode_soup_empty(SoupPacketType::LOGOUT_REQUEST, logout));
    ASSERT_TRUE(SendSegment(logout, kPort));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EngineSend(EngineEvent{.order_id = request->order_id, .client_id = request->client_id, .price = 15'025,
                           .quantity = 500, .type = EngineEventType::ACCEPTED});
    EXPECT_FALSE(Receive(true, std::chrono::milliseconds(200)));
}

//=============================================================================
// OUCH through the gateway
//=============================================================================

TEST_F(ServerTest, EnterIsTranslatedAndAcceptedBackToTheClient) {
    constexpr uint16_t kPort = 50101;
    ASSERT_TRUE(Login(kPort));
    ASSERT_TRUE(Send(Enter(1, 'B', 500, 1'502'500), kPort));

    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->type, EngineRequestType::NEW);
    EXPECT_EQ(request->price, 15'025u);
    EXPECT_EQ(request->quantity, 500u);
    EXPECT_NE(request->recv_tsc, 0u);

    EngineSend(EngineEvent{.order_id = request->order_id, .client_id = request->client_id, .price = 15'025,
                           .quantity = 500, .type = EngineEventType::ACCEPTED});

    const auto reply = Receive();
    ASSERT_TRUE(reply);
    // Back where the order came from, from where it was sent to.
    EXPECT_EQ(reply->src.ip, kServer.ip);
    EXPECT_EQ(reply->dst.ip, kClient.ip);
    EXPECT_EQ(reply->transport.src_port, kServerPort);
    EXPECT_EQ(reply->transport.dst_port, kPort);

    ASSERT_EQ(reply->type, SoupPacketType::SEQUENCED_DATA);
    const OuchOutboundFrame ouch = reply->ouch();
    ASSERT_EQ(ouch.message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
    EXPECT_EQ(ouch.order_accepted.user_ref_num, 1u);
    EXPECT_EQ(ouch.order_accepted.order_reference_number, request->order_id);
    EXPECT_EQ(ouch.order_accepted.quantity, 500u);
    EXPECT_EQ(ouch.order_accepted.price, 1'502'500u);
    EXPECT_EQ(ouch.order_accepted.cl_ord_id, wire::alpha<14>("ORDER"));
}

TEST_F(ServerTest, InvalidEnterIsRejectedWithoutReachingTheEngine) {
    constexpr uint16_t kPort = 50201;
    ASSERT_TRUE(Login(kPort));
    ASSERT_TRUE(Send(Enter(1, 'B', 0, 1'502'500), kPort));

    const auto reply = Receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::SEQUENCED_DATA);
    const OuchOutboundFrame ouch = reply->ouch();
    ASSERT_EQ(ouch.message_type, OutboundOuchMessageType::ORDER_REJECTED);
    EXPECT_EQ(ouch.order_rejected.user_ref_num, 1u);
    EXPECT_EQ(ouch.order_rejected.reason, ouch_reject::kInvalidQuantity);
    EXPECT_FALSE(inbound_ring_->pop());
}

TEST_F(ServerTest, FillsAreRoutedToEachCounterparty) {
    constexpr uint16_t kSellerPort = 50301;
    constexpr uint16_t kBuyerPort  = 50302;
    ASSERT_TRUE(Login(kSellerPort));
    ASSERT_TRUE(Login(kBuyerPort));

    ASSERT_TRUE(Send(Enter(1, 'S', 100, 1'500'000), kSellerPort));
    const auto sell = EngineReceive();
    ASSERT_TRUE(sell);
    ASSERT_TRUE(Send(Enter(1, 'B', 100, 1'500'000), kBuyerPort));
    const auto buy = EngineReceive();
    ASSERT_TRUE(buy);
    ASSERT_NE(sell->client_id, buy->client_id); // separate sessions

    for (const EngineRequest &req : {*sell, *buy}) {
        EngineSend(EngineEvent{.order_id = req.order_id, .client_id = req.client_id, .price = req.price,
                               .quantity = req.quantity, .type = EngineEventType::ACCEPTED});
    }
    EngineSend(EngineEvent{.order_id = buy->order_id, .match_number = 1, .client_id = buy->client_id,
                           .price = 15'000, .quantity = 100, .type = EngineEventType::EXECUTED,
                           .reason = engine_reason::kRemovedLiquidity});
    EngineSend(EngineEvent{.order_id = sell->order_id, .match_number = 1, .client_id = sell->client_id,
                           .price = 15'000, .quantity = 100, .type = EngineEventType::EXECUTED,
                           .reason = engine_reason::kAddedLiquidity});

    std::vector<Reply> replies;
    while (replies.size() < 4) {
        auto reply = Receive();
        ASSERT_TRUE(reply) << "only " << replies.size() << " replies";
        replies.push_back(*reply);
    }

    int executions = 0;
    for (const Reply &reply : replies) {
        const OuchOutboundFrame ouch = reply.ouch();
        if (ouch.message_type != OutboundOuchMessageType::ORDER_EXECUTED) continue;
        ++executions;
        EXPECT_EQ(ouch.order_executed.execution_price, 1'500'000u);
        EXPECT_EQ(ouch.order_executed.match_number, 1u);
        if (reply.transport.dst_port == kBuyerPort) {
            EXPECT_EQ(ouch.order_executed.liquidity_flag, 'R');
        } else {
            EXPECT_EQ(reply.transport.dst_port, kSellerPort);
            EXPECT_EQ(ouch.order_executed.liquidity_flag, 'A');
        }
    }
    EXPECT_EQ(executions, 2);
}
