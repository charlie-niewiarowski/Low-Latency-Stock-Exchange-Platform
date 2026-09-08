//
// Comprehensive unit tests for the matching engine.
//
// Tests are single-threaded: engine.step() synchronously drains the inbound
// ring and processes every pending message, so no threads are spawned.
// Each test function constructs its own EngineFixture (fresh engine + rings)
// so there is no shared state between tests.
//

#include "test_runner.hpp"
#include "engine.hpp"

#include <cassert>

// ── Message helpers ───────────────────────────────────────────────────────────

static InboundMessage make_new(ClientId cid, Side side, Price price, Quantity qty) {
    return {.order_id = 0, .client_id = cid, .price = price, .quantity = qty,
            .message_type = MessageType::NEW, .side = side, .order_type = OrderType::LIMIT};
}

static InboundMessage make_new_market(ClientId cid, Side side, Quantity qty) {
    return {.order_id = 0, .client_id = cid, .price = 0, .quantity = qty,
            .message_type = MessageType::NEW, .side = side, .order_type = OrderType::MARKET};
}

static InboundMessage make_cancel(ClientId cid, OrderId oid) {
    return {.order_id = oid, .client_id = cid, .price = 0, .quantity = 0,
            .message_type = MessageType::CANCEL, .side = Side::BID, .order_type = OrderType::LIMIT};
}

static InboundMessage make_modify(ClientId cid, OrderId oid, Side side, Price price, Quantity qty) {
    return {.order_id = oid, .client_id = cid, .price = price, .quantity = qty,
            .message_type = MessageType::MODIFY, .side = side, .order_type = OrderType::LIMIT};
}

// ── Test fixture ──────────────────────────────────────────────────────────────

struct EF {
    InboundRing  in{64};
    OutboundRing out{64};
    std::atomic<bool> stop_flag{false};
    Engine       eng{in, out, stop_flag};

    // Push one message and synchronously process it.
    void submit(const InboundMessage& msg) {
        const bool pushed = in.push(msg);
        assert(pushed); // test ring (capacity 64) never fills within a single test
        eng.step();
    }

    // Returns the order_id that will be assigned to the next NEW message.
    OrderId next_id() const { return eng.next_order_id(); }

    // Drain all messages currently in the outbound ring.
    std::vector<OutboundMessage> drain() {
        std::vector<OutboundMessage> v;
        while (auto m = eng.pop_outbound()) v.push_back(*m);
        return v;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// NEW order — book placement
// ═════════════════════════════════════════════════════════════════════════════

static bool new_bid_rests_on_book() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 10));
    ASSERT_EQ(f.eng.order_count(), 1u);
    ASSERT_EQ(f.eng.bid_level_count(), 1u);
    ASSERT_EQ(f.eng.bids_at(100), 1u);
    ASSERT_EQ(f.eng.ask_level_count(), 0u);
    return true;
}

static bool new_ask_rests_on_book() {
    EF f;
    f.submit(make_new(1, Side::ASK, 200, 5));
    ASSERT_EQ(f.eng.order_count(), 1u);
    ASSERT_EQ(f.eng.ask_level_count(), 1u);
    ASSERT_EQ(f.eng.asks_at(200), 1u);
    ASSERT_EQ(f.eng.bid_level_count(), 0u);
    return true;
}

static bool new_sends_success_outbound() {
    EF f;
    f.submit(make_new(42, Side::BID, 100, 10));
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].client_id, 42u);
    ASSERT_TRUE(out[0].message_type == MessageType::NEW);
    ASSERT_TRUE(out[0].status == Status::SUCCESS);
    return true;
}

static bool new_outbound_carries_assigned_order_id() {
    EF f;
    OrderId expected = f.next_id();   // 0 before any orders
    f.submit(make_new(1, Side::BID, 100, 5));
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].order_id, expected);
    return true;
}

static bool new_order_ids_are_sequential() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 5));
    f.submit(make_new(2, Side::BID, 100, 5));
    auto out = f.drain();
    ASSERT_EQ(out.size(), 2u);
    ASSERT_EQ(out[0].order_id, 0u);
    ASSERT_EQ(out[1].order_id, 1u);
    return true;
}

static bool two_bids_at_same_price_level() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 5));
    f.submit(make_new(2, Side::BID, 100, 5));
    ASSERT_EQ(f.eng.order_count(), 2u);
    ASSERT_EQ(f.eng.bid_level_count(), 1u);
    ASSERT_EQ(f.eng.bids_at(100), 2u);
    return true;
}

static bool bids_at_different_price_levels() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 5));
    f.submit(make_new(2, Side::BID, 110, 5));
    ASSERT_EQ(f.eng.bid_level_count(), 2u);
    ASSERT_EQ(f.eng.bids_at(100), 1u);
    ASSERT_EQ(f.eng.bids_at(110), 1u);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// CANCEL
// ═════════════════════════════════════════════════════════════════════════════

static bool cancel_bid_removes_from_book() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_cancel(1, id));
    ASSERT_EQ(f.eng.order_count(), 0u);
    ASSERT_EQ(f.eng.bid_level_count(), 0u);
    ASSERT_FALSE(f.eng.has_order(id));
    return true;
}

static bool cancel_ask_removes_from_book() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::ASK, 200, 10));
    f.drain();
    f.submit(make_cancel(1, id));
    ASSERT_EQ(f.eng.order_count(), 0u);
    ASSERT_EQ(f.eng.ask_level_count(), 0u);
    ASSERT_FALSE(f.eng.has_order(id));
    return true;
}

static bool cancel_sends_success_outbound() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_cancel(1, id));
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::SUCCESS);
    ASSERT_EQ(out[0].client_id, 1u);
    ASSERT_EQ(out[0].order_id, id);
    ASSERT_TRUE(out[0].message_type == MessageType::CANCEL);
    return true;
}

static bool cancel_only_removes_targeted_order() {
    EF f;
    OrderId id1 = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    OrderId id2 = f.next_id();
    f.submit(make_new(2, Side::BID, 100, 5));
    f.drain();
    f.submit(make_cancel(1, id1));
    ASSERT_EQ(f.eng.order_count(), 1u);
    ASSERT_FALSE(f.eng.has_order(id1));
    ASSERT_TRUE(f.eng.has_order(id2));
    ASSERT_EQ(f.eng.bids_at(100), 1u);
    ASSERT_EQ(f.eng.bid_level_count(), 1u);
    return true;
}

static bool cancel_nonexistent_order_sends_failure() {
    EF f;
    f.submit(make_cancel(1, 9999));
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::FAILURE);
    return true;
}

static bool cancel_wrong_client_sends_failure_and_preserves_order() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_cancel(99, id));   // wrong client_id
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::FAILURE);
    ASSERT_EQ(f.eng.order_count(), 1u);   // order untouched
    ASSERT_TRUE(f.eng.has_order(id));
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// MODIFY
// ═════════════════════════════════════════════════════════════════════════════

static bool modify_nonexistent_order_sends_failure() {
    EF f;
    f.submit(make_modify(1, 9999, Side::BID, 100, 10));
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::FAILURE);
    return true;
}

static bool modify_wrong_client_sends_failure() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_modify(99, id, Side::BID, 110, 10));   // wrong client
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::FAILURE);
    return true;
}

static bool modify_wrong_side_sends_failure() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_modify(1, id, Side::ASK, 110, 10));    // original is BID, not ASK
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::FAILURE);
    ASSERT_EQ(f.eng.order_count(), 1u);
    return true;
}

static bool modify_price_change_moves_order_to_new_level() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_modify(1, id, Side::BID, 150, 10));    // price 100 → 150
    f.drain();
    ASSERT_TRUE(f.eng.has_order(id));
    ASSERT_EQ(f.eng.bids_at(100), 0u);
    ASSERT_EQ(f.eng.bids_at(150), 1u);
    ASSERT_EQ(f.eng.bid_level_count(), 1u);
    return true;
}

static bool modify_sends_success_with_correct_fields() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_modify(1, id, Side::BID, 150, 10));
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::SUCCESS);
    ASSERT_EQ(out[0].client_id, 1u);
    ASSERT_EQ(out[0].order_id, id);
    ASSERT_TRUE(out[0].message_type == MessageType::MODIFY);
    return true;
}

static bool modify_quantity_change_updates_order() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_modify(1, id, Side::BID, 100, 20));    // qty 10 → 20
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::SUCCESS);
    ASSERT_TRUE(f.eng.has_order(id));
    ASSERT_EQ(f.eng.bids_at(100), 1u);
    return true;
}

static bool modify_same_price_and_qty_keeps_order() {
    EF f;
    OrderId id = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_modify(1, id, Side::BID, 100, 10));    // no-op values
    auto out = f.drain();
    ASSERT_EQ(out.size(), 1u);
    ASSERT_TRUE(out[0].status == Status::SUCCESS);
    ASSERT_TRUE(f.eng.has_order(id));
    ASSERT_EQ(f.eng.bids_at(100), 1u);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Matching — limit orders
// ═════════════════════════════════════════════════════════════════════════════

static bool no_cross_nothing_filled() {
    EF f;
    f.submit(make_new(1, Side::BID, 90,  10));
    f.submit(make_new(2, Side::ASK, 100, 10));
    f.drain();
    ASSERT_EQ(f.eng.order_count(), 2u);
    ASSERT_FALSE(f.eng.pop_trade().has_value());
    return true;
}

static bool full_match_both_orders_removed() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_new(2, Side::ASK, 100, 10));
    f.drain();
    ASSERT_EQ(f.eng.order_count(), 0u);
    ASSERT_EQ(f.eng.bid_level_count(), 0u);
    ASSERT_EQ(f.eng.ask_level_count(), 0u);
    return true;
}

static bool full_match_sends_match_outbound_for_both_sides() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_new(2, Side::ASK, 100, 10));
    auto out = f.drain();   // no NEW ack for crossing ask — only MATCH messages
    ASSERT_EQ(out.size(), 2u);
    bool bid_seen = false, ask_seen = false;
    for (auto& m : out) {
        ASSERT_TRUE(m.message_type == MessageType::MATCH);
        ASSERT_TRUE(m.status == Status::SUCCESS);
        if (m.client_id == 1) bid_seen = true;
        if (m.client_id == 2) ask_seen = true;
    }
    ASSERT_TRUE(bid_seen);
    ASSERT_TRUE(ask_seen);
    return true;
}

static bool full_match_trade_recorded_with_correct_info() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 10));
    f.drain();
    f.submit(make_new(2, Side::ASK, 100, 10));
    f.drain();
    auto t = f.eng.pop_trade();
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->get_bid_info().client_id_, 1u);
    ASSERT_EQ(t->get_ask_info().client_id_, 2u);
    ASSERT_EQ(t->get_bid_info().quantity, 10u);
    ASSERT_EQ(t->get_ask_info().quantity, 10u);
    ASSERT_EQ(t->get_bid_info().price, 100u);
    return true;
}

static bool partial_match_bid_larger_than_ask() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 15));   // bid qty=15
    f.drain();
    f.submit(make_new(2, Side::ASK, 100, 10));   // ask qty=10: fills, bid keeps 5
    f.drain();
    ASSERT_EQ(f.eng.order_count(), 1u);
    ASSERT_EQ(f.eng.bid_level_count(), 1u);
    ASSERT_EQ(f.eng.ask_level_count(), 0u);
    auto t = f.eng.pop_trade();
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->get_bid_info().quantity, 10u);   // fill qty = min(15,10)
    ASSERT_EQ(t->get_ask_info().quantity, 10u);
    return true;
}

static bool partial_match_ask_larger_than_bid() {
    EF f;
    f.submit(make_new(1, Side::BID, 100, 10));   // bid qty=10
    f.drain();
    f.submit(make_new(2, Side::ASK, 100, 15));   // ask qty=15: bid fills, ask keeps 5
    f.drain();
    ASSERT_EQ(f.eng.order_count(), 1u);
    ASSERT_EQ(f.eng.bid_level_count(), 0u);
    ASSERT_EQ(f.eng.ask_level_count(), 1u);
    return true;
}

static bool fifo_first_bid_at_price_level_matched_first() {
    EF f;
    OrderId id0 = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 5));    // id=0, first in queue
    OrderId id1 = f.next_id();
    f.submit(make_new(2, Side::BID, 100, 5));    // id=1, second
    f.drain();
    f.submit(make_new(3, Side::ASK, 100, 5));    // fills exactly one bid
    f.drain();
    ASSERT_FALSE(f.eng.has_order(id0));   // first bid consumed
    ASSERT_TRUE(f.eng.has_order(id1));    // second remains
    ASSERT_EQ(f.eng.order_count(), 1u);
    return true;
}

static bool price_priority_best_bid_matched_first() {
    EF f;
    OrderId id_high = f.next_id();
    f.submit(make_new(1, Side::BID, 110, 5));    // higher price → best bid
    OrderId id_low = f.next_id();
    f.submit(make_new(2, Side::BID, 100, 5));    // lower price
    f.drain();
    f.submit(make_new(3, Side::ASK, 100, 5));    // fills only one bid
    f.drain();
    ASSERT_FALSE(f.eng.has_order(id_high));   // best bid (110) matched
    ASSERT_TRUE(f.eng.has_order(id_low));     // lower bid remains
    return true;
}

static bool price_priority_best_ask_matched_first() {
    EF f;
    OrderId id_low = f.next_id();
    f.submit(make_new(1, Side::ASK, 90,  5));    // lower price → best ask
    OrderId id_high = f.next_id();
    f.submit(make_new(2, Side::ASK, 100, 5));    // higher price
    f.drain();
    f.submit(make_new(3, Side::BID, 100, 5));    // fills only one ask
    f.drain();
    ASSERT_FALSE(f.eng.has_order(id_low));    // best ask (90) matched
    ASSERT_TRUE(f.eng.has_order(id_high));    // higher ask remains
    return true;
}

static bool large_ask_consumes_multiple_bids() {
    EF f;
    OrderId id0 = f.next_id();
    f.submit(make_new(1, Side::BID, 100, 5));
    OrderId id1 = f.next_id();
    f.submit(make_new(2, Side::BID, 100, 5));
    f.drain();
    f.submit(make_new(3, Side::ASK, 100, 10));   // fills both bids
    f.drain();
    ASSERT_FALSE(f.eng.has_order(id0));
    ASSERT_FALSE(f.eng.has_order(id1));
    ASSERT_EQ(f.eng.order_count(), 0u);
    // Two separate trades recorded — one per bid consumed.
    ASSERT_TRUE(f.eng.pop_trade().has_value());
    ASSERT_TRUE(f.eng.pop_trade().has_value());
    ASSERT_FALSE(f.eng.pop_trade().has_value());
    return true;
}

static bool ask_consumes_bids_across_multiple_price_levels() {
    EF f;
    f.submit(make_new(1, Side::BID, 110, 5));    // best bid level
    f.submit(make_new(2, Side::BID, 100, 5));    // second level
    f.drain();
    f.submit(make_new(3, Side::ASK, 100, 10));   // fills both levels
    f.drain();
    ASSERT_EQ(f.eng.order_count(), 0u);
    ASSERT_EQ(f.eng.bid_level_count(), 0u);
    return true;
}

// When a NEW order immediately crosses the book, suppress_ack=true means no
// NEW acknowledgement is sent — only MATCH messages.
static bool new_immediately_matches_suppresses_new_ack() {
    EF f;
    f.submit(make_new(1, Side::ASK, 100, 10));   // resting ask
    f.drain();
    f.submit(make_new(2, Side::BID, 100, 10));   // crosses immediately
    auto out = f.drain();
    ASSERT_EQ(out.size(), 2u);
    for (auto& m : out) {
        ASSERT_TRUE(m.message_type == MessageType::MATCH);
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Matching — market orders
// ═════════════════════════════════════════════════════════════════════════════

// A MARKET BID has price=0, which does not cross a positive ASK in the price
// check.  The MARKET flag bypasses that check and forces the fill.
static bool market_bid_fills_against_limit_ask_at_any_price() {
    EF f;
    f.submit(make_new(1, Side::ASK, 200, 10));   // LIMIT ask, price far above 0
    f.drain();
    f.submit(make_new_market(2, Side::BID, 10)); // MARKET bid at price=0
    f.drain();
    ASSERT_EQ(f.eng.order_count(), 0u);
    auto t = f.eng.pop_trade();
    ASSERT_TRUE(t.has_value());
    ASSERT_EQ(t->get_bid_info().quantity, 10u);
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Modify triggers matching
// ═════════════════════════════════════════════════════════════════════════════

// Raising a bid price to cross a resting ask must produce MATCH messages and
// suppress the MODIFY ack (same suppress_ack logic as NEW).
static bool modify_raises_bid_price_to_trigger_match() {
    EF f;
    OrderId bid_id = f.next_id();
    f.submit(make_new(1, Side::BID, 90,  10));   // bid at 90, no cross
    f.submit(make_new(2, Side::ASK, 100, 10));   // ask at 100, no cross
    f.drain();
    f.submit(make_modify(1, bid_id, Side::BID, 100, 10));  // raise bid → crosses
    auto out = f.drain();
    // suppress_ack=true: only MATCH messages, no MODIFY ack
    ASSERT_EQ(out.size(), 2u);
    for (auto& m : out) {
        ASSERT_TRUE(m.message_type == MessageType::MATCH);
        ASSERT_TRUE(m.status == Status::SUCCESS);
    }
    ASSERT_EQ(f.eng.order_count(), 0u);
    return true;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    return run_tests("Engine", {
        // NEW
        {"new: BID rests on book",                      new_bid_rests_on_book},
        {"new: ASK rests on book",                      new_ask_rests_on_book},
        {"new: sends SUCCESS outbound",                  new_sends_success_outbound},
        {"new: outbound carries assigned order_id",      new_outbound_carries_assigned_order_id},
        {"new: order_ids are sequential",                new_order_ids_are_sequential},
        {"new: two bids at same price level",            two_bids_at_same_price_level},
        {"new: bids at different price levels",          bids_at_different_price_levels},
        // CANCEL
        {"cancel: removes BID from book",                cancel_bid_removes_from_book},
        {"cancel: removes ASK from book",                cancel_ask_removes_from_book},
        {"cancel: sends SUCCESS outbound",               cancel_sends_success_outbound},
        {"cancel: only removes targeted order",          cancel_only_removes_targeted_order},
        {"cancel: non-existent order → FAILURE",         cancel_nonexistent_order_sends_failure},
        {"cancel: wrong client_id → FAILURE",            cancel_wrong_client_sends_failure_and_preserves_order},
        // MODIFY
        {"modify: non-existent order → FAILURE",         modify_nonexistent_order_sends_failure},
        {"modify: wrong client_id → FAILURE",            modify_wrong_client_sends_failure},
        {"modify: wrong side → FAILURE",                 modify_wrong_side_sends_failure},
        {"modify: price change moves to new level",      modify_price_change_moves_order_to_new_level},
        {"modify: sends SUCCESS outbound",               modify_sends_success_with_correct_fields},
        {"modify: quantity change updates order",        modify_quantity_change_updates_order},
        {"modify: same price+qty keeps order on book",   modify_same_price_and_qty_keeps_order},
        // Matching — limit
        {"match: no cross → nothing filled",             no_cross_nothing_filled},
        {"match: full fill removes both orders",         full_match_both_orders_removed},
        {"match: full fill sends MATCH for both sides",  full_match_sends_match_outbound_for_both_sides},
        {"match: full fill records trade with info",     full_match_trade_recorded_with_correct_info},
        {"match: partial fill — bid larger",             partial_match_bid_larger_than_ask},
        {"match: partial fill — ask larger",             partial_match_ask_larger_than_bid},
        {"match: FIFO — first bid at level matched",    fifo_first_bid_at_price_level_matched_first},
        {"match: price priority — best bid first",       price_priority_best_bid_matched_first},
        {"match: price priority — best ask first",       price_priority_best_ask_matched_first},
        {"match: large order consumes multiple bids",    large_ask_consumes_multiple_bids},
        {"match: ask fills bids across price levels",    ask_consumes_bids_across_multiple_price_levels},
        {"match: immediate fill suppresses NEW ack",     new_immediately_matches_suppresses_new_ack},
        // Matching — market
        {"match: market BID crosses limit ASK",          market_bid_fills_against_limit_ask_at_any_price},
        // Modify triggers match
        {"match: modify raises price to trigger fill",   modify_raises_bid_price_to_trigger_match},
    });
}
