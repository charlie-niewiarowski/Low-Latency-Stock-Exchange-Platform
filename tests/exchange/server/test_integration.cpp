//
// Comprehensive integration test: multiple clients, mixed valid/invalid requests.
//
// The outbound ring is fed by FakeEngine (see server_fixture.hpp), which echoes
// every inbound order back as a SUCCESS response.  The test verifies the full
// pipeline end-to-end: TCP accept → read → parse → validate → ring I/O →
// serialize → send → client receives correct bytes.
//
// Scenario
// --------
//  Client A (cid=1)  — NEW BID, then CANCEL that order, then MODIFY it
//  Client B (cid=2)  — NEW ASK (market, price=0)
//  Client C (cid=3)  — NEW BID with price=0  → should get ERROR invalid order
//  Client D (cid=4)  — malformed frame        → should get ERROR malformed
//  Client E (cid=5)  — three consecutive NEWs on the same connection
//  Client F (cid=6)  — valid NEW BID followed immediately by an invalid request
//

#include "test_runner.hpp"
#include "server_fixture.hpp"

#include <cassert>
#include <vector>

//=============================================================================
// Helpers
//=============================================================================

static InboundMessage new_bid(ClientId cid, Price price, Quantity qty) {
    return {cid, 0, price, qty, MessageType::NEW, Side::BID, OrderType::LIMIT, 0};
}

static InboundMessage new_ask(ClientId cid, Price price, Quantity qty) {
    return {cid, 0, price, qty, MessageType::NEW, Side::ASK, OrderType::LIMIT, 0};
}

static InboundMessage cancel_order(ClientId cid, OrderId oid) {
    return {cid, oid, 0, 0, MessageType::CANCEL, Side::BID, OrderType::LIMIT, 0};
}

static InboundMessage modify_order(ClientId cid, OrderId oid, Price price, Quantity qty) {
    return {cid, oid, price, qty, MessageType::MODIFY, Side::BID, OrderType::LIMIT, 0};
}

//=============================================================================
// Test
//=============================================================================

static bool integration_MultipleClientsAndRequests() {
    ServerFixture f; f.start();

    // Client A: NEW → CANCEL → MODIFY
    {
        int fd = connect_client();
        ASSERT_NE(fd, -1);

        // NEW BID
        send_request(fd, new_bid(1, 150, 20));
        auto r1 = read_ok_response(fd);
        ASSERT_TRUE(r1.valid);
        ASSERT_EQ(r1.client_id, 1u);
        OrderId order_a = r1.order_id;
        ASSERT_NE(order_a, 0u);

        // CANCEL the same order
        send_request(fd, cancel_order(1, order_a));
        auto r2 = read_ok_response(fd);
        ASSERT_TRUE(r2.valid);
        ASSERT_EQ(r2.client_id, 1u);
        ASSERT_EQ(r2.order_id, order_a);

        // MODIFY that order (engine still echoes success even after cancel — that's
        // the fake engine's job, not the server's)
        send_request(fd, modify_order(1, order_a, 200, 10));
        auto r3 = read_ok_response(fd);
        ASSERT_TRUE(r3.valid);
        ASSERT_EQ(r3.client_id, 1u);
        ASSERT_EQ(r3.order_id, order_a);

        ::close(fd);
    }

    // Client B: NEW ASK at price=0 (market order — valid)
    {
        int fd = connect_client();
        ASSERT_NE(fd, -1);

        send_request(fd, new_ask(2, 0, 5));
        auto r = read_ok_response(fd);
        ASSERT_TRUE(r.valid);
        ASSERT_EQ(r.client_id, 2u);

        ::close(fd);
    }

    // Client C: NEW BID with price=0 (invalid)
    {
        int fd = connect_client();
        ASSERT_NE(fd, -1);

        send_request(fd, new_bid(3, 0, 10));
        auto r = read_error_response(fd);
        ASSERT_TRUE(r.valid);
        ASSERT_STREQ(r.message, err_invalid_order);

        ::close(fd);
    }

    // Client D: malformed frame (wrong prefix)
    {
        int fd = connect_client();
        ASSERT_NE(fd, -1);

        char bad[INBOUND_BSIZE]{};
        memcpy(bad, "BADFRAME\n", 9);
        send_raw(fd, bad, sizeof(bad));

        auto r = read_error_response(fd);
        ASSERT_TRUE(r.valid);
        ASSERT_STREQ(r.message, err_malformed_request);

        ::close(fd);
    }

    // Client E: three consecutive NEWs on the same connection
    {
        int fd = connect_client();
        ASSERT_NE(fd, -1);

        std::vector<OrderId> ids;
        for (int i = 0; i < 3; ++i) {
            send_request(fd, new_bid(5, 100 + static_cast<Price>(i * 10),
                                        static_cast<Quantity>(i + 1)));
            auto r = read_ok_response(fd);
            ASSERT_TRUE(r.valid);
            ASSERT_EQ(r.client_id, 5u);
            ids.push_back(r.order_id);
        }

        // All three order ids must be distinct (the fake engine increments its counter).
        ASSERT_NE(ids[0], ids[1]);
        ASSERT_NE(ids[1], ids[2]);
        ASSERT_NE(ids[0], ids[2]);

        ::close(fd);
    }

    // Client F: valid NEW then invalid NEW (over-quantity)
    {
        int fd = connect_client();
        ASSERT_NE(fd, -1);

        // Valid first
        send_request(fd, new_bid(6, 300, 50));
        auto r1 = read_ok_response(fd);
        ASSERT_TRUE(r1.valid);
        ASSERT_EQ(r1.client_id, 6u);

        // Invalid second (quantity exceeds 1e6)
        InboundMessage bad_qty = new_bid(6, 300, static_cast<Quantity>(1e6) + 1);
        send_request(fd, bad_qty);
        auto r2 = read_error_response(fd);
        ASSERT_TRUE(r2.valid);
        ASSERT_STREQ(r2.message, err_invalid_order);

        // Connection should still be alive for another valid request
        send_request(fd, new_ask(6, 0, 5));
        auto r3 = read_ok_response(fd);
        ASSERT_TRUE(r3.valid);
        ASSERT_EQ(r3.client_id, 6u);

        ::close(fd);
    }

    return true;
}

//=============================================================================
// main()
//=============================================================================

int main() {
    return run_tests("Integration", {
        {"multi-client mixed requests", integration_MultipleClientsAndRequests},
    });
}