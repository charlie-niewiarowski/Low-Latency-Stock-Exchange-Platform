//
// Created by cniew on 9/11/26.
//
// End-to-end through a real Server over the TESTING net_ring0 vdev: the test
// plays the client (OUCH packets in on the spare tx queue, replies out on
// the spare rx queue -- see Server's constructor) and the engine (popping
// EngineRequests, pushing EngineEvents).
//

#include "server.hpp"

#include <array>
#include <chrono>
#include <optional>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

namespace {

constexpr auto kTimeout = std::chrono::seconds(2);

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
auto PollUntil(F &&poll) -> decltype(poll()) {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
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
    using Reply = packet_factory<TCPHeader, OuchOutboundFrame>::DeserializedPacket;

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

    // Sends from the client's (ip, client_port) to the server.
    static bool Send(const OuchInboundFrame &ouch, const uint16_t client_port) {
        const TCPHeader tcp{.src_port = client_port, .dst_port = kServerPort, .flags = RTE_TCP_ACK_FLAG};
        dpdk::packet pkt = packet_factory<TCPHeader, OuchInboundFrame>::build_packet(
            *pool_, kClient, kServer, tcp, ouch);
        std::array<dpdk::packet, 1> pkts{std::move(pkt)};
        return client_tx_queue_->send_burst(pkts) == 1;
    }

    static bool SendRaw(std::string_view payload) {
        dpdk::packet pkt = pool_->get();
        const auto *bytes = reinterpret_cast<const std::byte *>(payload.data());
        if (!pkt || pkt.append(dpdk::packet::Payload(bytes, payload.size())) == nullptr) return false;
        std::array<dpdk::packet, 1> pkts{std::move(pkt)};
        return client_tx_queue_->send_burst(pkts) == 1;
    }

    static std::optional<Reply> Receive() {
        return PollUntil([&]() -> std::optional<Reply> {
            dpdk::packet_burst burst = client_rx_queue_->receive_burst(1);
            for (const dpdk::packet &packet : burst) {
                return packet_factory<TCPHeader, OuchOutboundFrame>::deserialize_packet(packet);
            }
            return std::nullopt;
        });
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

TEST_F(ServerTest, DropsPacketsThatAreNotOuch) {
    ASSERT_TRUE(client_tx_queue_);
    ASSERT_TRUE(SendRaw("some random bullshit"));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(inbound_ring_->pop());
    EXPECT_EQ(client_rx_queue_->receive_burst(1).size(), 0u);
}

TEST_F(ServerTest, EnterIsTranslatedAndAcceptedBackToTheClient) {
    constexpr uint16_t kPort = 50101;
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

    ASSERT_EQ(reply->payload.message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
    EXPECT_EQ(reply->payload.order_accepted.user_ref_num, 1u);
    EXPECT_EQ(reply->payload.order_accepted.order_reference_number, request->order_id);
    EXPECT_EQ(reply->payload.order_accepted.quantity, 500u);
    EXPECT_EQ(reply->payload.order_accepted.price, 1'502'500u);
    EXPECT_EQ(reply->payload.order_accepted.cl_ord_id, wire::alpha<14>("ORDER"));
}

TEST_F(ServerTest, InvalidEnterIsRejectedWithoutReachingTheEngine) {
    ASSERT_TRUE(Send(Enter(1, 'B', 0, 1'502'500), 50201));

    const auto reply = Receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->payload.message_type, OutboundOuchMessageType::ORDER_REJECTED);
    EXPECT_EQ(reply->payload.order_rejected.user_ref_num, 1u);
    EXPECT_EQ(reply->payload.order_rejected.reason, ouch_reject::kInvalidQuantity);
    EXPECT_FALSE(inbound_ring_->pop());
}

TEST_F(ServerTest, FillsAreRoutedToEachCounterparty) {
    constexpr uint16_t kSellerPort = 50301;
    constexpr uint16_t kBuyerPort  = 50302;

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

    for (const Reply &reply : replies) {
        if (reply.payload.message_type != OutboundOuchMessageType::ORDER_EXECUTED) continue;
        const OuchOrderExecuted &e = reply.payload.order_executed;
        EXPECT_EQ(e.execution_price, 1'500'000u);
        EXPECT_EQ(e.match_number, 1u);
        if (reply.transport.dst_port == kBuyerPort) {
            EXPECT_EQ(e.liquidity_flag, 'R');
        } else {
            EXPECT_EQ(reply.transport.dst_port, kSellerPort);
            EXPECT_EQ(e.liquidity_flag, 'A');
        }
    }
}
