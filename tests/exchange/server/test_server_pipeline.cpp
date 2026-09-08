//
// Isolation tests for specific server pipeline stages.
//
// Each test targets one function / code path in server.cpp:
//   readRequest    — buffer accumulation + state transition
//   parseRequest   — header validation + InboundMessage extraction
//   executeRequest — order validation + inbound-ring push
//   serializeResponse — OK vs ERROR response bytes
//   drainOutboundRing — routing outbound messages to the right connection
//   finishResponseCycle — connection reset so the same fd reuses cleanly
//
// The server runs on port 4000 in a background thread; a FakeEngine pumps
// the rings.  Tests are run sequentially to avoid port conflicts.
//

#include "test_runner.hpp"
#include "server_fixture.hpp"

//=============================================================================
// Parse Request
//=============================================================================

// Valid header → server parses the message, pushes to ring, engine responds, client gets OK.
static bool parseRequest_ValidHeader_Returns_OK() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    InboundMessage msg{1, 0, 100, 10, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, msg);

    auto resp = read_ok_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_EQ(resp.client_id, 1u);
    return true;
}

// "GARBAGE\n" prefix → parseRequest rejects, server sends ERROR immediately.
static bool parseRequest_WrongPrefix_Returns_ErrorMalformed() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    // Build a full INBOUND_BSIZE buffer with a wrong framing prefix.
    char buf[INBOUND_BSIZE]{};
    snprintf(buf, sizeof(buf), "GARBAGE\n");
    // pad remainder with zeros — the total size matches what readRequest needs
    // before it transitions to PARSING.
    send_raw(fd, buf, sizeof(buf));

    auto resp = read_error_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_STREQ(resp.message, err_malformed_request);
    return true;
}

// Header with correct prefix but wrong case should also be rejected.
static bool parseRequest_LowercasePrefix_Returns_ErrorMalformed() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    char buf[INBOUND_BSIZE]{};
    snprintf(buf, sizeof(buf), "exchange\n");
    send_raw(fd, buf, sizeof(buf));

    auto resp = read_error_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_STREQ(resp.message, err_malformed_request);
    return true;
}

// parseRequest must correctly copy the client_id from the wire bytes.
static bool parseRequest_ClientIdExtractedCorrectly() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    constexpr ClientId expected_cid = 42;
    InboundMessage msg{expected_cid, 0, 100, 10, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, msg);

    auto resp = read_ok_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_EQ(resp.client_id, expected_cid);
    return true;
}

//=============================================================================
// Exec Request
//=============================================================================

// A BID order with price=0 is invalid; executeRequest must reject it immediately.
static bool executeRequest_BidZeroPrice_Returns_ErrorInvalidOrder() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    InboundMessage msg{1, 0, 0 /*price=0*/, 10, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, msg);

    auto resp = read_error_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_STREQ(resp.message, err_invalid_order);
    return true;
}

// quantity > 1e6 is invalid.
static bool executeRequest_ExcessiveQuantity_Returns_ErrorInvalidOrder() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    InboundMessage msg{1, 0, 100, static_cast<Quantity>(1e6) + 1,
                       MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, msg);

    auto resp = read_error_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_STREQ(resp.message, err_invalid_order);
    return true;
}

// An unknown message_type (MATCH from client) is invalid.
static bool executeRequest_UnknownMessageType_Returns_ErrorInvalidOrder() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    InboundMessage msg{};
    msg.message_type = MessageType::MATCH;
    send_request(fd, msg);

    auto resp = read_error_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_STREQ(resp.message, err_invalid_order);
    return true;
}

// A valid CANCEL message must reach the inbound ring (fake engine echoes it back).
static bool executeRequest_ValidCancel_PushedToRing() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    constexpr OrderId target_oid = 77;
    InboundMessage msg{5, target_oid, 0, 0, MessageType::CANCEL, Side::BID, OrderType::LIMIT};
    send_request(fd, msg);

    auto resp = read_ok_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_EQ(resp.client_id, 5u);
    ASSERT_EQ(resp.order_id, target_oid);
    return true;
}

// A valid MODIFY message must reach the inbound ring.
static bool executeRequest_ValidModify_PushedToRing() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    constexpr OrderId mod_oid = 99;
    InboundMessage msg{7, mod_oid, 200, 5, MessageType::MODIFY, Side::BID, OrderType::LIMIT};
    send_request(fd, msg);

    auto resp = read_ok_response(fd);
    ::close(fd);
    ASSERT_TRUE(resp.valid);
    ASSERT_EQ(resp.client_id, 7u);
    ASSERT_EQ(resp.order_id, mod_oid);
    return true;
}

//=============================================================================
// Serialization
//=============================================================================

// Success path: response starts with "EXCHANGE\nOK\n" then contains the ids.
static bool serializeResponse_OkPath_ContainsCorrectIds() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    InboundMessage msg{10, 0, 500, 3, MessageType::NEW, Side::ASK, OrderType::LIMIT, 0};
    send_request(fd, msg);

    char raw[OUTBOUND_BSIZE + 1]{};
    ssize_t n = ::recv(fd, raw, sizeof(raw) - 1, 0);
    ::close(fd);

    ASSERT_TRUE(n > 0);

    const char* ok_hdr = "EXCHANGE\nOK\n";
    size_t hlen = strlen(ok_hdr);
    ASSERT_TRUE(static_cast<size_t>(n) >= hlen + sizeof(ClientId) + sizeof(OrderId));
    ASSERT_TRUE(memcmp(raw, ok_hdr, hlen) == 0);

    ClientId cid{};
    memcpy(&cid, raw + hlen, sizeof(cid));
    ASSERT_EQ(cid, 10u);
    return true;
}

// Error path: response starts with "EXCHANGE\nERROR\n" then the error string.
static bool serializeResponse_ErrorPath_ContainsErrorString() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    // Trigger invalid-order error: BID with price=0.
    InboundMessage msg{3, 0, 0, 5, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, msg);

    char raw[OUTBOUND_BSIZE + 1]{};
    ssize_t n = ::recv(fd, raw, sizeof(raw) - 1, 0);
    ::close(fd);

    ASSERT_TRUE(n > 0);

    const char* err_hdr = "EXCHANGE\nERROR\n";
    size_t hlen = strlen(err_hdr);
    ASSERT_TRUE(static_cast<size_t>(n) >= hlen);
    ASSERT_TRUE(memcmp(raw, err_hdr, hlen) == 0);

    std::string body(raw + hlen, static_cast<size_t>(n) - hlen);
    ASSERT_STREQ(body, err_invalid_order);
    return true;
}

//=============================================================================
// Drain Outbound Ring
//=============================================================================

// Two clients connected simultaneously; each must receive only their own response.
static bool drainOutboundRing_TwoClients_RoutedIndependently() {
    ServerFixture f; f.start();

    int fd_a = connect_client();
    int fd_b = connect_client();
    ASSERT_NE(fd_a, -1);
    ASSERT_NE(fd_b, -1);

    InboundMessage msg_a{100, 0, 50,  1, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    InboundMessage msg_b{200, 0, 75,  2, MessageType::NEW, Side::ASK, OrderType::LIMIT, 0};

    send_request(fd_a, msg_a);
    send_request(fd_b, msg_b);

    auto resp_a = read_ok_response(fd_a);
    auto resp_b = read_ok_response(fd_b);

    ::close(fd_a);
    ::close(fd_b);

    ASSERT_TRUE(resp_a.valid);
    ASSERT_TRUE(resp_b.valid);
    ASSERT_EQ(resp_a.client_id, 100u);
    ASSERT_EQ(resp_b.client_id, 200u);
    return true;
}

//=============================================================================
// Finish Response Cycle
//=============================================================================

// After a complete request-response round-trip, the same connection must be able
// to handle a second request (state resets to READING).
static bool finishResponseCycle_ConnectionReusedForSecondRequest() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    InboundMessage msg1{50, 0, 100, 5, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, msg1);
    auto resp1 = read_ok_response(fd);
    ASSERT_TRUE(resp1.valid);
    ASSERT_EQ(resp1.client_id, 50u);

    InboundMessage msg2{50, 0, 200, 3, MessageType::NEW, Side::ASK, OrderType::LIMIT, 0};
    send_request(fd, msg2);
    auto resp2 = read_ok_response(fd);
    ASSERT_TRUE(resp2.valid);
    ASSERT_EQ(resp2.client_id, 50u);

    ::close(fd);
    return true;
}

// An error response also resets the connection so the client can send again.
static bool finishResponseCycle_AfterErrorConnectionIsReady() {
    ServerFixture f; f.start();

    int fd = connect_client();
    ASSERT_NE(fd, -1);

    // First request: invalid (BID price=0).
    InboundMessage bad{60, 0, 0, 5, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, bad);
    auto err = read_error_response(fd);
    ASSERT_TRUE(err.valid);

    // Second request: valid.
    InboundMessage good{60, 0, 100, 5, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
    send_request(fd, good);
    auto ok = read_ok_response(fd);
    ASSERT_TRUE(ok.valid);
    ASSERT_EQ(ok.client_id, 60u);

    ::close(fd);
    return true;
}

//=============================================================================
// main()
//=============================================================================

int main() {
    return run_tests("Server Pipeline", {
        // parseRequest
        {"parseRequest: valid header → OK",                 parseRequest_ValidHeader_Returns_OK},
        {"parseRequest: wrong prefix → ERROR malformed",    parseRequest_WrongPrefix_Returns_ErrorMalformed},
        {"parseRequest: lowercase prefix → ERROR malformed",parseRequest_LowercasePrefix_Returns_ErrorMalformed},
        {"parseRequest: client_id extracted correctly",     parseRequest_ClientIdExtractedCorrectly},
        // executeRequest
        {"executeRequest: BID price=0 → ERROR invalid",     executeRequest_BidZeroPrice_Returns_ErrorInvalidOrder},
        {"executeRequest: qty>1e6 → ERROR invalid",         executeRequest_ExcessiveQuantity_Returns_ErrorInvalidOrder},
        {"executeRequest: MATCH type → ERROR invalid",      executeRequest_UnknownMessageType_Returns_ErrorInvalidOrder},
        {"executeRequest: valid CANCEL pushed to ring",     executeRequest_ValidCancel_PushedToRing},
        {"executeRequest: valid MODIFY pushed to ring",     executeRequest_ValidModify_PushedToRing},
        // serializeResponse
        {"serializeResponse: OK contains correct ids",      serializeResponse_OkPath_ContainsCorrectIds},
        {"serializeResponse: ERROR contains error string",  serializeResponse_ErrorPath_ContainsErrorString},
        // drainOutboundRing
        {"drainOutboundRing: two clients routed independently", drainOutboundRing_TwoClients_RoutedIndependently},
        // finishResponseCycle
        {"finishResponseCycle: connection reused after OK", finishResponseCycle_ConnectionReusedForSecondRequest},
        {"finishResponseCycle: connection reused after error", finishResponseCycle_AfterErrorConnectionIsReady},
    });
}