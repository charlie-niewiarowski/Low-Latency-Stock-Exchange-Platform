//
// Created by cniew on 10/18/26.
//

#include "ouch_responder.hpp"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kTimestamp = 34'200'000'000'000; // 09:30:00

OrderRecord MakeRecord(const ClientId client, const uint32_t user_ref_num) {
    return OrderRecord{
        .client_id                     = client,
        .user_ref_num                  = user_ref_num,
        .quantity                      = 500,
        .price                         = 1'502'500,
        .symbol                        = wire::alpha<8>("AAPL"),
        .cl_ord_id                     = wire::alpha<14>("ORDER"),
        .side                          = 'B',
        .time_in_force                 = '0',
        .display                       = 'Y',
        .capacity                      = 'A',
        .intermarket_sweep_eligibility = 'N',
        .cross_type                    = 'N',
    };
}

GatewayNote Entered(const OrderId id, const OrderRecord &record) {
    return GatewayNote{.kind = GatewayNote::Kind::ORDER_ENTERED, .reject_reason = 0, .order_id = id, .record = record};
}

GatewayNote Replacing(const OrderId id, const OrderRecord &record) {
    return GatewayNote{.kind = GatewayNote::Kind::ORDER_REPLACING, .reject_reason = 0, .order_id = id, .record = record};
}

EngineEvent Event(const EngineEventType type, const OrderId id, const Price price, const Quantity quantity,
                  const char reason = 0) {
    return EngineEvent{.order_id = id, .price = price, .quantity = quantity, .type = type, .reason = reason};
}

} // namespace

TEST(OuchResponder, GatewayRejectNeedsNoEngineEvent) {
    OuchResponder responder;
    const GatewayNote note{.kind = GatewayNote::Kind::REJECTED, .reject_reason = ouch_reject::kInvalidPrice,
                           .order_id = 0, .record = MakeRecord(3, 7)};

    const auto response = responder.on_note(note, kTimestamp);

    ASSERT_TRUE(response);
    EXPECT_EQ(response->client_id, 3u);
    ASSERT_EQ(response->ouch.message_type, OutboundOuchMessageType::ORDER_REJECTED);
    EXPECT_EQ(response->ouch.order_rejected.timestamp, kTimestamp);
    EXPECT_EQ(response->ouch.order_rejected.user_ref_num, 7u);
    EXPECT_EQ(response->ouch.order_rejected.reason, ouch_reject::kInvalidPrice);
    EXPECT_EQ(response->ouch.order_rejected.cl_ord_id, wire::alpha<14>("ORDER"));
    EXPECT_EQ(responder.live_orders(), 0u);
}

TEST(OuchResponder, AcceptedEchoesTheEnterOrder) {
    OuchResponder responder;
    EXPECT_FALSE(responder.on_note(Entered(42, MakeRecord(3, 7)), kTimestamp));

    const auto response = responder.on_event(Event(EngineEventType::ACCEPTED, 42, 15'025, 500), kTimestamp);

    ASSERT_TRUE(response);
    EXPECT_EQ(response->client_id, 3u);
    ASSERT_EQ(response->ouch.message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
    const OuchOrderAccepted &a = response->ouch.order_accepted;
    EXPECT_EQ(a.timestamp, kTimestamp);
    EXPECT_EQ(a.user_ref_num, 7u);
    EXPECT_EQ(a.side, 'B');
    EXPECT_EQ(a.quantity, 500u);
    EXPECT_EQ(a.symbol, wire::alpha<8>("AAPL"));
    EXPECT_EQ(a.price, 1'502'500u); // as entered, OUCH 4-decimal
    EXPECT_EQ(a.order_reference_number, 42u);
    EXPECT_EQ(a.capacity, 'A');
    EXPECT_EQ(a.order_state, 'L');
    EXPECT_EQ(a.cl_ord_id, wire::alpha<14>("ORDER"));
}

TEST(OuchResponder, ExecutionsConvertPriceAndRetireFilledOrders) {
    OuchResponder responder;
    responder.on_note(Entered(42, MakeRecord(3, 7)), kTimestamp);
    responder.on_event(Event(EngineEventType::ACCEPTED, 42, 15'025, 500), kTimestamp);

    EngineEvent fill = Event(EngineEventType::EXECUTED, 42, 15'020, 200, engine_reason::kAddedLiquidity);
    fill.match_number = 9;
    const auto response = responder.on_event(fill, kTimestamp);

    ASSERT_TRUE(response);
    ASSERT_EQ(response->ouch.message_type, OutboundOuchMessageType::ORDER_EXECUTED);
    const OuchOrderExecuted &e = response->ouch.order_executed;
    EXPECT_EQ(e.user_ref_num, 7u);
    EXPECT_EQ(e.executed_shares, 200u);
    EXPECT_EQ(e.execution_price, 1'502'000u); // cents -> 4 decimals
    EXPECT_EQ(e.liquidity_flag, 'A');
    EXPECT_EQ(e.match_number, 9u);
    EXPECT_EQ(responder.live_orders(), 1u);

    ASSERT_TRUE(responder.on_event(Event(EngineEventType::EXECUTED, 42, 15'020, 300, 'A'), kTimestamp));
    EXPECT_EQ(responder.live_orders(), 0u);
    EXPECT_FALSE(responder.on_event(Event(EngineEventType::EXECUTED, 42, 15'020, 1, 'A'), kTimestamp));
}

TEST(OuchResponder, CanceledReportsDecrementAndRetiresAtZero) {
    OuchResponder responder;
    responder.on_note(Entered(42, MakeRecord(3, 7)), kTimestamp);
    responder.on_event(Event(EngineEventType::ACCEPTED, 42, 15'025, 500), kTimestamp);

    const auto partial = responder.on_event(
        Event(EngineEventType::CANCELED, 42, 15'025, 100, engine_reason::kUserRequested), kTimestamp);
    ASSERT_TRUE(partial);
    ASSERT_EQ(partial->ouch.message_type, OutboundOuchMessageType::ORDER_CANCELED);
    EXPECT_EQ(partial->ouch.order_canceled.user_ref_num, 7u);
    EXPECT_EQ(partial->ouch.order_canceled.decrement_shares, 100u);
    EXPECT_EQ(partial->ouch.order_canceled.reason, 'U');
    EXPECT_EQ(responder.live_orders(), 1u);

    ASSERT_TRUE(responder.on_event(Event(EngineEventType::CANCELED, 42, 15'025, 400, 'U'), kTimestamp));
    EXPECT_EQ(responder.live_orders(), 0u);
}

TEST(OuchResponder, ReplacedNamesBothUserRefNumsAndRetiresTheOriginal) {
    OuchResponder responder;
    responder.on_note(Entered(42, MakeRecord(3, 7)), kTimestamp);
    responder.on_event(Event(EngineEventType::ACCEPTED, 42, 15'025, 500), kTimestamp);

    OrderRecord replacement = MakeRecord(3, 8);
    replacement.price = 1'501'000;
    replacement.cl_ord_id = wire::alpha<14>("REPLACED");
    // What OrderEntry can't know: the original's, filled in on REPLACED.
    replacement.side = 0;
    replacement.symbol = {};
    replacement.capacity = 0;
    replacement.cross_type = 0;
    responder.on_note(Replacing(43, replacement), kTimestamp);

    EngineEvent replaced = Event(EngineEventType::REPLACED, 43, 15'010, 400);
    replaced.orig_order_id = 42;
    const auto response = responder.on_event(replaced, kTimestamp);

    ASSERT_TRUE(response);
    ASSERT_EQ(response->ouch.message_type, OutboundOuchMessageType::ORDER_REPLACED);
    const OuchOrderReplaced &r = response->ouch.order_replaced;
    EXPECT_EQ(r.orig_user_ref_num, 7u);
    EXPECT_EQ(r.user_ref_num, 8u);
    EXPECT_EQ(r.quantity, 400u);
    EXPECT_EQ(r.price, 1'501'000u);
    EXPECT_EQ(r.order_reference_number, 43u);
    EXPECT_EQ(r.cl_ord_id, wire::alpha<14>("REPLACED"));
    EXPECT_EQ(r.side, 'B');                       // inherited
    EXPECT_EQ(r.symbol, wire::alpha<8>("AAPL"));  // inherited
    EXPECT_EQ(r.capacity, 'A');                   // inherited
    EXPECT_EQ(r.cross_type, 'N');                 // inherited

    EXPECT_EQ(responder.live_orders(), 1u);
    EXPECT_FALSE(responder.on_event(Event(EngineEventType::CANCELED, 42, 15'025, 1, 'U'), kTimestamp));
    EXPECT_TRUE(responder.on_event(Event(EngineEventType::CANCELED, 43, 15'010, 400, 'U'), kTimestamp));
    EXPECT_EQ(responder.live_orders(), 0u);
}

TEST(OuchResponder, IgnoredReplaceRetiresTheReplacementSilently) {
    OuchResponder responder;
    responder.on_note(Replacing(43, MakeRecord(3, 8)), kTimestamp); // its original is already dead
    EXPECT_EQ(responder.live_orders(), 1u);

    EXPECT_FALSE(responder.on_event(Event(EngineEventType::REPLACE_IGNORED, 43, 0, 0), kTimestamp));
    EXPECT_EQ(responder.live_orders(), 0u);
}

TEST(OuchResponder, MarketOrderAcceptedThenRemainderCanceled) {
    OuchResponder responder;
    OrderRecord market = MakeRecord(3, 7);
    market.price = kOuchMarketPriceFloor;
    responder.on_note(Entered(42, market), kTimestamp);

    const auto accepted = responder.on_event(Event(EngineEventType::ACCEPTED, 42, 0, 500), kTimestamp);
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->ouch.order_accepted.price, kOuchMarketPriceFloor); // echoed as entered

    responder.on_event(Event(EngineEventType::EXECUTED, 42, 15'000, 200, 'R'), kTimestamp);
    const auto canceled = responder.on_event(
        Event(EngineEventType::CANCELED, 42, 0, 300, engine_reason::kImmediateOrCancel), kTimestamp);
    ASSERT_TRUE(canceled);
    EXPECT_EQ(canceled->ouch.order_canceled.reason, 'I');
    EXPECT_EQ(responder.live_orders(), 0u);
}

TEST(OuchResponder, EngineRejectBecomesProcessingError) {
    OuchResponder responder;
    responder.on_note(Entered(42, MakeRecord(3, 7)), kTimestamp);

    const auto response = responder.on_event(
        Event(EngineEventType::REJECTED, 42, 0, 0, engine_reason::kBookFull), kTimestamp);

    ASSERT_TRUE(response);
    ASSERT_EQ(response->ouch.message_type, OutboundOuchMessageType::ORDER_REJECTED);
    EXPECT_EQ(response->ouch.order_rejected.reason, ouch_reject::kProcessingError);
    EXPECT_EQ(responder.live_orders(), 0u);
}
