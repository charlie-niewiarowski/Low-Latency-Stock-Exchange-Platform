//
// Created by cniew on 10/18/26.
//

#include "order_entry.hpp"
#include "validation.hpp"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

constexpr validation::PriceBand kBand{.min = 1, .max = 100'000}; // $0.01 - $1,000.00

template <size_t N>
std::array<char, N> Pad(const char *s) {
    std::array<char, N> out{};
    out.fill(' ');
    for (size_t i = 0; i < N && s[i] != '\0'; ++i) out[i] = s[i];
    return out;
}

OuchEnterOrder MakeEnter(const uint32_t user_ref_num) {
    OuchEnterOrder m{};
    m.user_ref_num = user_ref_num;
    m.side = 'B';
    m.quantity = 500;
    m.symbol = Pad<8>("AAPL");
    m.price = 1'502'500; // $150.25
    m.time_in_force = '0';
    m.display = 'Y';
    m.capacity = 'A';
    m.intermarket_sweep_eligibility = 'N';
    m.cross_type = 'N';
    m.cl_ord_id = Pad<14>("ORDER");
    return m;
}

OuchReplaceOrder MakeReplace(const uint32_t orig_user_ref_num, const uint32_t user_ref_num) {
    OuchReplaceOrder m{};
    m.orig_user_ref_num = orig_user_ref_num;
    m.user_ref_num = user_ref_num;
    m.quantity = 600;
    m.price = 1'501'000; // $150.10
    m.time_in_force = '3';
    m.display = 'Y';
    m.intermarket_sweep_eligibility = 'N';
    m.cl_ord_id = Pad<14>("REPLACED");
    return m;
}

OuchCancelOrder MakeCancel(const uint32_t user_ref_num, const uint32_t quantity) {
    OuchCancelOrder m{};
    m.user_ref_num = user_ref_num;
    m.quantity = quantity;
    return m;
}

OuchInboundFrame Frame(const OuchEnterOrder &m)   { OuchInboundFrame f{}; f.enter_order = m;   return f; }
OuchInboundFrame Frame(const OuchReplaceOrder &m) { OuchInboundFrame f{}; f.replace_order = m; return f; }
OuchInboundFrame Frame(const OuchCancelOrder &m)  { OuchInboundFrame f{}; f.cancel_order = m;  return f; }

bool Empty(const Translation &t) { return !t.note && !t.request; }

} // namespace

//=============================================================================
// validation
//=============================================================================

TEST(Validation, PriceConversion) {
    using validation::to_engine_price;

    const auto limit = to_engine_price(1'502'500, kBand);
    ASSERT_TRUE(limit);
    EXPECT_EQ(limit->type, OrderType::LIMIT);
    EXPECT_EQ(limit->price, 15'025u);

    for (const uint64_t market : {kOuchMarketPriceFloor, uint64_t{0x7FFFFFFF}, UINT64_MAX}) {
        const auto price = to_engine_price(market, kBand);
        ASSERT_TRUE(price) << market;
        EXPECT_EQ(price->type, OrderType::MARKET);
        EXPECT_EQ(price->price, 0u);
    }

    EXPECT_FALSE(to_engine_price(1'502'501, kBand));    // sub-penny
    EXPECT_FALSE(to_engine_price(0, kBand));            // below band
    EXPECT_FALSE(to_engine_price(10'000'100, kBand));   // $1,000.01, above band
    EXPECT_TRUE(to_engine_price(10'000'000, kBand));    // $1,000.00, band edge

    // The penny above the max limit price is the market floor; anything
    // strictly between is sub-penny.
    constexpr validation::PriceBand wide{.min = 1, .max = UINT32_MAX};
    EXPECT_EQ(to_engine_price(kOuchMaxLimitPrice, wide)->type, OrderType::LIMIT);
    EXPECT_FALSE(to_engine_price(kOuchMaxLimitPrice + 50, wide));
    EXPECT_EQ(to_engine_price(kOuchMaxLimitPrice + 100, wide)->type, OrderType::MARKET);
}

TEST(Validation, AcceptsSupportedEnter) {
    OuchEnterOrder m = MakeEnter(1);
    m.firm = Pad<4>("ABCD"); m.appendage.set(OuchTag::FIRM);           // informational: allowed
    m.price_type = 'L';      m.appendage.set(OuchTag::PRICE_TYPE);     // explicit default: allowed

    const validation::EnterCheck check = validation::check_enter(m, kBand);

    EXPECT_EQ(check.reject_reason, 0);
    EXPECT_EQ(check.side, Side::BID);
    EXPECT_EQ(check.order_type, OrderType::LIMIT);
    EXPECT_EQ(check.price, 15'025u);
    EXPECT_EQ(check.time_in_force, TimeInForce::DAY);
}

TEST(Validation, RejectsUnsupportedEnter) {
    const auto reason = [](auto mutate) {
        OuchEnterOrder m = MakeEnter(1);
        mutate(m);
        return validation::check_enter(m, kBand).reject_reason;
    };

    EXPECT_EQ(reason([](auto &m) { m.side = 'T'; }), ouch_reject::kInvalidSide);
    EXPECT_EQ(reason([](auto &m) { m.quantity = 0; }), ouch_reject::kInvalidQuantity);
    EXPECT_EQ(reason([](auto &m) { m.quantity = 1'000'000; }), ouch_reject::kInvalidQuantity);
    EXPECT_EQ(reason([](auto &m) { m.price = 1'502'501; }), ouch_reject::kInvalidPrice);
    EXPECT_EQ(reason([](auto &m) { m.time_in_force = '5'; }), ouch_reject::kOther);
    EXPECT_EQ(reason([](auto &m) { m.display = 'N'; }), ouch_reject::kInvalidDisplay);
    EXPECT_EQ(reason([](auto &m) { m.capacity = 'X'; }), ouch_reject::kOther);
    EXPECT_EQ(reason([](auto &m) { m.intermarket_sweep_eligibility = 'Y'; }), ouch_reject::kIsoNotAllowed);
    EXPECT_EQ(reason([](auto &m) { m.cross_type = 'O'; }), ouch_reject::kInvalidCrossOrder);
    EXPECT_EQ(reason([](auto &m) { m.min_qty = 100; m.appendage.set(OuchTag::MIN_QTY); }),
              ouch_reject::kInvalidMinQuantity);
    EXPECT_EQ(reason([](auto &m) { m.max_floor = 100; m.appendage.set(OuchTag::MAX_FLOOR); }),
              ouch_reject::kInvalidMaxFloor);
    EXPECT_EQ(reason([](auto &m) { m.price_type = 'P'; m.appendage.set(OuchTag::PRICE_TYPE); }),
              ouch_reject::kInvalidPegType);
    EXPECT_EQ(reason([](auto &m) { m.post_only = 'P'; m.appendage.set(OuchTag::POST_ONLY); }),
              ouch_reject::kOther);
    EXPECT_EQ(reason([](auto &m) { m.appendage.set(OuchTag::USER_REF_IDX); }), ouch_reject::kOther);
}

TEST(Validation, ReplaceRules) {
    const auto valid = [](auto mutate) {
        OuchReplaceOrder m = MakeReplace(1, 2);
        mutate(m);
        return validation::check_replace(m, 'B', kBand).valid;
    };

    EXPECT_TRUE(valid([](auto &) {}));
    EXPECT_TRUE(valid([](auto &m) { m.side = 'B'; m.appendage.set(OuchTag::SIDE); }));
    EXPECT_FALSE(valid([](auto &m) { m.side = 'S'; m.appendage.set(OuchTag::SIDE); }));
    EXPECT_FALSE(valid([](auto &m) { m.quantity = 0; }));
    EXPECT_FALSE(valid([](auto &m) { m.price = kOuchMarketPriceFloor; })); // can't replace to market
    EXPECT_FALSE(valid([](auto &m) { m.time_in_force = '6'; }));
    EXPECT_FALSE(valid([](auto &m) { m.display = 'N'; }));
    EXPECT_FALSE(valid([](auto &m) { m.min_qty = 100; m.appendage.set(OuchTag::MIN_QTY); }));

    const validation::ReplaceCheck check = validation::check_replace(MakeReplace(1, 2), 'B', kBand);
    EXPECT_EQ(check.price, 15'010u);
    EXPECT_EQ(check.time_in_force, TimeInForce::IOC);
}

//=============================================================================
// OrderEntry -- Enter
//=============================================================================

TEST(OrderEntry, EnterProducesNoteThenNewRequest) {
    OrderEntry entry{kBand};
    const Translation t = entry.translate(7, Frame(MakeEnter(1)));

    ASSERT_TRUE(t.note);
    EXPECT_EQ(t.note->kind, GatewayNote::Kind::ORDER_ENTERED);
    EXPECT_EQ(t.note->order_id, 1u);
    EXPECT_EQ(t.note->record.client_id, 7u);
    EXPECT_EQ(t.note->record.user_ref_num, 1u);
    EXPECT_EQ(t.note->record.price, 1'502'500u);
    EXPECT_EQ(t.note->record.symbol, Pad<8>("AAPL"));
    EXPECT_EQ(t.note->record.cl_ord_id, Pad<14>("ORDER"));
    EXPECT_EQ(t.note->record.capacity, 'A');

    ASSERT_TRUE(t.request);
    EXPECT_EQ(t.request->type, EngineRequestType::NEW);
    EXPECT_EQ(t.request->order_id, 1u);
    EXPECT_EQ(t.request->client_id, 7u);
    EXPECT_EQ(t.request->side, Side::BID);
    EXPECT_EQ(t.request->order_type, OrderType::LIMIT);
    EXPECT_EQ(t.request->price, 15'025u);
    EXPECT_EQ(t.request->quantity, 500u);
    EXPECT_EQ(t.request->time_in_force, TimeInForce::DAY);
}

TEST(OrderEntry, MarketEnter) {
    OrderEntry entry{kBand};
    OuchEnterOrder m = MakeEnter(1);
    m.side = 'S';
    m.price = 0x7FFFFFFF;
    m.time_in_force = '3';

    const Translation t = entry.translate(1, Frame(m));

    ASSERT_TRUE(t.request);
    EXPECT_EQ(t.request->side, Side::ASK);
    EXPECT_EQ(t.request->order_type, OrderType::MARKET);
    EXPECT_EQ(t.request->price, 0u);
    EXPECT_EQ(t.request->time_in_force, TimeInForce::IOC);
}

TEST(OrderEntry, IgnoresRetransmittedUserRefNums) {
    OrderEntry entry{kBand};
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(5))).request);

    EXPECT_TRUE(Empty(entry.translate(1, Frame(MakeEnter(5)))));
    EXPECT_TRUE(Empty(entry.translate(1, Frame(MakeEnter(3)))));
    EXPECT_TRUE(entry.translate(1, Frame(MakeEnter(6))).request);
}

TEST(OrderEntry, UserRefNumsArePerClient) {
    OrderEntry entry{kBand};
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(1))).request);

    const Translation other = entry.translate(2, Frame(MakeEnter(1)));
    ASSERT_TRUE(other.request);
    EXPECT_EQ(other.request->order_id, 2u);
}

TEST(OrderEntry, RejectedEnterConsumesUserRefNumButNotAnOrderId) {
    OrderEntry entry{kBand};
    OuchEnterOrder bad = MakeEnter(1);
    bad.quantity = 0;

    const Translation t = entry.translate(1, Frame(bad));
    ASSERT_TRUE(t.note);
    EXPECT_FALSE(t.request);
    EXPECT_EQ(t.note->kind, GatewayNote::Kind::REJECTED);
    EXPECT_EQ(t.note->reject_reason, ouch_reject::kInvalidQuantity);
    EXPECT_EQ(t.note->record.user_ref_num, 1u);
    EXPECT_EQ(t.note->record.cl_ord_id, Pad<14>("ORDER"));

    EXPECT_TRUE(Empty(entry.translate(1, Frame(MakeEnter(1)))));
    const Translation next = entry.translate(1, Frame(MakeEnter(2)));
    ASSERT_TRUE(next.request);
    EXPECT_EQ(next.request->order_id, 1u);

    // A rejected order was never entered, so it can't be canceled.
    EXPECT_TRUE(Empty(entry.translate(1, Frame(MakeCancel(1, 0)))));
}

//=============================================================================
// OrderEntry -- Cancel
//=============================================================================

TEST(OrderEntry, CancelResolvesOrderIdWithoutWaitingForTheEngine) {
    OrderEntry entry{kBand};
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(1))).request);

    const Translation t = entry.translate(1, Frame(MakeCancel(1, 200)));

    EXPECT_FALSE(t.note);
    ASSERT_TRUE(t.request);
    EXPECT_EQ(t.request->type, EngineRequestType::CANCEL);
    EXPECT_EQ(t.request->order_id, 1u);
    EXPECT_EQ(t.request->client_id, 1u);
    EXPECT_EQ(t.request->quantity, 200u);
}

TEST(OrderEntry, CancelOfUnknownOrOtherClientsOrderIsIgnored) {
    OrderEntry entry{kBand};
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(1))).request);

    EXPECT_TRUE(Empty(entry.translate(1, Frame(MakeCancel(9, 0)))));
    EXPECT_TRUE(Empty(entry.translate(2, Frame(MakeCancel(1, 0)))));
}

//=============================================================================
// OrderEntry -- Replace
//=============================================================================

TEST(OrderEntry, ReplaceAssignsNewOrderIdAndKeepsOriginalFields) {
    OrderEntry entry{kBand};
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(1))).request);

    const Translation t = entry.translate(1, Frame(MakeReplace(1, 2)));

    ASSERT_TRUE(t.note);
    EXPECT_EQ(t.note->kind, GatewayNote::Kind::ORDER_REPLACING);
    EXPECT_EQ(t.note->order_id, 2u);
    EXPECT_EQ(t.note->record.user_ref_num, 2u);
    EXPECT_EQ(t.note->record.quantity, 600u);
    EXPECT_EQ(t.note->record.price, 1'501'000u);
    EXPECT_EQ(t.note->record.cl_ord_id, Pad<14>("REPLACED"));
    EXPECT_EQ(t.note->record.time_in_force, '3');
    EXPECT_EQ(t.note->record.side, 'B');                // inherited
    EXPECT_EQ(t.note->record.symbol, Pad<8>("AAPL"));   // inherited
    EXPECT_EQ(t.note->record.capacity, 'A');            // inherited

    ASSERT_TRUE(t.request);
    EXPECT_EQ(t.request->type, EngineRequestType::REPLACE);
    EXPECT_EQ(t.request->order_id, 1u);
    EXPECT_EQ(t.request->new_order_id, 2u);
    EXPECT_EQ(t.request->side, Side::BID);
    EXPECT_EQ(t.request->price, 15'010u);
    EXPECT_EQ(t.request->quantity, 600u);
    EXPECT_EQ(t.request->time_in_force, TimeInForce::IOC);

    // Both UserRefNums of the chain resolve.
    const Translation cancel_new = entry.translate(1, Frame(MakeCancel(2, 0)));
    ASSERT_TRUE(cancel_new.request);
    EXPECT_EQ(cancel_new.request->order_id, 2u);
    const Translation cancel_orig = entry.translate(1, Frame(MakeCancel(1, 0)));
    ASSERT_TRUE(cancel_orig.request);
    EXPECT_EQ(cancel_orig.request->order_id, 1u);
}

TEST(OrderEntry, ReplaceWithUnknownOriginalOrUsedUserRefNumIsIgnored) {
    OrderEntry entry{kBand};
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(1))).request);
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(2))).request);

    EXPECT_TRUE(Empty(entry.translate(1, Frame(MakeReplace(9, 3)))));
    EXPECT_TRUE(Empty(entry.translate(1, Frame(MakeReplace(1, 2)))));

    // Neither consumed UserRefNum 3.
    EXPECT_TRUE(entry.translate(1, Frame(MakeReplace(1, 3))).request);
}

TEST(OrderEntry, InvalidReplaceCancelsTheOriginalWithoutConsumingTheUserRefNum) {
    OrderEntry entry{kBand};
    ASSERT_TRUE(entry.translate(1, Frame(MakeEnter(1))).request);
    OuchReplaceOrder bad = MakeReplace(1, 2);
    bad.quantity = 1'000'000;

    const Translation t = entry.translate(1, Frame(bad));

    EXPECT_FALSE(t.note);
    ASSERT_TRUE(t.request);
    EXPECT_EQ(t.request->type, EngineRequestType::CANCEL);
    EXPECT_EQ(t.request->order_id, 1u);
    EXPECT_EQ(t.request->quantity, 0u);

    const Translation retry = entry.translate(1, Frame(MakeReplace(1, 2)));
    ASSERT_TRUE(retry.request);
    EXPECT_EQ(retry.request->type, EngineRequestType::REPLACE);
}
