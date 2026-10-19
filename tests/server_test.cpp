//
// Created by cniew on 9/11/26.
//
// End-to-end through a real Server over the TESTING net_ring0 vdev: the test
// plays the client -- real TCP connections, scripted by tcp_peer.hpp,
// carrying SoupBinTCP-framed OUCH -- and the engine (popping EngineRequests,
// pushing EngineEvents).
//
// server-tests builds with small SoupBinTCP limits (see tests/CMakeLists.txt):
// a 4 KiB replay log per session and 2 s login/idle timeouts.
//

#include "server.hpp"
#include "tcp_peer.hpp"

#include <immintrin.h>

#include <array>
#include <chrono>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

using tcp_peer::SoupReply;
using tcp_peer::TcpPeer;

namespace {

constexpr auto kTimeout = tcp_peer::kTimeout;

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

std::vector<std::byte> LoginBytes(const std::string_view username, const uint64_t sequence = 1,
                                  const char *session = "") {
    const SoupLoginRequest login{.username = wire::alpha<6>(username), .password = wire::alpha<10>("test"),
                                 .requested_session = wire::alpha<10>(session),
                                 .requested_sequence_number = sequence};
    std::vector<std::byte> out(64);
    out.resize(encode_soup_login_request(login, out));
    return out;
}

std::vector<std::byte> DataBytes(const OuchInboundFrame &ouch) {
    std::vector<std::byte> out(256);
    out.resize(encode_soup_unsequenced(ouch, out));
    return out;
}

std::vector<std::byte> EmptyBytes(const SoupPacketType type) {
    std::vector<std::byte> out(kSoupHeaderLen);
    encode_soup_empty(type, out);
    return out;
}

OuchOutboundFrame Ouch(const SoupReply &reply) { return decode_outbound_ouch(reply.payload); }

std::optional<SoupLoginAccepted> Accepted(const std::optional<SoupReply> &reply) {
    if (!reply || reply->type != SoupPacketType::LOGIN_ACCEPTED) return std::nullopt;
    return decode_soup_login_accepted(reply->payload);
}

} // namespace

// The EAL can only be brought up once per process, so the whole suite shares
// one Server. Each test connects from its own ports and logs in under its own
// usernames -- its own sessions, with UserRefNums starting at 1 -- and reads
// OrderIds/ClientIds off the requests rather than assuming them.
class ServerTest : public testing::Test {
protected:
    static void SetUpTestSuite() {
        inbound_ring_ = std::make_shared<InboundRing>(RINGBUF_SIZE);
        outbound_ring_ = std::make_shared<OutboundRing>(RINGBUF_SIZE);
        const ServerNetwork network = ServerNetwork::loopback_test();
        server_ = std::make_unique<Server>(inbound_ring_, outbound_ring_, network);
        dpdk::port &port = server_->dpdk_port_;
        wire_ = std::make_unique<tcp_peer::Wire>(
            port.claim_tx_queue(uint16_t{0}), port.claim_rx_queue(network.control_tx),
            port.claim_rx_queue(network.data_tx), port.get_pool(), port.mac_address(), network.ipv4,
            network.listen_port);
        server_->run();
    }

    static void TearDownTestSuite() {
        server_->stop();
        wire_.reset();
        server_.reset();
        outbound_ring_.reset();
        inbound_ring_.reset();
    }

    // Nothing a previous test left in flight is this test's business.
    void SetUp() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        while (inbound_ring_->pop()) {}
        wire_->drain();
    }

    static TcpPeer Connect(const uint16_t port) {
        TcpPeer peer(*wire_, port);
        EXPECT_TRUE(peer.connect()) << "handshake from port " << port;
        return peer;
    }

    // Logs in as username; returns the Login Accepted.
    static std::optional<SoupLoginAccepted> Login(TcpPeer &peer, const std::string_view username,
                                                  const uint64_t sequence = 1) {
        if (!peer.send(LoginBytes(username, sequence))) return std::nullopt;
        return Accepted(peer.receive());
    }

    static bool Send(TcpPeer &peer, const OuchInboundFrame &ouch) { return peer.send(DataBytes(ouch)); }

    // The test as the engine.
    static std::optional<EngineRequest> EngineReceive() {
        return PollUntil([&] { return inbound_ring_->pop(); });
    }

    static void EngineSend(const EngineEvent &event) { ASSERT_TRUE(outbound_ring_->push(event)); }

    static void Accept(const EngineRequest &request) {
        EngineSend(EngineEvent{.order_id = request.order_id, .client_id = request.client_id, .price = request.price,
                               .quantity = request.quantity, .type = EngineEventType::ACCEPTED});
    }

    static void Execute(const EngineRequest &request, const uint64_t match_number, const uint32_t quantity = 1) {
        EngineSend(EngineEvent{.order_id = request.order_id, .match_number = match_number,
                               .client_id = request.client_id, .price = request.price, .quantity = quantity,
                               .type = EngineEventType::EXECUTED, .reason = engine_reason::kAddedLiquidity});
    }

    static const LatencyHandler &Latency() { return server_->latency_; }

    static inline std::shared_ptr<InboundRing> inbound_ring_;
    static inline std::shared_ptr<OutboundRing> outbound_ring_;
    static inline std::unique_ptr<Server> server_;
    static inline std::unique_ptr<tcp_peer::Wire> wire_;
};

TEST_F(ServerTest, DropsPacketsThatAreNotSoupBinTcp) {
    const std::string_view junk = "some random bullshit";
    ASSERT_TRUE(wire_->inject_raw(std::as_bytes(std::span{junk})));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(inbound_ring_->pop());
}

//=============================================================================
// SoupBinTCP session
//=============================================================================

TEST_F(ServerTest, LoginIsAcceptedIntoTheSimSession) {
    TcpPeer peer = Connect(50001);
    const auto accepted = Login(peer, "S50001");
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->session, soup_session_id(SOUP_SESSION));
    EXPECT_EQ(accepted->sequence_number, 1u);
}

TEST_F(ServerTest, LoginToAnotherSessionIsRejectedAndClosed) {
    TcpPeer peer = Connect(50011);
    ASSERT_TRUE(peer.send(LoginBytes("S50011", 1, "OTHER")));

    const auto reply = peer.receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::LOGIN_REJECTED);
    ASSERT_EQ(reply->payload.size(), 1u);
    EXPECT_EQ(static_cast<char>(reply->payload[0]), soup_reject::kSessionNotAvailable);
    EXPECT_TRUE(peer.wait_closed()); // the server closes after Login Rejected
}

TEST_F(ServerTest, BlankUsernameIsNotAuthorized) {
    TcpPeer peer = Connect(50012);
    ASSERT_TRUE(peer.send(LoginBytes("")));

    const auto reply = peer.receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::LOGIN_REJECTED);
    EXPECT_EQ(static_cast<char>(reply->payload[0]), soup_reject::kNotAuthorized);
}

TEST_F(ServerTest, DataBeforeLoginIsIgnored) {
    TcpPeer peer = Connect(50021);
    ASSERT_TRUE(Send(peer, Enter(1, 'B', 500, 1'502'500)));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(inbound_ring_->pop());
    EXPECT_FALSE(peer.receive(true, std::chrono::milliseconds(100)));
}

TEST_F(ServerTest, HandlesSeveralPacketsInOneSegment) {
    TcpPeer peer = Connect(50031);
    std::vector<std::byte> segment = LoginBytes("S50031");
    const std::vector<std::byte> data = DataBytes(Enter(1, 'B', 500, 1'502'500));
    segment.insert(segment.end(), data.begin(), data.end());
    ASSERT_TRUE(peer.send(segment));

    EXPECT_TRUE(Accepted(peer.receive()));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->type, EngineRequestType::NEW);
}

TEST_F(ServerTest, ReassemblesPacketsSplitAcrossSegments) {
    TcpPeer peer = Connect(50032);
    std::vector<std::byte> stream = LoginBytes("S50032");
    const std::vector<std::byte> data = DataBytes(Enter(1, 'B', 500, 1'502'500));
    const size_t data_at = stream.size();
    stream.insert(stream.end(), data.begin(), data.end());

    // Cut mid-length-field, mid-login, across the login/data boundary, and mid-data.
    const std::array<size_t, 6> cuts{0, 1, 20, data_at + 7, stream.size() - 3, stream.size()};
    const std::span<const std::byte> bytes{stream};
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        ASSERT_TRUE(peer.send(bytes.subspan(cuts[i], cuts[i + 1] - cuts[i])));
    }

    EXPECT_TRUE(Accepted(peer.receive()));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->type, EngineRequestType::NEW);
    EXPECT_EQ(request->quantity, 500u);
}

TEST_F(ServerTest, OversizedPacketEndsTheConnection) {
    TcpPeer peer = Connect(50033);
    ASSERT_TRUE(Login(peer, "S50033"));

    // Announces 4000 bytes and sends a few: more than a client packet may be.
    std::vector<std::byte> start(16, std::byte{'x'});
    start[0] = std::byte{0x0F};
    start[1] = std::byte{0xA0};
    start[2] = std::byte{'U'};
    ASSERT_TRUE(peer.send(start));
    EXPECT_TRUE(peer.wait_closed());
}

TEST_F(ServerTest, IdleSessionGetsServerHeartbeats) {
    TcpPeer peer = Connect(50041);
    ASSERT_TRUE(Login(peer, "S50041"));

    const auto heartbeat = peer.receive(/*skip_heartbeats=*/false, std::chrono::milliseconds(1500));
    ASSERT_TRUE(heartbeat);
    EXPECT_EQ(heartbeat->type, SoupPacketType::SERVER_HEARTBEAT);
}

TEST_F(ServerTest, ClientHeartbeatsKeepASessionAlive) {
    TcpPeer peer = Connect(50042);
    ASSERT_TRUE(Login(peer, "S50042"));

    // Past the idle timeout, heartbeating every half second.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(SOUP_IDLE_TIMEOUT_MS + 1000);
    while (std::chrono::steady_clock::now() < until) {
        ASSERT_TRUE(peer.send(EmptyBytes(SoupPacketType::CLIENT_HEARTBEAT)));
        peer.receive(false, std::chrono::milliseconds(500));
    }
    EXPECT_FALSE(peer.fin_received());
}

TEST_F(ServerTest, SilentClientIsDisconnected) {
    TcpPeer peer = Connect(50043);
    ASSERT_TRUE(Login(peer, "S50043"));
    EXPECT_TRUE(peer.wait_closed(std::chrono::milliseconds(SOUP_IDLE_TIMEOUT_MS + 1000)));
}

TEST_F(ServerTest, ConnectionThatNeverLogsInIsDropped) {
    TcpPeer peer = Connect(50044);
    EXPECT_TRUE(peer.wait_closed(std::chrono::milliseconds(SOUP_LOGIN_TIMEOUT_MS + 1000)));
}

TEST_F(ServerTest, LogoutClosesTheConnection) {
    TcpPeer peer = Connect(50051);
    ASSERT_TRUE(Login(peer, "S50051"));
    ASSERT_TRUE(peer.send(EmptyBytes(SoupPacketType::LOGOUT_REQUEST)));
    EXPECT_TRUE(peer.wait_closed());
}

TEST_F(ServerTest, SecondConcurrentLoginIsRejected) {
    TcpPeer first = Connect(50061);
    ASSERT_TRUE(Login(first, "S50061"));

    // Same session, case-insensitively.
    TcpPeer second = Connect(50062);
    ASSERT_TRUE(second.send(LoginBytes("s50061")));
    const auto reply = second.receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::LOGIN_REJECTED);
    EXPECT_EQ(static_cast<char>(reply->payload[0]), soup_reject::kNotAuthorized);
    EXPECT_FALSE(first.fin_received());
}

//=============================================================================
// Re-login and replay
//=============================================================================

TEST_F(ServerTest, ReloginResumesTheSessionAndReplaysWhatWasMissed) {
    TcpPeer first = Connect(50101);
    ASSERT_TRUE(Login(first, "S50101"));
    ASSERT_TRUE(Send(first, Enter(1, 'B', 1'000, 1'502'500)));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    Accept(*request);
    const auto accepted = first.receive();
    ASSERT_TRUE(accepted);
    ASSERT_EQ(Ouch(*accepted).message_type, OutboundOuchMessageType::ORDER_ACCEPTED); // sequence 1

    // Gone; meanwhile the resting order trades twice.
    ASSERT_TRUE(first.send_rst());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Execute(*request, 1);
    Execute(*request, 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpPeer second = Connect(50102);
    const auto login = Login(second, "S50101", /*sequence=*/2);
    ASSERT_TRUE(login);
    EXPECT_EQ(login->sequence_number, 2u);
    for (const uint64_t match : {1u, 2u}) {
        const auto replayed = second.receive();
        ASSERT_TRUE(replayed);
        const OuchOutboundFrame ouch = Ouch(*replayed);
        ASSERT_EQ(ouch.message_type, OutboundOuchMessageType::ORDER_EXECUTED);
        EXPECT_EQ(ouch.order_executed.match_number, match);
    }

    // Same session: same ClientId, UserRefNums carry on.
    ASSERT_TRUE(Send(second, Enter(2, 'B', 100, 1'502'500)));
    const auto next = EngineReceive();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->client_id, request->client_id);
}

TEST_F(ServerTest, ReloginFromOneReplaysEverything) {
    TcpPeer first = Connect(50111);
    ASSERT_TRUE(Login(first, "S50111"));
    ASSERT_TRUE(Send(first, Enter(1, 'B', 1'000, 1'502'500)));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    Accept(*request);
    ASSERT_TRUE(first.receive());
    ASSERT_TRUE(first.send(EmptyBytes(SoupPacketType::LOGOUT_REQUEST)));
    ASSERT_TRUE(first.wait_closed());

    TcpPeer second = Connect(50112);
    const auto login = Login(second, "S50111", 1);
    ASSERT_TRUE(login);
    EXPECT_EQ(login->sequence_number, 1u);
    const auto replayed = second.receive();
    ASSERT_TRUE(replayed);
    EXPECT_EQ(Ouch(*replayed).message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
}

TEST_F(ServerTest, ReloginWithZeroStartsAtTheNextMessage) {
    TcpPeer first = Connect(50121);
    ASSERT_TRUE(Login(first, "S50121"));
    ASSERT_TRUE(Send(first, Enter(1, 'B', 1'000, 1'502'500)));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    Accept(*request);
    ASSERT_TRUE(first.receive());
    ASSERT_TRUE(first.send_rst());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    TcpPeer second = Connect(50122);
    const auto login = Login(second, "S50121", /*sequence=*/0);
    ASSERT_TRUE(login);
    EXPECT_EQ(login->sequence_number, 2u);
    EXPECT_FALSE(second.receive(true, std::chrono::milliseconds(100))); // nothing replayed
}

TEST_F(ServerTest, ReplayOlderThanTheLogHoldsIsRejected) {
    TcpPeer first = Connect(50131);
    ASSERT_TRUE(Login(first, "S50131"));
    ASSERT_TRUE(Send(first, Enter(1, 'B', 10'000, 1'502'500)));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    ASSERT_TRUE(first.send_rst());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Far more than the 4 KiB log holds, while nobody is connected:
    // sequence 1 is the Accepted, 2..201 the executions.
    Accept(*request);
    for (uint64_t match = 1; match <= 200; ++match) Execute(*request, match);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    TcpPeer second = Connect(50132);
    ASSERT_TRUE(second.send(LoginBytes("S50131", 1)));
    const auto reply = second.receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::LOGIN_REJECTED);
    EXPECT_EQ(static_cast<char>(reply->payload[0]), soup_reject::kSessionNotAvailable);
    ASSERT_TRUE(second.wait_closed());

    // What's still held replays.
    TcpPeer third = Connect(50133);
    const auto login = Login(third, "S50131", 195);
    ASSERT_TRUE(login);
    EXPECT_EQ(login->sequence_number, 195u);
    for (uint64_t match = 194; match <= 200; ++match) {
        const auto replayed = third.receive();
        ASSERT_TRUE(replayed);
        EXPECT_EQ(Ouch(*replayed).order_executed.match_number, match);
    }
}

TEST_F(ServerTest, ClientThatFallsAWholeLogBehindIsDisconnected) {
    TcpPeer peer = Connect(50141);
    ASSERT_TRUE(Login(peer, "S50141"));
    ASSERT_TRUE(Send(peer, Enter(1, 'B', 10'000, 1'502'500)));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    // A zero window: the server can't send it anything, so it all backs up.
    ASSERT_TRUE(peer.set_window(0));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Accept(*request);
    for (uint64_t match = 1; match <= 200; ++match) Execute(*request, match);
    EXPECT_TRUE(peer.wait_closed());
}

//=============================================================================
// OUCH through the gateway
//=============================================================================

TEST_F(ServerTest, EnterIsTranslatedAndAcceptedBackToTheClient) {
    TcpPeer peer = Connect(50201);
    ASSERT_TRUE(Login(peer, "S50201"));
    ASSERT_TRUE(Send(peer, Enter(1, 'B', 500, 1'502'500)));

    const auto request = EngineReceive();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->type, EngineRequestType::NEW);
    EXPECT_EQ(request->price, 15'025u);
    EXPECT_EQ(request->quantity, 500u);
    EXPECT_NE(request->recv_tsc, 0u);

    Accept(*request);

    const auto reply = peer.receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::SEQUENCED_DATA);
    const OuchOutboundFrame ouch = Ouch(*reply);
    ASSERT_EQ(ouch.message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
    EXPECT_EQ(ouch.order_accepted.user_ref_num, 1u);
    EXPECT_EQ(ouch.order_accepted.order_reference_number, request->order_id);
    EXPECT_EQ(ouch.order_accepted.quantity, 500u);
    EXPECT_EQ(ouch.order_accepted.price, 1'502'500u);
    EXPECT_EQ(ouch.order_accepted.cl_ord_id, wire::alpha<14>("ORDER"));
}

TEST_F(ServerTest, OneLatencySamplePerAnsweredRequest) {
    TcpPeer peer = Connect(50211);
    ASSERT_TRUE(Login(peer, "S50211"));
    ASSERT_TRUE(Send(peer, Enter(1, 'B', 500, 1'502'500)));
    const auto request = EngineReceive();
    ASSERT_TRUE(request);

    const uint64_t before = Latency().samples_seen();

    // The answer, then a fill: only the answer is a sample.
    EngineEvent accepted{.order_id = request->order_id, .client_id = request->client_id, .price = 15'025,
                         .quantity = 500, .type = EngineEventType::ACCEPTED, .answers_request = true};
    accepted.read_begin_tsc  = request->read_begin_tsc;
    accepted.recv_tsc        = request->recv_tsc;
    accepted.server_push_tsc = request->server_push_tsc;
    accepted.engine_pop_tsc  = __rdtsc();
    accepted.engine_push_tsc = __rdtsc();
    EngineSend(accepted);
    Execute(*request, 1, 100);

    ASSERT_TRUE(peer.receive());
    ASSERT_TRUE(peer.receive());
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // recorded after the send returns
    EXPECT_EQ(Latency().samples_seen(), before + 1);
}

TEST_F(ServerTest, InvalidEnterIsRejectedWithoutReachingTheEngine) {
    TcpPeer peer = Connect(50221);
    ASSERT_TRUE(Login(peer, "S50221"));
    ASSERT_TRUE(Send(peer, Enter(1, 'B', 0, 1'502'500)));

    const auto reply = peer.receive();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->type, SoupPacketType::SEQUENCED_DATA);
    const OuchOutboundFrame ouch = Ouch(*reply);
    ASSERT_EQ(ouch.message_type, OutboundOuchMessageType::ORDER_REJECTED);
    EXPECT_EQ(ouch.order_rejected.user_ref_num, 1u);
    EXPECT_EQ(ouch.order_rejected.reason, ouch_reject::kInvalidQuantity);
    EXPECT_FALSE(inbound_ring_->pop());
}

TEST_F(ServerTest, FillsAreRoutedToEachCounterparty) {
    TcpPeer seller = Connect(50231);
    TcpPeer buyer = Connect(50232);
    ASSERT_TRUE(Login(seller, "S50231"));
    ASSERT_TRUE(Login(buyer, "S50232"));

    ASSERT_TRUE(Send(seller, Enter(1, 'S', 100, 1'500'000)));
    const auto sell = EngineReceive();
    ASSERT_TRUE(sell);
    ASSERT_TRUE(Send(buyer, Enter(1, 'B', 100, 1'500'000)));
    const auto buy = EngineReceive();
    ASSERT_TRUE(buy);
    ASSERT_NE(sell->client_id, buy->client_id); // separate sessions

    Accept(*sell);
    Accept(*buy);
    EngineSend(EngineEvent{.order_id = buy->order_id, .match_number = 1, .client_id = buy->client_id,
                           .price = 15'000, .quantity = 100, .type = EngineEventType::EXECUTED,
                           .reason = engine_reason::kRemovedLiquidity});
    EngineSend(EngineEvent{.order_id = sell->order_id, .match_number = 1, .client_id = sell->client_id,
                           .price = 15'000, .quantity = 100, .type = EngineEventType::EXECUTED,
                           .reason = engine_reason::kAddedLiquidity});

    for (const auto &[peer, liquidity] : {std::pair{&buyer, 'R'}, std::pair{&seller, 'A'}}) {
        const auto ack = peer->receive();
        ASSERT_TRUE(ack);
        EXPECT_EQ(Ouch(*ack).message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
        const auto fill = peer->receive();
        ASSERT_TRUE(fill);
        const OuchOutboundFrame ouch = Ouch(*fill);
        ASSERT_EQ(ouch.message_type, OutboundOuchMessageType::ORDER_EXECUTED);
        EXPECT_EQ(ouch.order_executed.execution_price, 1'500'000u);
        EXPECT_EQ(ouch.order_executed.match_number, 1u);
        EXPECT_EQ(ouch.order_executed.liquidity_flag, liquidity);
    }
}
