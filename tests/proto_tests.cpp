//
// Created by cniew on 10/18/26.
//
// Byte-level tests for the OUCH 5.0 / ITCH 5.0 wire codecs. Every offset
// and length asserted here is taken from the spec's message tables, so a
// failure means the encoder disagrees with Nasdaq, not just with itself.
//

#include "ouch_proto.hpp"
#include "itch_proto.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

namespace {

using Bytes = std::vector<std::byte>;

template <typename Frame>
Bytes Encode(const Frame &frame) {
    Bytes out(encoded_size(frame));
    size_t written = 0;
    if constexpr (std::is_same_v<Frame, OuchInboundFrame>) {
        written = encode_inbound_ouch(frame, out);
    } else if constexpr (std::is_same_v<Frame, OuchOutboundFrame>) {
        written = encode_outbound_ouch(frame, out);
    } else {
        written = encode_itch(frame, out);
    }
    EXPECT_EQ(written, out.size());
    return out;
}

// Big-endian unsigned value of bytes[offset, offset + len).
uint64_t BE(const Bytes &bytes, size_t offset, size_t len) {
    uint64_t value = 0;
    for (size_t i = 0; i < len; ++i) {
        value = (value << 8) | static_cast<uint8_t>(bytes.at(offset + i));
    }
    return value;
}

char At(const Bytes &bytes, size_t offset) { return static_cast<char>(bytes.at(offset)); }

template <size_t N>
std::array<char, N> Alpha(const Bytes &bytes, size_t offset) {
    std::array<char, N> out{};
    for (size_t i = 0; i < N; ++i) out[i] = At(bytes, offset + i);
    return out;
}

template <size_t N>
std::array<char, N> Pad(const char *s) {
    std::array<char, N> out{};
    out.fill(' ');
    for (size_t i = 0; i < N && s[i] != '\0'; ++i) out[i] = s[i];
    return out;
}

void Push(Bytes &bytes, std::initializer_list<uint8_t> values) {
    for (uint8_t v : values) bytes.push_back(static_cast<std::byte>(v));
}

OuchEnterOrder MakeEnter() {
    OuchEnterOrder m{};
    m.user_ref_num = 0x01020304;
    m.side = 'B';
    m.quantity = 500;
    m.symbol = Pad<8>("AAPL");
    m.price = 1'502'500; // $150.25
    m.time_in_force = '0';
    m.display = 'Y';
    m.capacity = 'A';
    m.intermarket_sweep_eligibility = 'N';
    m.cross_type = 'N';
    m.cl_ord_id = Pad<14>("ORDER1");
    return m;
}

OuchInboundFrame Frame(const OuchEnterOrder &m)   { OuchInboundFrame f{}; f.enter_order = m;   return f; }
OuchInboundFrame Frame(const OuchReplaceOrder &m) { OuchInboundFrame f{}; f.replace_order = m; return f; }
OuchInboundFrame Frame(const OuchCancelOrder &m)  { OuchInboundFrame f{}; f.cancel_order = m;  return f; }

} // namespace

//=============================================================================
// OUCH inbound
//=============================================================================

TEST(OuchInbound, EnterOrderCoreLayoutMatchesSpec) {
    const OuchEnterOrder m = MakeEnter();
    const Bytes b = Encode(Frame(m));

    ASSERT_EQ(b.size(), 47u); // 45 core + 2 Appendage Length, no tags
    EXPECT_EQ(At(b, 0), 'O');
    EXPECT_EQ(BE(b, 1, 4), 0x01020304u);
    EXPECT_EQ(At(b, 5), 'B');
    EXPECT_EQ(BE(b, 6, 4), 500u);
    EXPECT_EQ(Alpha<8>(b, 10), m.symbol);
    EXPECT_EQ(BE(b, 18, 8), 1'502'500u);
    EXPECT_EQ(At(b, 26), '0');
    EXPECT_EQ(At(b, 27), 'Y');
    EXPECT_EQ(At(b, 28), 'A');
    EXPECT_EQ(At(b, 29), 'N');
    EXPECT_EQ(At(b, 30), 'N');
    EXPECT_EQ(Alpha<14>(b, 31), m.cl_ord_id);
    EXPECT_EQ(BE(b, 45, 2), 0u);
}

TEST(OuchInbound, EnterOrderEmitsOnlyPresentTagsInTagOrder) {
    OuchEnterOrder m = MakeEnter();
    m.user_ref_idx = 9;   m.appendage.set(OuchTag::USER_REF_IDX);
    m.firm = Pad<4>("ABCD"); m.appendage.set(OuchTag::FIRM);
    m.min_qty = 100;      m.appendage.set(OuchTag::MIN_QTY);
    m.max_floor = 50;     // set but not marked present: must not be emitted

    const Bytes b = Encode(Frame(m));

    // Firm: 1+1+4, MinQty: 1+1+4, UserRefIdx: 1+1+1
    ASSERT_EQ(b.size(), 47u + 6 + 6 + 3);
    EXPECT_EQ(BE(b, 45, 2), 15u);
    EXPECT_EQ(BE(b, 47, 1), 5u);  EXPECT_EQ(BE(b, 48, 1), 2u);  EXPECT_EQ(Alpha<4>(b, 49), m.firm);
    EXPECT_EQ(BE(b, 53, 1), 5u);  EXPECT_EQ(BE(b, 54, 1), 3u);  EXPECT_EQ(BE(b, 55, 4), 100u);
    EXPECT_EQ(BE(b, 59, 1), 2u);  EXPECT_EQ(BE(b, 60, 1), 28u); EXPECT_EQ(BE(b, 61, 1), 9u);
}

TEST(OuchInbound, EnterOrderRoundTripsCoreAndAppendage) {
    OuchEnterOrder m = MakeEnter();
    m.peg_offset = -125; m.appendage.set(OuchTag::PEG_OFFSET); // signed price
    m.group_id = 0xBEEF; m.appendage.set(OuchTag::GROUP_ID);

    const OuchInboundFrame decoded = decode_inbound_ouch(Encode(Frame(m)));

    ASSERT_EQ(decoded.message_type, InboundOuchMessageType::ENTER_ORDER);
    const OuchEnterOrder &d = decoded.enter_order;
    EXPECT_EQ(d.user_ref_num, m.user_ref_num);
    EXPECT_EQ(d.side, m.side);
    EXPECT_EQ(d.quantity, m.quantity);
    EXPECT_EQ(d.symbol, m.symbol);
    EXPECT_EQ(d.price, m.price);
    EXPECT_EQ(d.time_in_force, m.time_in_force);
    EXPECT_EQ(d.display, m.display);
    EXPECT_EQ(d.capacity, m.capacity);
    EXPECT_EQ(d.intermarket_sweep_eligibility, m.intermarket_sweep_eligibility);
    EXPECT_EQ(d.cross_type, m.cross_type);
    EXPECT_EQ(d.cl_ord_id, m.cl_ord_id);

    EXPECT_EQ(d.peg_offset, -125);
    EXPECT_EQ(d.group_id, 0xBEEF);
    EXPECT_TRUE(d.appendage.has(OuchTag::PEG_OFFSET));
    EXPECT_TRUE(d.appendage.has(OuchTag::GROUP_ID));

    // Absent tags keep their spec defaults and aren't marked present.
    EXPECT_FALSE(d.appendage.has(OuchTag::FIRM));
    EXPECT_EQ(d.firm, Pad<4>(""));
    EXPECT_EQ(d.price_type, 'L');
    EXPECT_EQ(d.aiq_strategy, '*');
}

TEST(OuchInbound, ReplaceOrderLayoutMatchesSpec) {
    OuchReplaceOrder m{};
    m.orig_user_ref_num = 10;
    m.user_ref_num = 11;
    m.quantity = 600;
    m.price = 1'503'000;
    m.time_in_force = '3';
    m.display = 'N';
    m.intermarket_sweep_eligibility = 'N';
    m.cl_ord_id = Pad<14>("ORDER2");
    m.side = 'S'; m.appendage.set(OuchTag::SIDE);

    const Bytes b = Encode(Frame(m));

    ASSERT_EQ(b.size(), 40u + 3);
    EXPECT_EQ(At(b, 0), 'U');
    EXPECT_EQ(BE(b, 1, 4), 10u);
    EXPECT_EQ(BE(b, 5, 4), 11u);
    EXPECT_EQ(BE(b, 9, 4), 600u);
    EXPECT_EQ(BE(b, 13, 8), 1'503'000u);
    EXPECT_EQ(At(b, 21), '3');
    EXPECT_EQ(At(b, 22), 'N');
    EXPECT_EQ(At(b, 23), 'N');
    EXPECT_EQ(Alpha<14>(b, 24), m.cl_ord_id);
    EXPECT_EQ(BE(b, 38, 2), 3u);
    EXPECT_EQ(BE(b, 40, 1), 2u); EXPECT_EQ(BE(b, 41, 1), 27u); EXPECT_EQ(At(b, 42), 'S');

    const OuchInboundFrame decoded = decode_inbound_ouch(b);
    ASSERT_EQ(decoded.message_type, InboundOuchMessageType::REPLACE_ORDER);
    EXPECT_EQ(decoded.replace_order.orig_user_ref_num, 10u);
    EXPECT_EQ(decoded.replace_order.user_ref_num, 11u);
    EXPECT_EQ(decoded.replace_order.quantity, 600u);
    EXPECT_EQ(decoded.replace_order.price, 1'503'000u);
    EXPECT_EQ(decoded.replace_order.cl_ord_id, m.cl_ord_id);
    EXPECT_TRUE(decoded.replace_order.appendage.has(OuchTag::SIDE));
    EXPECT_EQ(decoded.replace_order.side, 'S');
}

TEST(OuchInbound, CancelOrderLayoutMatchesSpec) {
    OuchCancelOrder m{};
    m.user_ref_num = 0xAABBCCDD;
    m.quantity = 0;

    const Bytes b = Encode(Frame(m));

    ASSERT_EQ(b.size(), 11u);
    EXPECT_EQ(At(b, 0), 'X');
    EXPECT_EQ(BE(b, 1, 4), 0xAABBCCDDu);
    EXPECT_EQ(BE(b, 5, 4), 0u);
    EXPECT_EQ(BE(b, 9, 2), 0u);

    const OuchInboundFrame decoded = decode_inbound_ouch(b);
    ASSERT_EQ(decoded.message_type, InboundOuchMessageType::CANCEL_ORDER);
    EXPECT_EQ(decoded.cancel_order.user_ref_num, 0xAABBCCDDu);
    EXPECT_TRUE(decoded.cancel_order.appendage.empty());
}

TEST(OuchInbound, AppendageLengthMayBeOmitted) {
    OuchCancelOrder m{};
    m.user_ref_num = 3;
    m.quantity = 40;
    Bytes b = Encode(Frame(m));
    b.resize(9); // drop the 2-byte Appendage Length

    const OuchInboundFrame decoded = decode_inbound_ouch(b);
    ASSERT_EQ(decoded.message_type, InboundOuchMessageType::CANCEL_ORDER);
    EXPECT_EQ(decoded.cancel_order.user_ref_num, 3u);
    EXPECT_EQ(decoded.cancel_order.quantity, 40u);
}

TEST(OuchInbound, SkipsUnknownAndUnsupportedTags) {
    OuchCancelOrder m{};
    m.user_ref_num = 3;
    m.quantity = 0;
    Bytes b = Encode(Frame(m));
    b.resize(9);
    // Appendage Length 13: unknown tag 99 (2-byte value, 4 bytes), Firm
    // (not supported on Cancel, 6 bytes), UserRefIdx = 4 (3 bytes).
    Push(b, {0x00, 13, 0x03, 99, 0xDE, 0xAD, 0x05, 2, 'A', 'B', 'C', 'D', 0x02, 28, 4});

    const OuchInboundFrame decoded = decode_inbound_ouch(b);
    ASSERT_EQ(decoded.message_type, InboundOuchMessageType::CANCEL_ORDER);
    EXPECT_TRUE(decoded.cancel_order.appendage.has(OuchTag::USER_REF_IDX));
    EXPECT_EQ(decoded.cancel_order.user_ref_idx, 4u);
}

TEST(OuchInbound, RejectsMalformedMessages) {
    const Bytes enter = Encode(Frame(MakeEnter()));

    EXPECT_EQ(decode_inbound_ouch({}).message_type, InboundOuchMessageType::NONE);

    Bytes unknown_type = enter;
    unknown_type[0] = std::byte{'Z'};
    EXPECT_EQ(decode_inbound_ouch(unknown_type).message_type, InboundOuchMessageType::NONE);

    Bytes short_core(enter.begin(), enter.begin() + 44);
    EXPECT_EQ(decode_inbound_ouch(short_core).message_type, InboundOuchMessageType::NONE);

    Bytes half_length(enter.begin(), enter.begin() + 46); // 1 of 2 Appendage Length bytes
    EXPECT_EQ(decode_inbound_ouch(half_length).message_type, InboundOuchMessageType::NONE);

    Bytes length_mismatch = enter;
    length_mismatch[46] = std::byte{3}; // claims 3 appendage bytes, has 0
    EXPECT_EQ(decode_inbound_ouch(length_mismatch).message_type, InboundOuchMessageType::NONE);

    Bytes trailing = enter;
    Push(trailing, {0x00}); // byte past a zero-length appendage
    EXPECT_EQ(decode_inbound_ouch(trailing).message_type, InboundOuchMessageType::NONE);

    Bytes overrun = enter;
    overrun[46] = std::byte{2};
    Push(overrun, {0x05, 3}); // TagValue says 5 more bytes, only 1 remains
    EXPECT_EQ(decode_inbound_ouch(overrun).message_type, InboundOuchMessageType::NONE);

    Bytes zero_length_tag = enter;
    zero_length_tag[46] = std::byte{1};
    Push(zero_length_tag, {0x00});
    EXPECT_EQ(decode_inbound_ouch(zero_length_tag).message_type, InboundOuchMessageType::NONE);

    Bytes wrong_width = enter;
    wrong_width[46] = std::byte{4};
    Push(wrong_width, {0x03, 3, 0x00, 0x64}); // MinQty with a 2-byte value instead of 4
    EXPECT_EQ(decode_inbound_ouch(wrong_width).message_type, InboundOuchMessageType::NONE);
}

TEST(OuchInbound, EncodeFailsCleanlyIntoShortBuffer) {
    const OuchInboundFrame frame = Frame(MakeEnter());
    std::array<std::byte, 46> out{};
    EXPECT_EQ(encode_inbound_ouch(frame, out), 0u);
    EXPECT_EQ(encode_inbound_ouch(OuchInboundFrame{}, out), 0u);
}

//=============================================================================
// OUCH outbound
//=============================================================================

TEST(OuchOutbound, OrderAcceptedLayoutMatchesSpec) {
    OuchOutboundFrame f{};
    f.order_accepted = OuchOrderAccepted{};
    OuchOrderAccepted &m = f.order_accepted;
    m.timestamp = 0x0102030405060708;
    m.user_ref_num = 77;
    m.side = 'S';
    m.quantity = 300;
    m.symbol = Pad<8>("MSFT");
    m.price = 4'000'000;
    m.time_in_force = '0';
    m.display = 'Y';
    m.order_reference_number = 0x1122334455667788;
    m.capacity = 'P';
    m.intermarket_sweep_eligibility = 'N';
    m.cross_type = 'N';
    m.order_state = 'L';
    m.cl_ord_id = Pad<14>("ACK");

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 64u);
    EXPECT_EQ(At(b, 0), 'A');
    EXPECT_EQ(BE(b, 1, 8), 0x0102030405060708u);
    EXPECT_EQ(BE(b, 9, 4), 77u);
    EXPECT_EQ(At(b, 13), 'S');
    EXPECT_EQ(BE(b, 14, 4), 300u);
    EXPECT_EQ(Alpha<8>(b, 18), m.symbol);
    EXPECT_EQ(BE(b, 26, 8), 4'000'000u);
    EXPECT_EQ(At(b, 34), '0');
    EXPECT_EQ(At(b, 35), 'Y');
    EXPECT_EQ(BE(b, 36, 8), 0x1122334455667788u);
    EXPECT_EQ(At(b, 44), 'P');
    EXPECT_EQ(At(b, 45), 'N');
    EXPECT_EQ(At(b, 46), 'N');
    EXPECT_EQ(At(b, 47), 'L');
    EXPECT_EQ(Alpha<14>(b, 48), m.cl_ord_id);
    EXPECT_EQ(BE(b, 62, 2), 0u);

    const OuchOutboundFrame decoded = decode_outbound_ouch(b);
    ASSERT_EQ(decoded.message_type, OutboundOuchMessageType::ORDER_ACCEPTED);
    EXPECT_EQ(decoded.order_accepted.order_reference_number, m.order_reference_number);
    EXPECT_EQ(decoded.order_accepted.order_state, 'L');
}

TEST(OuchOutbound, OrderReplacedLayoutMatchesSpec) {
    OuchOutboundFrame f{};
    f.order_replaced = OuchOrderReplaced{};
    OuchOrderReplaced &m = f.order_replaced;
    m.timestamp = 5;
    m.orig_user_ref_num = 10;
    m.user_ref_num = 11;
    m.side = 'B';
    m.quantity = 600;
    m.symbol = Pad<8>("AAPL");
    m.price = 1'503'000;
    m.time_in_force = '0';
    m.display = 'Y';
    m.order_reference_number = 42;
    m.capacity = 'A';
    m.intermarket_sweep_eligibility = 'N';
    m.cross_type = 'N';
    m.order_state = 'L';
    m.cl_ord_id = Pad<14>("ORDER2");

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 68u);
    EXPECT_EQ(At(b, 0), 'U');
    EXPECT_EQ(BE(b, 9, 4), 10u);
    EXPECT_EQ(BE(b, 13, 4), 11u);
    EXPECT_EQ(At(b, 17), 'B');
    EXPECT_EQ(BE(b, 18, 4), 600u);
    EXPECT_EQ(Alpha<8>(b, 22), m.symbol);
    EXPECT_EQ(BE(b, 30, 8), 1'503'000u);
    EXPECT_EQ(BE(b, 40, 8), 42u);
    EXPECT_EQ(At(b, 48), 'A');
    EXPECT_EQ(At(b, 51), 'L');
    EXPECT_EQ(Alpha<14>(b, 52), m.cl_ord_id);
    EXPECT_EQ(BE(b, 66, 2), 0u);

    const OuchOutboundFrame decoded = decode_outbound_ouch(b);
    ASSERT_EQ(decoded.message_type, OutboundOuchMessageType::ORDER_REPLACED);
    EXPECT_EQ(decoded.order_replaced.orig_user_ref_num, 10u);
    EXPECT_EQ(decoded.order_replaced.user_ref_num, 11u);
}

TEST(OuchOutbound, OrderCanceledLayoutMatchesSpec) {
    OuchOutboundFrame f{};
    f.order_canceled = OuchOrderCanceled{};
    f.order_canceled.timestamp = 9;
    f.order_canceled.user_ref_num = 12;
    f.order_canceled.decrement_shares = 250;
    f.order_canceled.reason = 'U';

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 20u);
    EXPECT_EQ(At(b, 0), 'C');
    EXPECT_EQ(BE(b, 1, 8), 9u);
    EXPECT_EQ(BE(b, 9, 4), 12u);
    EXPECT_EQ(BE(b, 13, 4), 250u);
    EXPECT_EQ(At(b, 17), 'U');
    EXPECT_EQ(BE(b, 18, 2), 0u);

    const OuchOutboundFrame decoded = decode_outbound_ouch(b);
    ASSERT_EQ(decoded.message_type, OutboundOuchMessageType::ORDER_CANCELED);
    EXPECT_EQ(decoded.order_canceled.decrement_shares, 250u);
    EXPECT_EQ(decoded.order_canceled.reason, 'U');
}

TEST(OuchOutbound, OrderExecutedLayoutMatchesSpec) {
    OuchOutboundFrame f{};
    f.order_executed = OuchOrderExecuted{};
    f.order_executed.timestamp = 1;
    f.order_executed.user_ref_num = 13;
    f.order_executed.executed_shares = 100;
    f.order_executed.execution_price = 1'502'500;
    f.order_executed.liquidity_flag = 'A';
    f.order_executed.match_number = 0xCAFEBABE;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 36u);
    EXPECT_EQ(At(b, 0), 'E');
    EXPECT_EQ(BE(b, 9, 4), 13u);
    EXPECT_EQ(BE(b, 13, 4), 100u);
    EXPECT_EQ(BE(b, 17, 8), 1'502'500u);
    EXPECT_EQ(At(b, 25), 'A');
    EXPECT_EQ(BE(b, 26, 8), 0xCAFEBABEu);
    EXPECT_EQ(BE(b, 34, 2), 0u);

    const OuchOutboundFrame decoded = decode_outbound_ouch(b);
    ASSERT_EQ(decoded.message_type, OutboundOuchMessageType::ORDER_EXECUTED);
    EXPECT_EQ(decoded.order_executed.match_number, 0xCAFEBABEu);
}

TEST(OuchOutbound, OrderRejectedLayoutMatchesSpec) {
    OuchOutboundFrame f{};
    f.order_rejected = OuchOrderRejected{};
    f.order_rejected.timestamp = 2;
    f.order_rejected.user_ref_num = 14;
    f.order_rejected.reason = 0x0102;
    f.order_rejected.cl_ord_id = Pad<14>("BAD");
    f.order_rejected.user_ref_idx = 6;
    f.order_rejected.appendage.set(OuchTag::USER_REF_IDX);

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 31u + 3);
    EXPECT_EQ(At(b, 0), 'J');
    EXPECT_EQ(BE(b, 9, 4), 14u);
    EXPECT_EQ(BE(b, 13, 2), 0x0102u);
    EXPECT_EQ(Alpha<14>(b, 15), f.order_rejected.cl_ord_id);
    EXPECT_EQ(BE(b, 29, 2), 3u);

    const OuchOutboundFrame decoded = decode_outbound_ouch(b);
    ASSERT_EQ(decoded.message_type, OutboundOuchMessageType::ORDER_REJECTED);
    EXPECT_EQ(decoded.order_rejected.reason, 0x0102u);
    EXPECT_EQ(decoded.order_rejected.user_ref_idx, 6u);
}

TEST(OuchOutbound, OutboundTypeBytesDoNotDecodeAsInbound) {
    OuchOutboundFrame f{};
    f.order_canceled = OuchOrderCanceled{};
    f.order_canceled.reason = 'U';
    // 'C' isn't an inbound type at all.
    EXPECT_EQ(decode_inbound_ouch(Encode(f)).message_type, InboundOuchMessageType::NONE);
}

//=============================================================================
// ITCH
//=============================================================================

namespace {

ItchHeader MakeHeader(ItchMessageType type) {
    return ItchHeader{.message_type = type, .stock_locate = 0x0102, .tracking_number = 0x0304,
                      .timestamp = 0x0000A1B2C3D4E5F6};
}

void ExpectHeader(const Bytes &b, char type) {
    EXPECT_EQ(At(b, 0), type);
    EXPECT_EQ(BE(b, 1, 2), 0x0102u);
    EXPECT_EQ(BE(b, 3, 2), 0x0304u);
    EXPECT_EQ(BE(b, 5, 6), 0xA1B2C3D4E5F6u);
}

} // namespace

TEST(Itch, AddOrderLayoutMatchesSpec) {
    ITCH f{};
    f.add_order = ItchAddOrder{};
    f.add_order.header = MakeHeader(ItchMessageType::ADD_ORDER);
    f.add_order.order_reference_number = 0x1122334455667788;
    f.add_order.buy_sell_indicator = 'B';
    f.add_order.shares = 100;
    f.add_order.stock = Pad<8>("AAPL");
    f.add_order.price = 1'502'500;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 36u);
    ExpectHeader(b, 'A');
    EXPECT_EQ(BE(b, 11, 8), 0x1122334455667788u);
    EXPECT_EQ(At(b, 19), 'B');
    EXPECT_EQ(BE(b, 20, 4), 100u);
    EXPECT_EQ(Alpha<8>(b, 24), f.add_order.stock);
    EXPECT_EQ(BE(b, 32, 4), 1'502'500u);

    const ITCH decoded = decode_itch(b);
    ASSERT_EQ(decoded.header.message_type, ItchMessageType::ADD_ORDER);
    EXPECT_EQ(decoded.header.stock_locate, 0x0102);
    EXPECT_EQ(decoded.header.tracking_number, 0x0304);
    EXPECT_EQ(decoded.header.timestamp, 0xA1B2C3D4E5F6u);
    EXPECT_EQ(decoded.add_order.order_reference_number, 0x1122334455667788u);
    EXPECT_EQ(decoded.add_order.shares, 100u);
    EXPECT_EQ(decoded.add_order.stock, f.add_order.stock);
    EXPECT_EQ(decoded.add_order.price, 1'502'500u);
}

TEST(Itch, OrderExecutedLayoutMatchesSpec) {
    ITCH f{};
    f.order_executed = ItchOrderExecuted{};
    f.order_executed.header = MakeHeader(ItchMessageType::ORDER_EXECUTED);
    f.order_executed.order_reference_number = 7;
    f.order_executed.executed_shares = 50;
    f.order_executed.match_number = 99;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 31u);
    ExpectHeader(b, 'E');
    EXPECT_EQ(BE(b, 11, 8), 7u);
    EXPECT_EQ(BE(b, 19, 4), 50u);
    EXPECT_EQ(BE(b, 23, 8), 99u);
    EXPECT_EQ(decode_itch(b).order_executed.match_number, 99u);
}

TEST(Itch, OrderExecutedWithPriceLayoutMatchesSpec) {
    ITCH f{};
    f.order_executed_with_price = ItchOrderExecutedWithPrice{};
    f.order_executed_with_price.header = MakeHeader(ItchMessageType::ORDER_EXECUTED_W_PRICE);
    f.order_executed_with_price.order_reference_number = 7;
    f.order_executed_with_price.executed_shares = 50;
    f.order_executed_with_price.match_number = 99;
    f.order_executed_with_price.printable = 'Y';
    f.order_executed_with_price.execution_price = 1'500'000;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 36u);
    ExpectHeader(b, 'C');
    EXPECT_EQ(BE(b, 23, 8), 99u);
    EXPECT_EQ(At(b, 31), 'Y');
    EXPECT_EQ(BE(b, 32, 4), 1'500'000u);
    EXPECT_EQ(decode_itch(b).order_executed_with_price.execution_price, 1'500'000u);
}

TEST(Itch, OrderCancelLayoutMatchesSpec) {
    ITCH f{};
    f.order_cancel = ItchOrderCancel{};
    f.order_cancel.header = MakeHeader(ItchMessageType::ORDER_CANCEL);
    f.order_cancel.order_reference_number = 7;
    f.order_cancel.canceled_shares = 25;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 23u);
    ExpectHeader(b, 'X');
    EXPECT_EQ(BE(b, 11, 8), 7u);
    EXPECT_EQ(BE(b, 19, 4), 25u);
    EXPECT_EQ(decode_itch(b).order_cancel.canceled_shares, 25u);
}

TEST(Itch, OrderDeleteLayoutMatchesSpec) {
    ITCH f{};
    f.order_delete = ItchOrderDelete{};
    f.order_delete.header = MakeHeader(ItchMessageType::ORDER_DELETE);
    f.order_delete.order_reference_number = 7;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 19u);
    ExpectHeader(b, 'D');
    EXPECT_EQ(BE(b, 11, 8), 7u);
    EXPECT_EQ(decode_itch(b).order_delete.order_reference_number, 7u);
}

TEST(Itch, OrderReplaceLayoutMatchesSpec) {
    ITCH f{};
    f.order_replace = ItchOrderReplace{};
    f.order_replace.header = MakeHeader(ItchMessageType::ORDER_REPLACE);
    f.order_replace.original_order_reference_number = 7;
    f.order_replace.new_order_reference_number = 8;
    f.order_replace.shares = 300;
    f.order_replace.price = 1'501'000;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 35u);
    ExpectHeader(b, 'U');
    EXPECT_EQ(BE(b, 11, 8), 7u);
    EXPECT_EQ(BE(b, 19, 8), 8u);
    EXPECT_EQ(BE(b, 27, 4), 300u);
    EXPECT_EQ(BE(b, 31, 4), 1'501'000u);
    EXPECT_EQ(decode_itch(b).order_replace.new_order_reference_number, 8u);
}

TEST(Itch, TradeLayoutMatchesSpec) {
    ITCH f{};
    f.trade = ItchTrade{};
    f.trade.header = MakeHeader(ItchMessageType::TRADE);
    f.trade.order_reference_number = 0;
    f.trade.buy_sell_indicator = 'B';
    f.trade.shares = 10;
    f.trade.stock = Pad<8>("AAPL");
    f.trade.price = 1'502'500;
    f.trade.match_number = 1234;

    const Bytes b = Encode(f);

    ASSERT_EQ(b.size(), 44u);
    ExpectHeader(b, 'P');
    EXPECT_EQ(At(b, 19), 'B');
    EXPECT_EQ(BE(b, 20, 4), 10u);
    EXPECT_EQ(Alpha<8>(b, 24), f.trade.stock);
    EXPECT_EQ(BE(b, 32, 4), 1'502'500u);
    EXPECT_EQ(BE(b, 36, 8), 1234u);
    EXPECT_EQ(decode_itch(b).trade.match_number, 1234u);
}

TEST(Itch, TimestampKeepsOnlyLow48Bits) {
    ITCH f{};
    f.order_delete = ItchOrderDelete{};
    f.order_delete.header.timestamp = 0xFFFF'A1B2'C3D4'E5F6;

    EXPECT_EQ(decode_itch(Encode(f)).header.timestamp, 0xA1B2'C3D4'E5F6u);
}

TEST(Itch, RejectsWrongLengthAndUnknownType) {
    ITCH f{};
    f.order_delete = ItchOrderDelete{};
    const Bytes b = Encode(f);

    Bytes short_msg(b.begin(), b.end() - 1);
    EXPECT_EQ(decode_itch(short_msg).header.message_type, ItchMessageType::NONE);

    Bytes long_msg = b;
    Push(long_msg, {0x00});
    EXPECT_EQ(decode_itch(long_msg).header.message_type, ItchMessageType::NONE);

    Bytes unknown = b;
    unknown[0] = std::byte{'Z'};
    EXPECT_EQ(decode_itch(unknown).header.message_type, ItchMessageType::NONE);

    EXPECT_EQ(decode_itch({}).header.message_type, ItchMessageType::NONE);
    EXPECT_EQ(encoded_size(ITCH{}), 0u);
}
