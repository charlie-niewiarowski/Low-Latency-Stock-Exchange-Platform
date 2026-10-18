//
// Created by cniew on 9/11/26.
//
// Orderbook behaviour under Nasdaq OUCH semantics, observed the way the rest
// of the exchange observes it: the EngineEvents it emits and the ITCH it
// publishes.
//

#include "engine.hpp"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

namespace {

constexpr ClientId kAlice = 1;
constexpr ClientId kBob   = 2;

EngineRequest New(const OrderId id, const ClientId client, const Side side, const Price price,
                  const Quantity quantity, const TimeInForce tif = TimeInForce::DAY,
                  const OrderType type = OrderType::LIMIT) {
    return EngineRequest{.order_id = id, .client_id = client, .price = price, .quantity = quantity,
                         .type = EngineRequestType::NEW, .side = side, .order_type = type,
                         .time_in_force = tif};
}

EngineRequest Market(const OrderId id, const ClientId client, const Side side, const Quantity quantity) {
    return New(id, client, side, 0, quantity, TimeInForce::DAY, OrderType::MARKET);
}

EngineRequest Cancel(const OrderId id, const ClientId client, const Quantity quantity) {
    return EngineRequest{.order_id = id, .client_id = client, .quantity = quantity,
                         .type = EngineRequestType::CANCEL};
}

EngineRequest Replace(const OrderId id, const OrderId new_id, const ClientId client, const Price price,
                      const Quantity quantity, const TimeInForce tif = TimeInForce::DAY) {
    return EngineRequest{.order_id = id, .new_order_id = new_id, .client_id = client, .price = price,
                         .quantity = quantity, .type = EngineRequestType::REPLACE,
                         .order_type = OrderType::LIMIT, .time_in_force = tif};
}

void ExpectEvent(const EngineEvent &e, const EngineEventType type, const OrderId id, const ClientId client,
                 const Price price, const Quantity quantity) {
    EXPECT_EQ(e.type, type);
    EXPECT_EQ(e.order_id, id);
    EXPECT_EQ(e.client_id, client);
    EXPECT_EQ(e.price, price);
    EXPECT_EQ(e.quantity, quantity);
}

} // namespace

class OrderbookTest : public testing::Test {
protected:
    // Orderbook is several MB of price ladder plus a preallocated pool: heap only.
    OutboundRing out_{1 << 12};
    MarketDataRing market_data_{1 << 12};
    std::unique_ptr<Orderbook> book_ = std::make_unique<Orderbook>(out_, market_data_);

    bool Send(const EngineRequest &req) { return book_->process(req, TscClock::now_tsc()); }

    std::vector<EngineEvent> Events() {
        std::vector<EngineEvent> events;
        while (auto e = out_.pop()) events.push_back(*e);
        return events;
    }

    std::vector<ITCH> Itch() {
        std::vector<ITCH> messages;
        while (auto m = market_data_.pop()) messages.push_back(*m);
        return messages;
    }

    // Sends req and asserts it was applied, discarding what it produced.
    void Setup(const EngineRequest &req) {
        ASSERT_TRUE(Send(req));
        Events();
        Itch();
    }
};

//=============================================================================
// entering orders
//=============================================================================

TEST_F(OrderbookTest, RestingLimitIsAcceptedAndPublished) {
    ASSERT_TRUE(Send(New(1, kAlice, Side::BID, 1000, 100)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 1u);
    ExpectEvent(events[0], EngineEventType::ACCEPTED, 1, kAlice, 1000, 100);

    const auto itch = Itch();
    ASSERT_EQ(itch.size(), 1u);
    ASSERT_EQ(itch[0].header.message_type, ItchMessageType::ADD_ORDER);
    EXPECT_EQ(itch[0].header.stock_locate, BOOK_STOCK_LOCATE);
    EXPECT_EQ(itch[0].add_order.order_reference_number, 1u);
    EXPECT_EQ(itch[0].add_order.buy_sell_indicator, 'B');
    EXPECT_EQ(itch[0].add_order.shares, 100u);
    EXPECT_EQ(itch[0].add_order.price, 100'000u); // $10.00, 4 implied decimals
    EXPECT_EQ(itch[0].add_order.stock, wire::alpha<8>(BOOK_SYMBOL));

    EXPECT_TRUE(book_->has_order(1));
    EXPECT_EQ(book_->bids_at(1000), 1u);
}

TEST_F(OrderbookTest, CrossingLimitTradesAtRestingPriceWithoutTouchingTheBook) {
    Setup(New(1, kAlice, Side::ASK, 1000, 100));

    ASSERT_TRUE(Send(New(2, kBob, Side::BID, 1010, 60)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 3u);
    ExpectEvent(events[0], EngineEventType::ACCEPTED, 2, kBob, 1010, 60);
    ExpectEvent(events[1], EngineEventType::EXECUTED, 2, kBob, 1000, 60);
    EXPECT_EQ(events[1].reason, engine_reason::kRemovedLiquidity);
    ExpectEvent(events[2], EngineEventType::EXECUTED, 1, kAlice, 1000, 60);
    EXPECT_EQ(events[2].reason, engine_reason::kAddedLiquidity);
    EXPECT_EQ(events[1].match_number, events[2].match_number);

    // Only the resting side shows up in ITCH; the aggressor never rested.
    const auto itch = Itch();
    ASSERT_EQ(itch.size(), 1u);
    ASSERT_EQ(itch[0].header.message_type, ItchMessageType::ORDER_EXECUTED);
    EXPECT_EQ(itch[0].order_executed.order_reference_number, 1u);
    EXPECT_EQ(itch[0].order_executed.executed_shares, 60u);
    EXPECT_EQ(itch[0].order_executed.match_number, events[1].match_number);

    EXPECT_FALSE(book_->has_order(2));
    EXPECT_EQ(book_->open_quantity(1), 40u);
    EXPECT_EQ(book_->executed_quantity(1), 60u);
}

TEST_F(OrderbookTest, AggressorSweepsLevelsThenRestsRemainder) {
    Setup(New(1, kAlice, Side::ASK, 1000, 50));
    Setup(New(2, kAlice, Side::ASK, 1001, 50));

    ASSERT_TRUE(Send(New(3, kBob, Side::BID, 1001, 150)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 5u);
    ExpectEvent(events[1], EngineEventType::EXECUTED, 3, kBob, 1000, 50);
    ExpectEvent(events[3], EngineEventType::EXECUTED, 3, kBob, 1001, 50);
    EXPECT_LT(events[1].match_number, events[3].match_number);

    const auto itch = Itch();
    ASSERT_EQ(itch.size(), 3u);
    EXPECT_EQ(itch[0].header.message_type, ItchMessageType::ORDER_EXECUTED);
    EXPECT_EQ(itch[1].header.message_type, ItchMessageType::ORDER_EXECUTED);
    ASSERT_EQ(itch[2].header.message_type, ItchMessageType::ADD_ORDER);
    EXPECT_EQ(itch[2].add_order.shares, 50u); // only the remainder is published

    EXPECT_EQ(book_->ask_level_count(), 0u);
    EXPECT_EQ(book_->open_quantity(3), 50u);
    EXPECT_EQ(book_->executed_quantity(3), 100u);
}

TEST_F(OrderbookTest, TimePriorityWithinALevel) {
    Setup(New(1, kAlice, Side::ASK, 1000, 50));
    Setup(New(2, kBob, Side::ASK, 1000, 50));

    ASSERT_TRUE(Send(New(3, kBob, Side::BID, 1000, 30)));

    EXPECT_EQ(book_->open_quantity(1), 20u);
    EXPECT_EQ(book_->open_quantity(2), 50u);
}

TEST_F(OrderbookTest, IocRemainderIsCanceled) {
    Setup(New(1, kAlice, Side::ASK, 1000, 50));

    ASSERT_TRUE(Send(New(2, kBob, Side::BID, 1000, 80, TimeInForce::IOC)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 4u);
    ExpectEvent(events[3], EngineEventType::CANCELED, 2, kBob, 1000, 30);
    EXPECT_EQ(events[3].reason, engine_reason::kImmediateOrCancel);
    EXPECT_FALSE(book_->has_order(2));
    EXPECT_EQ(Itch().size(), 1u); // the execution only
}

TEST_F(OrderbookTest, MarketOrderTakesAnyPriceAndNeverRests) {
    Setup(New(1, kAlice, Side::ASK, 1000, 10));
    Setup(New(2, kAlice, Side::ASK, 5000, 10));

    ASSERT_TRUE(Send(Market(3, kBob, Side::BID, 30)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 6u);
    ExpectEvent(events[1], EngineEventType::EXECUTED, 3, kBob, 1000, 10);
    ExpectEvent(events[3], EngineEventType::EXECUTED, 3, kBob, 5000, 10);
    ExpectEvent(events[5], EngineEventType::CANCELED, 3, kBob, 0, 10);
    EXPECT_EQ(events[5].reason, engine_reason::kImmediateOrCancel);
    EXPECT_EQ(book_->order_count(), 0u);
}

TEST_F(OrderbookTest, MarketOrderIntoEmptyBookIsCanceled) {
    ASSERT_TRUE(Send(Market(1, kBob, Side::ASK, 100)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 2u);
    ExpectEvent(events[0], EngineEventType::ACCEPTED, 1, kBob, 0, 100);
    ExpectEvent(events[1], EngineEventType::CANCELED, 1, kBob, 0, 100);
    EXPECT_TRUE(Itch().empty());
}

TEST_F(OrderbookTest, DuplicateOrderIdIsIgnored) {
    Setup(New(1, kAlice, Side::BID, 1000, 100));

    EXPECT_FALSE(Send(New(1, kAlice, Side::BID, 1000, 100)));
    EXPECT_TRUE(Events().empty());
}

//=============================================================================
// cancel
//=============================================================================

TEST_F(OrderbookTest, PartialCancelReducesInPlaceAndKeepsPriority) {
    Setup(New(1, kAlice, Side::ASK, 1000, 100));
    Setup(New(2, kBob, Side::ASK, 1000, 100));

    ASSERT_TRUE(Send(Cancel(1, kAlice, 40)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 1u);
    ExpectEvent(events[0], EngineEventType::CANCELED, 1, kAlice, 1000, 60);
    EXPECT_EQ(events[0].reason, engine_reason::kUserRequested);

    const auto itch = Itch();
    ASSERT_EQ(itch.size(), 1u);
    ASSERT_EQ(itch[0].header.message_type, ItchMessageType::ORDER_CANCEL);
    EXPECT_EQ(itch[0].order_cancel.canceled_shares, 60u);

    // Still first in line.
    Setup(New(3, kBob, Side::BID, 1000, 50));
    EXPECT_FALSE(book_->has_order(1));
    EXPECT_EQ(book_->open_quantity(2), 90u);
}

TEST_F(OrderbookTest, CancelQuantityIncludesExecutedShares) {
    Setup(New(1, kAlice, Side::ASK, 1000, 100));
    Setup(New(2, kBob, Side::BID, 1000, 30)); // ask: 30 executed, 70 open

    ASSERT_TRUE(Send(Cancel(1, kAlice, 50))); // at most 50 total -> 20 open
    ExpectEvent(Events().at(0), EngineEventType::CANCELED, 1, kAlice, 1000, 50);
    EXPECT_EQ(book_->open_quantity(1), 20u);

    ASSERT_TRUE(Send(Cancel(1, kAlice, 0)));
    ExpectEvent(Events().at(0), EngineEventType::CANCELED, 1, kAlice, 1000, 20);
    const auto itch = Itch();
    ASSERT_EQ(itch.size(), 2u);
    EXPECT_EQ(itch[1].header.message_type, ItchMessageType::ORDER_DELETE);
    EXPECT_FALSE(book_->has_order(1));
    EXPECT_EQ(book_->ask_level_count(), 0u);
}

TEST_F(OrderbookTest, IgnoredCancels) {
    Setup(New(1, kAlice, Side::ASK, 1000, 100));

    EXPECT_FALSE(Send(Cancel(1, kAlice, 100))); // wouldn't reduce: superfluous
    EXPECT_FALSE(Send(Cancel(1, kAlice, 500)));
    EXPECT_FALSE(Send(Cancel(1, kBob, 0)));     // not Bob's order
    EXPECT_FALSE(Send(Cancel(9, kAlice, 0)));   // unknown / dead
    EXPECT_TRUE(Events().empty());
    EXPECT_TRUE(Itch().empty());
}

//=============================================================================
// replace
//=============================================================================

TEST_F(OrderbookTest, ReplaceLosesTimePriority) {
    Setup(New(1, kAlice, Side::ASK, 1000, 100));
    Setup(New(2, kBob, Side::ASK, 1000, 100));

    ASSERT_TRUE(Send(Replace(1, 3, kAlice, 1000, 100)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 1u);
    ExpectEvent(events[0], EngineEventType::REPLACED, 3, kAlice, 1000, 100);
    EXPECT_EQ(events[0].orig_order_id, 1u);

    const auto itch = Itch();
    ASSERT_EQ(itch.size(), 1u);
    ASSERT_EQ(itch[0].header.message_type, ItchMessageType::ORDER_REPLACE);
    EXPECT_EQ(itch[0].order_replace.original_order_reference_number, 1u);
    EXPECT_EQ(itch[0].order_replace.new_order_reference_number, 3u);
    EXPECT_EQ(itch[0].order_replace.shares, 100u);
    EXPECT_EQ(itch[0].order_replace.price, 100'000u);

    EXPECT_FALSE(book_->has_order(1));
    Setup(New(4, kBob, Side::BID, 1000, 100));
    EXPECT_FALSE(book_->has_order(2)); // Bob's order is now ahead
    EXPECT_EQ(book_->open_quantity(3), 100u);
}

TEST_F(OrderbookTest, ReplaceQuantityIsTotalLiabilityOfTheChain) {
    // The spec's example: enter 500, execute 100, then replace.
    Setup(New(1, kAlice, Side::ASK, 1000, 500));
    Setup(New(2, kBob, Side::BID, 1000, 100));

    ASSERT_TRUE(Send(Replace(1, 3, kAlice, 1001, 500))); // keep the remaining 400 exposed
    ExpectEvent(Events().at(0), EngineEventType::REPLACED, 3, kAlice, 1001, 400);
    EXPECT_EQ(book_->open_quantity(3), 400u);
    EXPECT_EQ(book_->executed_quantity(3), 100u);

    ASSERT_TRUE(Send(Replace(3, 4, kAlice, 1001, 600))); // a new 500 exposed
    EXPECT_EQ(book_->open_quantity(4), 500u);
    EXPECT_EQ(book_->executed_quantity(4), 100u);
}

TEST_F(OrderbookTest, ReplaceAtOrBelowExecutedSharesCancels) {
    Setup(New(1, kAlice, Side::ASK, 1000, 100));
    Setup(New(2, kBob, Side::BID, 1000, 60));

    ASSERT_TRUE(Send(Replace(1, 3, kAlice, 1000, 50)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 1u);
    ExpectEvent(events[0], EngineEventType::CANCELED, 1, kAlice, 1000, 40);
    EXPECT_EQ(Itch().at(0).header.message_type, ItchMessageType::ORDER_DELETE);
    EXPECT_EQ(book_->order_count(), 0u);
}

TEST_F(OrderbookTest, MarketableReplaceTradesBeforeResting) {
    Setup(New(1, kAlice, Side::BID, 990, 100));
    Setup(New(2, kBob, Side::ASK, 1000, 60));

    ASSERT_TRUE(Send(Replace(1, 3, kAlice, 1000, 100)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 3u);
    ExpectEvent(events[0], EngineEventType::REPLACED, 3, kAlice, 1000, 100);
    ExpectEvent(events[1], EngineEventType::EXECUTED, 3, kAlice, 1000, 60);
    ExpectEvent(events[2], EngineEventType::EXECUTED, 2, kBob, 1000, 60);

    const auto itch = Itch();
    ASSERT_EQ(itch.size(), 3u);
    EXPECT_EQ(itch[0].header.message_type, ItchMessageType::ORDER_DELETE);
    EXPECT_EQ(itch[1].header.message_type, ItchMessageType::ORDER_EXECUTED);
    ASSERT_EQ(itch[2].header.message_type, ItchMessageType::ADD_ORDER);
    EXPECT_EQ(itch[2].add_order.order_reference_number, 3u);
    EXPECT_EQ(itch[2].add_order.shares, 40u);

    EXPECT_EQ(book_->open_quantity(3), 40u);
    EXPECT_EQ(book_->executed_quantity(3), 60u);
}

TEST_F(OrderbookTest, IocReplaceCancelsItsRemainder) {
    Setup(New(1, kAlice, Side::BID, 990, 100));

    ASSERT_TRUE(Send(Replace(1, 2, kAlice, 995, 100, TimeInForce::IOC)));

    const auto events = Events();
    ASSERT_EQ(events.size(), 2u);
    ExpectEvent(events[0], EngineEventType::REPLACED, 2, kAlice, 995, 100);
    ExpectEvent(events[1], EngineEventType::CANCELED, 2, kAlice, 995, 100);
    EXPECT_EQ(events[1].reason, engine_reason::kImmediateOrCancel);
    EXPECT_EQ(book_->order_count(), 0u);
}

TEST_F(OrderbookTest, IgnoredReplaces) {
    Setup(New(1, kAlice, Side::BID, 990, 100));
    Setup(New(2, kAlice, Side::BID, 990, 100));

    EXPECT_FALSE(Send(Replace(9, 3, kAlice, 990, 100))); // unknown / dead original
    EXPECT_FALSE(Send(Replace(1, 3, kBob, 990, 100)));   // not Bob's order
    EXPECT_FALSE(Send(Replace(1, 2, kAlice, 990, 100))); // replacement id already live
    EXPECT_TRUE(Events().empty());
    EXPECT_TRUE(book_->has_order(1));
}

//=============================================================================
// answers_request -- exactly one event per request is its answer
//=============================================================================

TEST_F(OrderbookTest, OnlyTheFirstEventOfARequestAnswersIt) {
    Setup(New(1, kAlice, Side::ASK, 1000, 50));
    Setup(New(2, kAlice, Side::ASK, 1001, 50));

    // ACCEPTED, then two fills per level, then an IOC remainder.
    ASSERT_TRUE(Send(New(3, kBob, Side::BID, 1001, 150, TimeInForce::IOC)));
    const auto aggressive = Events();
    ASSERT_EQ(aggressive.size(), 6u);
    EXPECT_TRUE(aggressive[0].answers_request);
    EXPECT_EQ(aggressive[0].type, EngineEventType::ACCEPTED);
    for (size_t i = 1; i < aggressive.size(); ++i) {
        EXPECT_FALSE(aggressive[i].answers_request) << i;
    }

    Setup(New(4, kAlice, Side::BID, 990, 100));
    ASSERT_TRUE(Send(Replace(4, 5, kAlice, 995, 100)));
    const auto replaced = Events();
    ASSERT_EQ(replaced.size(), 1u);
    EXPECT_TRUE(replaced[0].answers_request);

    ASSERT_TRUE(Send(Cancel(5, kAlice, 0)));
    const auto canceled = Events();
    ASSERT_EQ(canceled.size(), 1u);
    EXPECT_TRUE(canceled[0].answers_request);
}

//=============================================================================
// market data backpressure
//=============================================================================

TEST(OrderbookMarketData, FullMarketDataRingDropsInsteadOfStalling) {
    OutboundRing out{1 << 6};
    MarketDataRing market_data{4};
    const auto book = std::make_unique<Orderbook>(out, market_data);

    for (OrderId id = 1; id <= 10; ++id) {
        ASSERT_TRUE(book->process(New(id, kAlice, Side::BID, 1000, 1), 0));
    }

    size_t events = 0;
    while (out.pop()) ++events;
    EXPECT_EQ(events, 10u); // every client event still delivered
    EXPECT_GT(book->market_data_drops(), 0u);
    EXPECT_EQ(book->order_count(), 10u);
}
