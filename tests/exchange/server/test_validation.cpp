//
// Isolation tests for the four validation functions in validation.hpp.
// No networking — pure logic, runs instantly.
//

#include "test_runner.hpp"
#include "validation.hpp"

//=============================================================================
// Helpers
//=============================================================================

static InboundMessage make_new(ClientId cid, Side side, Price price, Quantity qty) {
    return {cid, 0, price, qty, MessageType::NEW, side, OrderType::LIMIT, 0};
}

static InboundMessage make_cancel(ClientId cid, OrderId oid) {
    return {cid, oid, 0, 0, MessageType::CANCEL, Side::BID, OrderType::LIMIT, 0};
}

static InboundMessage make_modify(ClientId cid, OrderId oid, Side side, Price price, Quantity qty) {
    return {cid, oid, price, qty, MessageType::MODIFY, side, OrderType::LIMIT, 0};
}

//=============================================================================
// Validate New
//=============================================================================

static bool validate_new_BidLimitValid() {
    ASSERT_TRUE(validate_new(make_new(1, Side::BID, 100, 10)));
    return true;
}

static bool validate_new_AskLimitValid() {
    ASSERT_TRUE(validate_new(make_new(1, Side::ASK, 100, 10)));
    return true;
}

static bool validate_new_BidPriceZeroFails() {
    // BID price must be > 0 (Price is uint32_t, so <= 0 means == 0)
    ASSERT_FALSE(validate_new(make_new(1, Side::BID, 0, 10)));
    return true;
}

static bool validate_new_AskPriceZeroPasses() {
    // ASK allows price = 0 (market order semantics)
    ASSERT_TRUE(validate_new(make_new(1, Side::ASK, 0, 10)));
    return true;
}

static bool validate_new_ZeroQuantityPasses() {
    ASSERT_TRUE(validate_new(make_new(1, Side::BID, 100, 0)));
    return true;
}

static bool validate_new_MaxValidQuantityPasses() {
    ASSERT_TRUE(validate_new(make_new(1, Side::BID, 100, static_cast<Quantity>(1e6))));
    return true;
}

static bool validate_new_ExcessiveQuantityFails() {
    ASSERT_FALSE(validate_new(make_new(1, Side::BID, 100, static_cast<Quantity>(1e6) + 1)));
    return true;
}

static bool validate_new_WrongMessageTypeFails() {
    InboundMessage msg = make_new(1, Side::BID, 100, 10);
    msg.message_type = MessageType::CANCEL;
    ASSERT_FALSE(validate_new(msg));
    return true;
}

static bool validate_new_InvalidSideFails() {
    InboundMessage msg = make_new(1, Side::BID, 100, 10);
    msg.side = static_cast<Side>(99);
    ASSERT_FALSE(validate_new(msg));
    return true;
}

static bool validate_new_InvalidOrderTypeFails() {
    InboundMessage msg = make_new(1, Side::BID, 100, 10);
    msg.order_type = static_cast<OrderType>(99);
    ASSERT_FALSE(validate_new(msg));
    return true;
}

static bool validate_new_MarketAskValid() {
    // OrderType::MARKET (0) with ASK
    InboundMessage msg = make_new(1, Side::ASK, 0, 5);
    msg.order_type = OrderType::MARKET;
    ASSERT_TRUE(validate_new(msg));
    return true;
}

//=============================================================================
// Validate Cancel
//=============================================================================

static bool validate_cancel_ValidCancel() {
    ASSERT_TRUE(validate_cancel(make_cancel(1, 42)));
    return true;
}

static bool validate_cancel_WrongMessageTypeFails() {
    InboundMessage msg = make_cancel(1, 42);
    msg.message_type = MessageType::NEW;
    ASSERT_FALSE(validate_cancel(msg));
    return true;
}

static bool validate_cancel_ModifyTypeFailsToo() {
    InboundMessage msg = make_cancel(1, 42);
    msg.message_type = MessageType::MODIFY;
    ASSERT_FALSE(validate_cancel(msg));
    return true;
}

static bool validate_cancel_AnyOrderIdValid() {
    // cancel doesn't care about price, qty, side — only message_type
    ASSERT_TRUE(validate_cancel(make_cancel(1, 0)));
    ASSERT_TRUE(validate_cancel(make_cancel(1, 999999)));
    return true;
}

//=============================================================================
// Validate Modify
//=============================================================================

static bool validate_modify_ValidModify() {
    ASSERT_TRUE(validate_modify(make_modify(1, 42, Side::BID, 100, 10)));
    return true;
}

static bool validate_modify_WrongMessageTypeFails() {
    InboundMessage msg = make_modify(1, 42, Side::BID, 100, 10);
    msg.message_type = MessageType::NEW;
    ASSERT_FALSE(validate_modify(msg));
    return true;
}

static bool validate_modify_BidZeroPriceFails() {
    ASSERT_FALSE(validate_modify(make_modify(1, 42, Side::BID, 0, 10)));
    return true;
}

static bool validate_modify_AskZeroPricePasses() {
    ASSERT_TRUE(validate_modify(make_modify(1, 42, Side::ASK, 0, 10)));
    return true;
}

static bool validate_modify_ExcessiveQuantityFails() {
    ASSERT_FALSE(validate_modify(make_modify(1, 42, Side::BID, 100, static_cast<Quantity>(1e6) + 1)));
    return true;
}

static bool validate_modify_InvalidSideFails() {
    InboundMessage msg = make_modify(1, 42, Side::BID, 100, 10);
    msg.side = static_cast<Side>(99);
    ASSERT_FALSE(validate_modify(msg));
    return true;
}

static bool validate_modify_InvalidOrderTypeFails() {
    InboundMessage msg = make_modify(1, 42, Side::BID, 100, 10);
    msg.order_type = static_cast<OrderType>(99);
    ASSERT_FALSE(validate_modify(msg));
    return true;
}

//=============================================================================
// Vaidate Message Dispatch
//=============================================================================

static bool validate_message_DispatchesNEW() {
    // valid NEW → true
    ASSERT_TRUE(validate_message(make_new(1, Side::BID, 100, 10)));
    // invalid NEW (zero bid price) → false
    ASSERT_FALSE(validate_message(make_new(1, Side::BID, 0, 10)));
    return true;
}

static bool validate_message_DispatchesCANCEL() {
    ASSERT_TRUE(validate_message(make_cancel(1, 42)));
    return true;
}

static bool validate_message_DispatchesMODIFY() {
    ASSERT_TRUE(validate_message(make_modify(1, 42, Side::BID, 100, 10)));
    return true;
}

static bool validate_message_UnknownTypeFails() {
    // MessageType::MATCH is not a client-originated type
    InboundMessage msg{};
    msg.message_type = MessageType::MATCH;
    ASSERT_FALSE(validate_message(msg));
    return true;
}

//=============================================================================
// main()
//=============================================================================

int main() {
    return run_tests("Validation", {
        {"validate_new: bid limit valid",              validate_new_BidLimitValid},
        {"validate_new: ask limit valid",              validate_new_AskLimitValid},
        {"validate_new: bid price=0 fails",            validate_new_BidPriceZeroFails},
        {"validate_new: ask price=0 passes",           validate_new_AskPriceZeroPasses},
        {"validate_new: qty=0 passes",                 validate_new_ZeroQuantityPasses},
        {"validate_new: qty=1e6 passes",               validate_new_MaxValidQuantityPasses},
        {"validate_new: qty>1e6 fails",                validate_new_ExcessiveQuantityFails},
        {"validate_new: wrong message type fails",     validate_new_WrongMessageTypeFails},
        {"validate_new: invalid side fails",           validate_new_InvalidSideFails},
        {"validate_new: invalid order type fails",     validate_new_InvalidOrderTypeFails},
        {"validate_new: market ask valid",             validate_new_MarketAskValid},

        {"validate_cancel: valid cancel",              validate_cancel_ValidCancel},
        {"validate_cancel: wrong message type fails",  validate_cancel_WrongMessageTypeFails},
        {"validate_cancel: MODIFY type fails",         validate_cancel_ModifyTypeFailsToo},
        {"validate_cancel: any order_id valid",        validate_cancel_AnyOrderIdValid},

        {"validate_modify: valid modify",              validate_modify_ValidModify},
        {"validate_modify: wrong message type fails",  validate_modify_WrongMessageTypeFails},
        {"validate_modify: bid price=0 fails",         validate_modify_BidZeroPriceFails},
        {"validate_modify: ask price=0 passes",        validate_modify_AskZeroPricePasses},
        {"validate_modify: qty>1e6 fails",             validate_modify_ExcessiveQuantityFails},
        {"validate_modify: invalid side fails",        validate_modify_InvalidSideFails},
        {"validate_modify: invalid order type fails",  validate_modify_InvalidOrderTypeFails},

        {"validate_message: dispatches NEW",           validate_message_DispatchesNEW},
        {"validate_message: dispatches CANCEL",        validate_message_DispatchesCANCEL},
        {"validate_message: dispatches MODIFY",        validate_message_DispatchesMODIFY},
        {"validate_message: unknown type fails",       validate_message_UnknownTypeFails},
    });
}