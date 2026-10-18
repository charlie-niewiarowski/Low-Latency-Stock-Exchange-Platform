//
// Created by cniew on 5/16/26.
//

#ifndef OUCH_PROTO_H
#define OUCH_PROTO_H

#include "wire_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

//=============================================================================
// OUCH 5.0 (order entry, client <-> exchange, carried over TCPHeader)
//=============================================================================
// Field names/sizes/order follow Nasdaq's OUCH 5.0 spec, Sections 2/3 and
// Appendix A (Optional Appendage fields). The structs below are decoded,
// host-order views -- naturally aligned and padded like any C++ struct --
// NOT wire images. The wire format is produced and consumed exclusively by
// encode_*/decode_* at the bottom of this file, which write every message
// exactly as the spec lays it out: big-endian numerics, raw alpha bytes, no
// padding, core fields at their documented offsets, then a 2-byte
// Appendage Length and a list of TagValue elements (1-byte remaining
// length, 1-byte OptionTag, value).
//
// Appendage fields are ordinary struct members defaulted to their spec
// "tag absent" value, plus an OuchTagSet recording which tags were
// actually on the wire (decode) or should be put on it (encode). Presence
// is tracked explicitly rather than inferred from "differs from the
// default" because the two mean different things on a Replace: an absent
// tag there inherits the original order's value, it doesn't reset it.
// Tags a message doesn't support are skipped on decode, same as an
// unrecognized tag.
//
// Inbound (client -> exchange) and outbound (exchange -> client) messages
// are two separate unions (OuchInboundFrame/OuchOutboundFrame), each with
// its own discriminant enum -- matching the real protocol, where the two directions
// are separate streams that happen to reuse type letters (e.g. 'U' for both
// Replace Order Request and Order Replaced) without ever colliding.
//
// Every message struct still leads with `message_type`, giving each union a
// common initial sequence: reading `.message_type` is well-defined no matter
// which member was last written.
//
// Order identity on the wire is UserRefNum (a day-unique, strictly
// increasing 4-byte integer per OUCH port/account), not ClOrdID -- ClOrdID
// is a customer-chosen 14-char label that is explicitly *not* required to be
// unique. Client identity itself isn't carried in any OUCH message at all;
// it comes from which session the message arrived on.

using OuchClOrdId      = std::array<char, 14>; // customer order identifier; NOT required unique
using OuchStockSymbol  = std::array<char, 8>;  // alphanumeric, left-justified, space-padded

// ----- Optional Appendage / TagValue field types (Appendix A) -----
using OuchFirm         = std::array<char, 4>;  // MPID, space-padded
using OuchLocateBroker = std::array<char, 4>;
using OuchRoute        = std::array<char, 4>;
using OuchAiqGroupId   = std::array<char, 2>;

// Prices are unsigned, 4 implied decimal places. Anything at or above
// $200,000.0000 -- which covers both the documented cross-market sentinel
// $214,748.3647 (0x7FFFFFFF) and "the max integer value" -- is a market
// order (OUCH 5.0, Data Types).
inline constexpr uint64_t kOuchMaxLimitPrice    = 1'999'999'900; // $199,999.9900
inline constexpr uint64_t kOuchMarketPriceFloor = 2'000'000'000; // $200,000.0000

// Order Rejected reason codes (Appendix C) this exchange produces.
namespace ouch_reject {
    inline constexpr uint16_t kInvalidDisplay     = 0x0003;
    inline constexpr uint16_t kInvalidMaxFloor    = 0x0004;
    inline constexpr uint16_t kInvalidPegType     = 0x0005;
    inline constexpr uint16_t kIsoNotAllowed      = 0x0008;
    inline constexpr uint16_t kInvalidSide        = 0x0009;
    inline constexpr uint16_t kProcessingError    = 0x000A;
    inline constexpr uint16_t kInvalidMinQuantity = 0x000D;
    inline constexpr uint16_t kOther              = 0x000F;
    inline constexpr uint16_t kInvalidQuantity    = 0x0013;
    inline constexpr uint16_t kInvalidCrossOrder  = 0x0014;
    inline constexpr uint16_t kInvalidPrice       = 0x001D;
} // namespace ouch_reject

// OptionTag values, Appendix A.
enum class OuchTag : uint8_t {
    SECONDARY_ORD_REF_NUM = 1,
    FIRM                  = 2,
    MIN_QTY               = 3,
    CUSTOMER_TYPE         = 4,
    MAX_FLOOR             = 5,
    PRICE_TYPE            = 6,
    PEG_OFFSET            = 7,
    DISCRETION_PRICE      = 9,
    DISCRETION_PRICE_TYPE = 10,
    DISCRETION_PEG_OFFSET = 11,
    POST_ONLY             = 12,
    RANDOM_RESERVES       = 13,
    ROUTE                 = 14,
    EXPIRE_TIME           = 15,
    TRADE_NOW             = 16,
    HANDLE_INST           = 17,
    BBO_WEIGHT_INDICATOR  = 18,
    DISPLAY_QUANTITY      = 22,
    DISPLAY_PRICE         = 23,
    GROUP_ID              = 24,
    SHARES_LOCATED        = 25,
    LOCATE_BROKER         = 26,
    SIDE                  = 27,
    USER_REF_IDX          = 28,
    AIQ_STRATEGY          = 29,
    AIQ_GROUP_ID          = 30,
};

// Which optional tags are present on a message. Every Appendix A tag is
// < 32, so one bit per tag fits.
class OuchTagSet {
public:
    [[nodiscard]] constexpr bool has(OuchTag tag) const noexcept { return (bits_ & bit(tag)) != 0; }
    constexpr void set(OuchTag tag) noexcept { bits_ |= bit(tag); }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

private:
    static constexpr uint32_t bit(OuchTag tag) noexcept { return 1u << static_cast<uint8_t>(tag); }
    uint32_t bits_ = 0;
};

// Each message struct below exposes the same two static hooks the generic
// codec in ouch_detail drives:
//   core(msg, io)  -- every fixed field, in wire order (see wire_codec.hpp)
//   tags(msg, fn)  -- fn(OuchTag, field&) for each optional field this
//                     message supports, in ascending tag order (which is
//                     also the order encode emits them in)
// Both are templated on the message's constness so the one field list
// serves encode (const) and decode (mutable) alike.

//=============================================================================
// Inbound (client -> exchange)
//=============================================================================

enum class InboundOuchMessageType : char {
    NONE          = 0,   // sentinel: not a real message (default-constructed / failed parse)
    ENTER_ORDER   = 'O',
    REPLACE_ORDER = 'U',
    CANCEL_ORDER  = 'X',
};

struct OuchEnterOrder {
    InboundOuchMessageType message_type = InboundOuchMessageType::ENTER_ORDER;

    // ----- core fields -----
    uint32_t        user_ref_num;                   // day-unique, strictly increasing per port
    char            side;                            // 'B' buy, 'S' sell, 'T'/'E' sell short (exempt)
    uint32_t        quantity;                         // > 0 and < 1,000,000
    OuchStockSymbol symbol;
    uint64_t        price;                            // implied 4 decimal places
    char            time_in_force;                    // '0' Day,'3' IOC,'5' GTX,'6' GTT,'E' after hours
    char            display;                          // 'Y' visible, 'N' hidden, 'A' attributable
    char            capacity;                         // 'A' agency, 'P' principal, 'R' riskless, 'O' other
    char            intermarket_sweep_eligibility;    // 'Y' or 'N'
    char            cross_type;                       // 'N'/'O'/'C'/'H'/'S'/'R'/'E'/'A'
    OuchClOrdId     cl_ord_id;

    // ----- Optional Appendage fields (Appendix A), defaulted per spec -----
    OuchFirm         firm{' ', ' ', ' ', ' '};   // default: blank (use port's default firm)
    uint32_t         min_qty = 0;                 // default: no minimum
    char             customer_type = ' ';          // 'R' retail, 'N' not retail; default: port default
    uint32_t         max_floor = 0;                // default: no max floor
    char             price_type = 'L';              // 'L'/'P'/'M'/'R'/'Q'/'m'; default: limit
    int32_t          peg_offset = 0;                // default: no peg offset
    uint64_t         discretion_price = 0;           // default: no discretion price
    char             discretion_price_type = 'L';
    int32_t          discretion_peg_offset = 0;
    char             post_only = 'N';                // 'P' post only, 'N' no
    uint32_t         random_reserves = 0;
    uint32_t         expire_time = 0;                 // seconds to live; 0 = no expire time
    char             trade_now = 'N';
    char             handle_inst = ' ';
    uint16_t         group_id = 0;                     // customer group id; 0 = none
    char             shares_located = 'N';
    OuchLocateBroker locate_broker{};
    uint8_t          user_ref_idx = 0;                  // order-flow channel; 0 = none
    char             aiq_strategy = '*';                 // default: use port default
    OuchAiqGroupId   aiq_group_id{'*', '*'};

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.user_ref_num); io(m.side); io(m.quantity); io(m.symbol);
        io(m.price); io(m.time_in_force); io(m.display); io(m.capacity);
        io(m.intermarket_sweep_eligibility); io(m.cross_type); io(m.cl_ord_id);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::FIRM, m.firm);
        fn(OuchTag::MIN_QTY, m.min_qty);
        fn(OuchTag::CUSTOMER_TYPE, m.customer_type);
        fn(OuchTag::MAX_FLOOR, m.max_floor);
        fn(OuchTag::PRICE_TYPE, m.price_type);
        fn(OuchTag::PEG_OFFSET, m.peg_offset);
        fn(OuchTag::DISCRETION_PRICE, m.discretion_price);
        fn(OuchTag::DISCRETION_PRICE_TYPE, m.discretion_price_type);
        fn(OuchTag::DISCRETION_PEG_OFFSET, m.discretion_peg_offset);
        fn(OuchTag::POST_ONLY, m.post_only);
        fn(OuchTag::RANDOM_RESERVES, m.random_reserves);
        fn(OuchTag::EXPIRE_TIME, m.expire_time);
        fn(OuchTag::TRADE_NOW, m.trade_now);
        fn(OuchTag::HANDLE_INST, m.handle_inst);
        fn(OuchTag::GROUP_ID, m.group_id);
        fn(OuchTag::SHARES_LOCATED, m.shares_located);
        fn(OuchTag::LOCATE_BROKER, m.locate_broker);
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
        fn(OuchTag::AIQ_STRATEGY, m.aiq_strategy);
        fn(OuchTag::AIQ_GROUP_ID, m.aiq_group_id);
    }
};

struct OuchReplaceOrder {
    InboundOuchMessageType message_type = InboundOuchMessageType::REPLACE_ORDER;

    // ----- core fields -----
    uint32_t        orig_user_ref_num;   // UserRefNum of the existing order
    uint32_t        user_ref_num;        // new UserRefNum for the replacement
    uint32_t        quantity;            // total shares liable, inclusive of prior executions
    uint64_t        price;
    char            time_in_force;
    char            display;
    char            intermarket_sweep_eligibility;
    OuchClOrdId     cl_ord_id;

    // ----- Optional Appendage fields -- everything Enter Order supports
    // except Firm/GroupID (inherited from the original order), plus Side.
    // An absent tag inherits the original order's value (see the header
    // comment), so the defaults here only matter if the tag is present. -----
    uint32_t         min_qty = 0;
    uint32_t         max_floor = 0;
    char             price_type = 'L';
    int32_t          peg_offset = 0;
    uint64_t         discretion_price = 0;
    char             discretion_price_type = 'L';
    int32_t          discretion_peg_offset = 0;
    char             post_only = 'N';
    uint32_t         random_reserves = 0;
    uint32_t         expire_time = 0;
    char             trade_now = 'N';
    char             handle_inst = ' ';
    char             shares_located = 'N';
    OuchLocateBroker locate_broker{};
    char             side = ' ';
    uint8_t          user_ref_idx = 0;
    char             aiq_strategy = '*';
    OuchAiqGroupId   aiq_group_id{'*', '*'};

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.orig_user_ref_num); io(m.user_ref_num); io(m.quantity);
        io(m.price); io(m.time_in_force); io(m.display); io(m.intermarket_sweep_eligibility);
        io(m.cl_ord_id);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::MIN_QTY, m.min_qty);
        fn(OuchTag::MAX_FLOOR, m.max_floor);
        fn(OuchTag::PRICE_TYPE, m.price_type);
        fn(OuchTag::PEG_OFFSET, m.peg_offset);
        fn(OuchTag::DISCRETION_PRICE, m.discretion_price);
        fn(OuchTag::DISCRETION_PRICE_TYPE, m.discretion_price_type);
        fn(OuchTag::DISCRETION_PEG_OFFSET, m.discretion_peg_offset);
        fn(OuchTag::POST_ONLY, m.post_only);
        fn(OuchTag::RANDOM_RESERVES, m.random_reserves);
        fn(OuchTag::EXPIRE_TIME, m.expire_time);
        fn(OuchTag::TRADE_NOW, m.trade_now);
        fn(OuchTag::HANDLE_INST, m.handle_inst);
        fn(OuchTag::SHARES_LOCATED, m.shares_located);
        fn(OuchTag::LOCATE_BROKER, m.locate_broker);
        fn(OuchTag::SIDE, m.side);
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
        fn(OuchTag::AIQ_STRATEGY, m.aiq_strategy);
        fn(OuchTag::AIQ_GROUP_ID, m.aiq_group_id);
    }
};

struct OuchCancelOrder {
    InboundOuchMessageType message_type = InboundOuchMessageType::CANCEL_ORDER;

    // ----- core fields -----
    uint32_t user_ref_num;   // the order being canceled/reduced
    uint32_t quantity;       // new intended order size; 0 = full cancel

    // ----- Optional Appendage fields -----
    uint8_t user_ref_idx = 0;

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.user_ref_num); io(m.quantity);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
    }
};

// Inbound (client -> exchange) OUCH frame is one message type wide; the
// caller reads `.message_type` first, then reads through the matching member.
union OuchInboundFrame {
    InboundOuchMessageType message_type;
    OuchEnterOrder          enter_order;
    OuchReplaceOrder        replace_order;
    OuchCancelOrder         cancel_order;

    OuchInboundFrame() : message_type(InboundOuchMessageType::NONE) {}
};

//=============================================================================
// Outbound (exchange -> client)
//=============================================================================

enum class OutboundOuchMessageType : char {
    NONE           = 0,
    ORDER_ACCEPTED = 'A',
    ORDER_REPLACED = 'U',   // separate enum from InboundOuchMessageType, so this can match
                             // the real spec's type byte -- see the section header comment.
    ORDER_CANCELED = 'C',
    ORDER_EXECUTED = 'E',
    ORDER_REJECTED = 'J',
};

struct OuchOrderAccepted {
    OutboundOuchMessageType message_type = OutboundOuchMessageType::ORDER_ACCEPTED;

    // ----- core fields -----
    uint64_t        timestamp;      // nanoseconds since midnight
    uint32_t        user_ref_num;
    char            side;
    uint32_t        quantity;
    OuchStockSymbol symbol;
    uint64_t        price;
    char            time_in_force;
    char            display;
    uint64_t        order_reference_number;   // exchange-assigned, day-unique
    char            capacity;
    char            intermarket_sweep_eligibility;
    char            cross_type;
    char            order_state;              // 'L' live, 'D' dead
    OuchClOrdId     cl_ord_id;

    // ----- Optional Appendage fields -----
    OuchFirm       firm{' ', ' ', ' ', ' '};
    uint32_t       min_qty = 0;
    char           customer_type = ' ';
    uint32_t       max_floor = 0;
    char           price_type = 'L';
    int32_t        peg_offset = 0;
    uint64_t       discretion_price = 0;
    char           discretion_price_type = 'L';
    int32_t        discretion_peg_offset = 0;
    char           post_only = 'N';
    uint32_t       random_reserves = 0;
    OuchRoute      route{};
    uint32_t       expire_time = 0;
    char           trade_now = 'N';
    char           handle_inst = ' ';
    char           bbo_weight_indicator = ' ';
    uint16_t       group_id = 0;
    char           shares_located = 'N';
    uint8_t        user_ref_idx = 0;
    char           aiq_strategy = '*';
    OuchAiqGroupId aiq_group_id{'*', '*'};

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.timestamp); io(m.user_ref_num); io(m.side); io(m.quantity);
        io(m.symbol); io(m.price); io(m.time_in_force); io(m.display);
        io(m.order_reference_number); io(m.capacity); io(m.intermarket_sweep_eligibility);
        io(m.cross_type); io(m.order_state); io(m.cl_ord_id);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::FIRM, m.firm);
        fn(OuchTag::MIN_QTY, m.min_qty);
        fn(OuchTag::CUSTOMER_TYPE, m.customer_type);
        fn(OuchTag::MAX_FLOOR, m.max_floor);
        fn(OuchTag::PRICE_TYPE, m.price_type);
        fn(OuchTag::PEG_OFFSET, m.peg_offset);
        fn(OuchTag::DISCRETION_PRICE, m.discretion_price);
        fn(OuchTag::DISCRETION_PRICE_TYPE, m.discretion_price_type);
        fn(OuchTag::DISCRETION_PEG_OFFSET, m.discretion_peg_offset);
        fn(OuchTag::POST_ONLY, m.post_only);
        fn(OuchTag::RANDOM_RESERVES, m.random_reserves);
        fn(OuchTag::ROUTE, m.route);
        fn(OuchTag::EXPIRE_TIME, m.expire_time);
        fn(OuchTag::TRADE_NOW, m.trade_now);
        fn(OuchTag::HANDLE_INST, m.handle_inst);
        fn(OuchTag::BBO_WEIGHT_INDICATOR, m.bbo_weight_indicator);
        fn(OuchTag::GROUP_ID, m.group_id);
        fn(OuchTag::SHARES_LOCATED, m.shares_located);
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
        fn(OuchTag::AIQ_STRATEGY, m.aiq_strategy);
        fn(OuchTag::AIQ_GROUP_ID, m.aiq_group_id);
    }
};

struct OuchOrderReplaced {
    OutboundOuchMessageType message_type = OutboundOuchMessageType::ORDER_REPLACED;

    // ----- core fields -----
    uint64_t        timestamp;
    uint32_t        orig_user_ref_num;   // UserRefNum of the order being replaced
    uint32_t        user_ref_num;        // UserRefNum of the replacement order
    char            side;
    uint32_t        quantity;
    OuchStockSymbol symbol;
    uint64_t        price;
    char            time_in_force;
    char            display;
    uint64_t        order_reference_number;
    char            capacity;
    char            intermarket_sweep_eligibility;
    char            cross_type;
    char            order_state;
    OuchClOrdId     cl_ord_id;

    // ----- Optional Appendage fields -----
    OuchFirm       firm{' ', ' ', ' ', ' '};
    uint32_t       min_qty = 0;
    uint32_t       max_floor = 0;
    char           price_type = 'L';
    char           post_only = 'N';
    uint32_t       expire_time = 0;
    char           trade_now = 'N';
    char           handle_inst = ' ';
    char           bbo_weight_indicator = ' ';
    uint8_t        user_ref_idx = 0;
    char           aiq_strategy = '*';
    OuchAiqGroupId aiq_group_id{'*', '*'};

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.timestamp); io(m.orig_user_ref_num); io(m.user_ref_num);
        io(m.side); io(m.quantity); io(m.symbol); io(m.price); io(m.time_in_force);
        io(m.display); io(m.order_reference_number); io(m.capacity);
        io(m.intermarket_sweep_eligibility); io(m.cross_type); io(m.order_state);
        io(m.cl_ord_id);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::FIRM, m.firm);
        fn(OuchTag::MIN_QTY, m.min_qty);
        fn(OuchTag::MAX_FLOOR, m.max_floor);
        fn(OuchTag::PRICE_TYPE, m.price_type);
        fn(OuchTag::POST_ONLY, m.post_only);
        fn(OuchTag::EXPIRE_TIME, m.expire_time);
        fn(OuchTag::TRADE_NOW, m.trade_now);
        fn(OuchTag::HANDLE_INST, m.handle_inst);
        fn(OuchTag::BBO_WEIGHT_INDICATOR, m.bbo_weight_indicator);
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
        fn(OuchTag::AIQ_STRATEGY, m.aiq_strategy);
        fn(OuchTag::AIQ_GROUP_ID, m.aiq_group_id);
    }
};

struct OuchOrderCanceled {
    OutboundOuchMessageType message_type = OutboundOuchMessageType::ORDER_CANCELED;

    // ----- core fields -----
    uint64_t timestamp;
    uint32_t user_ref_num;
    uint32_t decrement_shares;   // incremental, not cumulative
    char     reason;             // see Appendix B (Order Cancel Reasons)

    // ----- Optional Appendage fields -----
    uint8_t user_ref_idx = 0;

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.timestamp); io(m.user_ref_num); io(m.decrement_shares);
        io(m.reason);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
    }
};

struct OuchOrderExecuted {
    OutboundOuchMessageType message_type = OutboundOuchMessageType::ORDER_EXECUTED;

    // ----- core fields -----
    uint64_t timestamp;
    uint32_t user_ref_num;
    uint32_t executed_shares;    // incremental, not cumulative
    uint64_t execution_price;
    char     liquidity_flag;     // see Appendix E (Liquidity Flags)
    uint64_t match_number;       // shared by both sides of a trade

    // ----- Optional Appendage fields -----
    uint8_t user_ref_idx = 0;

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.timestamp); io(m.user_ref_num); io(m.executed_shares);
        io(m.execution_price); io(m.liquidity_flag); io(m.match_number);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
    }
};

struct OuchOrderRejected {
    OutboundOuchMessageType message_type = OutboundOuchMessageType::ORDER_REJECTED;

    // ----- core fields -----
    uint64_t    timestamp;
    uint32_t    user_ref_num;
    uint16_t    reason;      // reject reason code, see Appendix D (numeric, not alpha)
    OuchClOrdId cl_ord_id;

    // ----- Optional Appendage fields -----
    uint8_t user_ref_idx = 0;

    OuchTagSet appendage{};

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        io(m.message_type); io(m.timestamp); io(m.user_ref_num); io(m.reason); io(m.cl_ord_id);
    }

    template <typename Self, typename F>
    static void tags(Self &m, F &&fn) {
        fn(OuchTag::USER_REF_IDX, m.user_ref_idx);
    }
};

// Outbound (exchange -> client) OUCH frame; same one-message-type-wide,
// tag-first shape as OuchInboundFrame above.
union OuchOutboundFrame {
    OutboundOuchMessageType message_type;
    OuchOrderAccepted        order_accepted;
    OuchOrderReplaced        order_replaced;
    OuchOrderCanceled        order_canceled;
    OuchOrderExecuted        order_executed;
    OuchOrderRejected        order_rejected;

    OuchOutboundFrame() : message_type(OutboundOuchMessageType::NONE) {}
};

//=============================================================================
// Wire codec
//=============================================================================

namespace ouch_detail {

inline constexpr size_t kAppendageLengthLen = 2;
inline constexpr size_t kTagValueHeaderLen  = 2; // Length + OptionTag

template <typename M>
size_t encoded_size(const M &msg) noexcept {
    wire::WireSizer sizer;
    M::core(msg, sizer);
    size_t size = sizer.size() + kAppendageLengthLen;
    M::tags(msg, [&](OuchTag tag, const auto &field) {
        if (msg.appendage.has(tag)) size += kTagValueHeaderLen + sizeof(field);
    });
    return size;
}

template <typename M>
size_t encode(const M &msg, std::span<std::byte> out) noexcept {
    wire::WireWriter writer{out};
    M::core(msg, writer);

    std::byte *appendage_length = writer.skip(kAppendageLengthLen);
    const size_t appendage_begin = writer.written();
    M::tags(msg, [&](OuchTag tag, const auto &field) {
        if (!msg.appendage.has(tag)) return;
        // TagValue Length counts what follows it: the OptionTag byte plus the value.
        writer(static_cast<uint8_t>(1 + sizeof(field)));
        writer(tag);
        writer(field);
    });

    if (!writer.ok()) return 0;
    wire::WireWriter::store(appendage_length, static_cast<uint16_t>(writer.written() - appendage_begin));
    return writer.written();
}

// Strict about structure, lenient about content: a short core, an
// Appendage Length that doesn't match the bytes that follow it, a TagValue
// running past the end, or a known tag with the wrong value width all fail
// the decode; an unrecognized (or unsupported-on-this-message) tag is
// skipped. Appendage Length itself may be omitted entirely (the message
// ends right after the core fields) -- the spec calls it optional on
// Replace/Cancel and treating it that way everywhere costs nothing.
template <typename M>
bool decode(std::span<const std::byte> bytes, M &msg) noexcept {
    wire::WireReader reader{bytes};
    M::core(msg, reader);
    if (!reader.ok()) return false;
    if (reader.remaining() == 0) return true;

    uint16_t appendage_length = 0;
    reader(appendage_length);
    if (!reader.ok() || reader.remaining() != appendage_length) return false;

    while (reader.remaining() > 0) {
        uint8_t length = 0;
        reader(length);
        if (length == 0 || length > reader.remaining()) return false;

        uint8_t raw_tag = 0;
        reader(raw_tag);
        const std::span<const std::byte> value = reader.take(length - 1);

        bool malformed = false;
        M::tags(msg, [&](OuchTag tag, auto &field) {
            if (static_cast<uint8_t>(tag) != raw_tag) return;
            if (value.size() != sizeof(field)) {
                malformed = true;
                return;
            }
            wire::WireReader value_reader{value};
            value_reader(field);
            msg.appendage.set(tag);
        });
        if (malformed) return false;
    }
    return true;
}

} // namespace ouch_detail

// Exact number of bytes encode_*_ouch will write for msg; 0 for NONE.
inline size_t encoded_size(const OuchInboundFrame &msg) noexcept {
    switch (msg.message_type) {
        case InboundOuchMessageType::NONE:          return 0;
        case InboundOuchMessageType::ENTER_ORDER:   return ouch_detail::encoded_size(msg.enter_order);
        case InboundOuchMessageType::REPLACE_ORDER: return ouch_detail::encoded_size(msg.replace_order);
        case InboundOuchMessageType::CANCEL_ORDER:  return ouch_detail::encoded_size(msg.cancel_order);
    }
    return 0; // defensive: only reachable via an out-of-range enum value
}

inline size_t encoded_size(const OuchOutboundFrame &msg) noexcept {
    switch (msg.message_type) {
        case OutboundOuchMessageType::NONE:           return 0;
        case OutboundOuchMessageType::ORDER_ACCEPTED: return ouch_detail::encoded_size(msg.order_accepted);
        case OutboundOuchMessageType::ORDER_REPLACED: return ouch_detail::encoded_size(msg.order_replaced);
        case OutboundOuchMessageType::ORDER_CANCELED: return ouch_detail::encoded_size(msg.order_canceled);
        case OutboundOuchMessageType::ORDER_EXECUTED: return ouch_detail::encoded_size(msg.order_executed);
        case OutboundOuchMessageType::ORDER_REJECTED: return ouch_detail::encoded_size(msg.order_rejected);
    }
    return 0; // defensive: only reachable via an out-of-range enum value
}

// Writes msg's spec wire bytes into out. Returns the number of bytes
// written, or 0 if msg is NONE or out is too small (see encoded_size).
inline size_t encode_inbound_ouch(const OuchInboundFrame &msg, std::span<std::byte> out) noexcept {
    switch (msg.message_type) {
        case InboundOuchMessageType::NONE:          return 0;
        case InboundOuchMessageType::ENTER_ORDER:   return ouch_detail::encode(msg.enter_order, out);
        case InboundOuchMessageType::REPLACE_ORDER: return ouch_detail::encode(msg.replace_order, out);
        case InboundOuchMessageType::CANCEL_ORDER:  return ouch_detail::encode(msg.cancel_order, out);
    }
    return 0; // defensive: only reachable via an out-of-range enum value
}

inline size_t encode_outbound_ouch(const OuchOutboundFrame &msg, std::span<std::byte> out) noexcept {
    switch (msg.message_type) {
        case OutboundOuchMessageType::NONE:           return 0;
        case OutboundOuchMessageType::ORDER_ACCEPTED: return ouch_detail::encode(msg.order_accepted, out);
        case OutboundOuchMessageType::ORDER_REPLACED: return ouch_detail::encode(msg.order_replaced, out);
        case OutboundOuchMessageType::ORDER_CANCELED: return ouch_detail::encode(msg.order_canceled, out);
        case OutboundOuchMessageType::ORDER_EXECUTED: return ouch_detail::encode(msg.order_executed, out);
        case OutboundOuchMessageType::ORDER_REJECTED: return ouch_detail::encode(msg.order_rejected, out);
    }
    return 0; // defensive: only reachable via an out-of-range enum value
}

// Decodes exactly one message spanning all of bytes (the session layer
// is what frames messages; see ouch_detail::decode for what's rejected).
// Returns a NONE frame on an empty buffer, an unrecognized type byte, or
// a malformed message.
inline OuchInboundFrame decode_inbound_ouch(std::span<const std::byte> bytes) noexcept {
    OuchInboundFrame msg{};
    if (bytes.empty()) return msg;

    bool ok = false;
    switch (static_cast<InboundOuchMessageType>(bytes[0])) {
        case InboundOuchMessageType::ENTER_ORDER:
            msg.enter_order = OuchEnterOrder{};
            ok = ouch_detail::decode(bytes, msg.enter_order);
            break;
        case InboundOuchMessageType::REPLACE_ORDER:
            msg.replace_order = OuchReplaceOrder{};
            ok = ouch_detail::decode(bytes, msg.replace_order);
            break;
        case InboundOuchMessageType::CANCEL_ORDER:
            msg.cancel_order = OuchCancelOrder{};
            ok = ouch_detail::decode(bytes, msg.cancel_order);
            break;
        default:
            break;
    }
    return ok ? msg : OuchInboundFrame{};
}

inline OuchOutboundFrame decode_outbound_ouch(std::span<const std::byte> bytes) noexcept {
    OuchOutboundFrame msg{};
    if (bytes.empty()) return msg;

    bool ok = false;
    switch (static_cast<OutboundOuchMessageType>(bytes[0])) {
        case OutboundOuchMessageType::ORDER_ACCEPTED:
            msg.order_accepted = OuchOrderAccepted{};
            ok = ouch_detail::decode(bytes, msg.order_accepted);
            break;
        case OutboundOuchMessageType::ORDER_REPLACED:
            msg.order_replaced = OuchOrderReplaced{};
            ok = ouch_detail::decode(bytes, msg.order_replaced);
            break;
        case OutboundOuchMessageType::ORDER_CANCELED:
            msg.order_canceled = OuchOrderCanceled{};
            ok = ouch_detail::decode(bytes, msg.order_canceled);
            break;
        case OutboundOuchMessageType::ORDER_EXECUTED:
            msg.order_executed = OuchOrderExecuted{};
            ok = ouch_detail::decode(bytes, msg.order_executed);
            break;
        case OutboundOuchMessageType::ORDER_REJECTED:
            msg.order_rejected = OuchOrderRejected{};
            ok = ouch_detail::decode(bytes, msg.order_rejected);
            break;
        default:
            break;
    }
    return ok ? msg : OuchOutboundFrame{};
}

#endif //OUCH_PROTO_H
