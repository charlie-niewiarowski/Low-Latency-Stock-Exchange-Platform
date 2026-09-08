//
// Created by cniew on 5/16/26.
//

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "communication_types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

//=============================================================================
// Wire frame sizes
//=============================================================================

// Inbound frame: "EXCHANGE\n"(9) + InboundMessage(56) + '\n'(1) = 66 bytes + 6 bytes padding = 72 bytes.
inline constexpr size_t INBOUND_BSIZE = 72;

// Outbound frame: fixed 64 bytes (cache-line aligned).
//   OK    -> "EXCHANGE\nOK\n"(12) + ClientId(4) + OrderId(8)          = 24 bytes
//   ERROR -> "EXCHANGE\nERROR\n"(15) + longest_error_str(17) + '\n'(1) = 33 bytes
// Both fit comfortably; the remaining bytes are zero-padded by the sender.
inline constexpr size_t OUTBOUND_BSIZE = 32;

//=============================================================================
// Status prefixes (server -> client)
//=============================================================================

#define OK_STATUS    "EXCHANGE\nOK\n"       // 12 bytes; ClientId @ 12, OrderId @ 16
#define MATCH_STATUS "EXCHANGE\nMATCH\n"   // 15 bytes; ClientId @ 15, OrderId @ 19
#define ERROR_STATUS "EXCHANGE\nERROR\n"

//=============================================================================
// Error string helpers
//=============================================================================

constexpr std::string_view server_error_string(const ServerError error) {
    switch (error) {
        case ServerError::MALFORMED_REQUEST: return "malformed req";
        case ServerError::INVALID_ORDER:     return "invalid order";
        case ServerError::SYSTEM_ERROR:      return "system error";
        case ServerError::EXECUTION_ERROR:   return "execution error";
        default:                             return "null";
    }
}

//=============================================================================
// Transport protocols (market_feed / packet_factory)
//=============================================================================
// Minimal transport-layer headers packet_factory writes onto outgoing DPDK
// packets. OUCH rides on TCP (order entry); ITCH rides on UDP (market data,
// multicast).

struct TCP {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq    = 0;
    uint32_t ack    = 0;
    uint8_t  flags  = 0;      // bitwise-OR of the sender's TCP_*_FLAG bits
    uint16_t window = 65535;
};

struct UDP {
    uint16_t src_port;
    uint16_t dst_port;
};

//=============================================================================
// OUCH 5.0 (order entry, client <-> exchange, carried over TCP)
//=============================================================================
// Field names/sizes/order follow Nasdaq's OUCH 5.0 spec. Every message struct
// leads with `message_type` at the same offset (byte 0) as every other one,
// giving OUCH's member union a common initial sequence: reading
// `ouch.message_type` is well-defined no matter which member was last
// written, so a receiver can inspect it before knowing which variant is live.
// Multi-byte integers here are host-endian for this simulated feed; a real
// wire wouldn't be, so packet_factory's (de)serialization is where any
// endianness conversion belongs — not in these struct layouts.

enum class OuchMessageType : char {
    NONE           = 0,   // sentinel: not a real message (default-constructed / failed parse)
    // client -> exchange
    ENTER_ORDER    = 'O',
    REPLACE_ORDER  = 'U',
    CANCEL_ORDER   = 'X',
    // exchange -> client
    ORDER_ACCEPTED = 'A',
    ORDER_REPLACED = 'R',
    ORDER_CANCELED = 'C',
    ORDER_EXECUTED = 'E',
    ORDER_REJECTED = 'J',
};

using OuchOrderToken = std::array<char, 14>; // client-assigned, alphanumeric, space-padded
using OuchStockSymbol = std::array<char, 8>; // alphanumeric, left-justified, space-padded
using OuchFirm = std::array<char, 4>;        // MPID, alphanumeric, space-padded

// ----- client -> exchange -----

struct OuchEnterOrder {
    OuchMessageType message_type = OuchMessageType::ENTER_ORDER;
    OuchOrderToken  order_token;
    char            buy_sell_indicator;             // 'B' or 'S'
    uint32_t        shares;
    OuchStockSymbol stock;
    uint32_t        price;                          // 4 implied decimal digits
    uint32_t        time_in_force;                   // seconds; 0 = Day, 99999 = IOC
    OuchFirm        firm;
    char            display;                         // 'Y' or 'N'
    char            capacity;                         // 'O'/'P'/'A'/'R'
    char            intermarket_sweep_eligibility;    // 'Y' or 'N'
    uint32_t        minimum_quantity;
    char            cross_type;                       // 'N'/'O'/'C'/'H'/'I'
    char            customer_type;                     // 'R' retail, 'N' non-retail
};

struct OuchReplaceOrder {
    OuchMessageType message_type = OuchMessageType::REPLACE_ORDER;
    OuchOrderToken  existing_order_token;
    OuchOrderToken  replacement_order_token;
    uint32_t        shares;
    uint32_t        price;
    uint32_t        time_in_force;
    char            display;
    char            intermarket_sweep_eligibility;
    uint32_t        minimum_quantity;
};

struct OuchCancelOrder {
    OuchMessageType message_type = OuchMessageType::CANCEL_ORDER;
    OuchOrderToken  order_token;
    uint32_t        shares;    // requested size to cancel down to; 0 = full cancel
};

// ----- exchange -> client -----

struct OuchOrderAccepted {
    OuchMessageType message_type = OuchMessageType::ORDER_ACCEPTED;
    OuchOrderToken  order_token;
    char            buy_sell_indicator;
    uint32_t        shares;
    OuchStockSymbol stock;
    uint32_t        price;
    uint32_t        time_in_force;
    OuchFirm        firm;
    char            display;
    uint64_t        order_reference_number;   // exchange-assigned
    char            capacity;
    char            intermarket_sweep_eligibility;
    uint32_t        minimum_quantity;
    char            cross_type;
    char            order_state;               // 'L' live, 'D' dead
};

struct OuchOrderReplaced {
    OuchMessageType message_type = OuchMessageType::ORDER_REPLACED;
    OuchOrderToken  replacement_order_token;
    char            buy_sell_indicator;
    uint32_t        shares;
    OuchStockSymbol stock;
    uint32_t        price;
    uint32_t        time_in_force;
    OuchFirm        firm;
    char            display;
    uint64_t        order_reference_number;
    char            capacity;
    char            intermarket_sweep_eligibility;
    uint32_t        minimum_quantity;
    char            cross_type;
    char            order_state;
    OuchOrderToken  previous_order_token;
};

struct OuchOrderCanceled {
    OuchMessageType message_type = OuchMessageType::ORDER_CANCELED;
    OuchOrderToken  order_token;
    uint32_t        decrement_shares;
    char            reason;    // 'U' user requested, 'I' IOC, 'T' timeout, 'S' supervisory, ...
};

struct OuchOrderExecuted {
    OuchMessageType message_type = OuchMessageType::ORDER_EXECUTED;
    OuchOrderToken  order_token;
    uint32_t        executed_shares;
    uint32_t        execution_price;
    char            liquidity_flag;   // 'A' added liquidity, 'R' removed liquidity
    uint64_t        match_number;
};

struct OuchOrderRejected {
    OuchMessageType message_type = OuchMessageType::ORDER_REJECTED;
    OuchOrderToken  order_token;
    char            reason;   // e.g. 'L' restricted stock, 'M' market not open, 'Z' test order
};

// A raw OUCH frame is one message type wide; the caller reads
// `.message_type` first, then reinterprets through the matching member.
union OUCH {
    OuchMessageType    message_type;
    OuchEnterOrder     enter_order;
    OuchReplaceOrder   replace_order;
    OuchCancelOrder    cancel_order;
    OuchOrderAccepted  order_accepted;
    OuchOrderReplaced  order_replaced;
    OuchOrderCanceled  order_canceled;
    OuchOrderExecuted  order_executed;
    OuchOrderRejected  order_rejected;

    OUCH() : message_type(OuchMessageType::NONE) {}
};

// Returns a view over the wire bytes of whichever OUCH member is active,
// sized to exactly that member (not sizeof(OUCH)) -- e.g. an ENTER_ORDER
// message serializes to sizeof(OuchEnterOrder) bytes, not the full union.
inline std::span<const std::byte> serialize_ouch(const OUCH &msg) noexcept {
    const auto *base = reinterpret_cast<const std::byte *>(&msg);
    switch (msg.message_type) {
        case OuchMessageType::NONE:            return {base, 0};
        case OuchMessageType::ENTER_ORDER:    return {base, sizeof(OuchEnterOrder)};
        case OuchMessageType::REPLACE_ORDER:  return {base, sizeof(OuchReplaceOrder)};
        case OuchMessageType::CANCEL_ORDER:   return {base, sizeof(OuchCancelOrder)};
        case OuchMessageType::ORDER_ACCEPTED: return {base, sizeof(OuchOrderAccepted)};
        case OuchMessageType::ORDER_REPLACED: return {base, sizeof(OuchOrderReplaced)};
        case OuchMessageType::ORDER_CANCELED: return {base, sizeof(OuchOrderCanceled)};
        case OuchMessageType::ORDER_EXECUTED: return {base, sizeof(OuchOrderExecuted)};
        case OuchMessageType::ORDER_REJECTED: return {base, sizeof(OuchOrderRejected)};
    }
    return {base, 0}; // defensive: only reachable via an out-of-range enum value
}

// Reconstructs an OUCH from raw wire bytes. bytes[0] is always
// message_type (the common initial sequence every member shares), which is
// enough to know how many of the remaining bytes belong to this message and
// how to interpret them. Returns a zeroed OUCH (message_type == 0, matching
// no real variant) on a too-short buffer or an unrecognized type byte.
inline OUCH deserialize_ouch(std::span<const std::byte> bytes) noexcept {
    OUCH msg{};
    if (bytes.empty()) return msg;

    size_t n = 0;
    switch (static_cast<OuchMessageType>(bytes[0])) {
        case OuchMessageType::ENTER_ORDER:    n = sizeof(OuchEnterOrder);    break;
        case OuchMessageType::REPLACE_ORDER:  n = sizeof(OuchReplaceOrder);  break;
        case OuchMessageType::CANCEL_ORDER:   n = sizeof(OuchCancelOrder);   break;
        case OuchMessageType::ORDER_ACCEPTED: n = sizeof(OuchOrderAccepted); break;
        case OuchMessageType::ORDER_REPLACED: n = sizeof(OuchOrderReplaced); break;
        case OuchMessageType::ORDER_CANCELED: n = sizeof(OuchOrderCanceled); break;
        case OuchMessageType::ORDER_EXECUTED: n = sizeof(OuchOrderExecuted); break;
        case OuchMessageType::ORDER_REJECTED: n = sizeof(OuchOrderRejected); break;
        default: return msg;
    }
    std::memcpy(&msg, bytes.data(), std::min(n, bytes.size()));
    return msg;
}

//=============================================================================
// ITCH 5.0 (market data, exchange -> subscribers, one-way over UDP multicast)
//=============================================================================
// Covers the per-order lifecycle messages this engine actually produces
// (add/execute/cancel/delete/replace/trade). Reference-data and market-wide
// administrative messages (System Event, Stock Directory, Stock Trading
// Action, MWCB, IPO Quoting Period, LULD, NOII, ...) are out of scope: this
// engine has no notion of trading sessions, halts, or multiple listed
// instruments' metadata to report.
//
// Every message leads with ItchHeader (message_type, stock_locate, tracking
// number, timestamp) at the same offset, again giving the union a common
// initial sequence.

using ItchStockSymbol = std::array<char, 8>;

enum class ItchMessageType : char {
    NONE                   = 0,   // sentinel: not a real message (default-constructed / failed parse)
    ADD_ORDER              = 'A',
    ORDER_EXECUTED         = 'E',
    ORDER_EXECUTED_W_PRICE = 'C',
    ORDER_CANCEL           = 'X',
    ORDER_DELETE           = 'D',
    ORDER_REPLACE          = 'U',
    TRADE                  = 'P',
};

struct ItchHeader {
    ItchMessageType      message_type;
    uint16_t              stock_locate;
    uint16_t              tracking_number;
    std::array<uint8_t, 6> timestamp;   // nanoseconds since midnight, 48-bit
};

struct ItchAddOrder {
    ItchHeader      header{.message_type = ItchMessageType::ADD_ORDER, .stock_locate = 0, .tracking_number = 0, .timestamp = {}};
    uint64_t        order_reference_number;
    char            buy_sell_indicator;
    uint32_t        shares;
    ItchStockSymbol stock;
    uint32_t        price;
};

struct ItchOrderExecuted {
    ItchHeader header{.message_type = ItchMessageType::ORDER_EXECUTED, .stock_locate = 0, .tracking_number = 0, .timestamp = {}};
    uint64_t   order_reference_number;
    uint32_t   executed_shares;
    uint64_t   match_number;
};

struct ItchOrderExecutedWithPrice {
    ItchHeader header{.message_type = ItchMessageType::ORDER_EXECUTED_W_PRICE, .stock_locate = 0, .tracking_number = 0, .timestamp = {}};
    uint64_t   order_reference_number;
    uint32_t   executed_shares;
    uint64_t   match_number;
    char       printable;   // 'Y' or 'N'
    uint32_t   execution_price;
};

struct ItchOrderCancel {
    ItchHeader header{.message_type = ItchMessageType::ORDER_CANCEL, .stock_locate = 0, .tracking_number = 0, .timestamp = {}};
    uint64_t   order_reference_number;
    uint32_t   canceled_shares;
};

struct ItchOrderDelete {
    ItchHeader header{.message_type = ItchMessageType::ORDER_DELETE, .stock_locate = 0, .tracking_number = 0, .timestamp = {}};
    uint64_t   order_reference_number;
};

struct ItchOrderReplace {
    ItchHeader header{.message_type = ItchMessageType::ORDER_REPLACE, .stock_locate = 0, .tracking_number = 0, .timestamp = {}};
    uint64_t   original_order_reference_number;
    uint64_t   new_order_reference_number;
    uint32_t   shares;
    uint32_t   price;
};

struct ItchTrade {
    ItchHeader      header{.message_type = ItchMessageType::TRADE, .stock_locate = 0, .tracking_number = 0, .timestamp = {}};
    uint64_t        order_reference_number;
    char            buy_sell_indicator;
    uint32_t        shares;
    ItchStockSymbol stock;
    uint32_t        price;
    uint64_t        match_number;
};

union ITCH {
    ItchHeader                  header;
    ItchAddOrder                add_order;
    ItchOrderExecuted           order_executed;
    ItchOrderExecutedWithPrice  order_executed_with_price;
    ItchOrderCancel             order_cancel;
    ItchOrderDelete             order_delete;
    ItchOrderReplace            order_replace;
    ItchTrade                   trade;

    ITCH() : header{.message_type = ItchMessageType::NONE, .stock_locate = 0,
                     .tracking_number = 0, .timestamp = {}} {}
};

// Returns a view over the wire bytes of whichever ITCH member is active,
// sized to exactly that member (not sizeof(ITCH)).
inline std::span<const std::byte> serialize_itch(const ITCH &msg) noexcept {
    const auto *base = reinterpret_cast<const std::byte *>(&msg);
    switch (msg.header.message_type) {
        case ItchMessageType::NONE:                   return {base, 0};
        case ItchMessageType::ADD_ORDER:              return {base, sizeof(ItchAddOrder)};
        case ItchMessageType::ORDER_EXECUTED:         return {base, sizeof(ItchOrderExecuted)};
        case ItchMessageType::ORDER_EXECUTED_W_PRICE: return {base, sizeof(ItchOrderExecutedWithPrice)};
        case ItchMessageType::ORDER_CANCEL:           return {base, sizeof(ItchOrderCancel)};
        case ItchMessageType::ORDER_DELETE:           return {base, sizeof(ItchOrderDelete)};
        case ItchMessageType::ORDER_REPLACE:          return {base, sizeof(ItchOrderReplace)};
        case ItchMessageType::TRADE:                  return {base, sizeof(ItchTrade)};
    }
    return {base, 0}; // defensive: only reachable via an out-of-range enum value
}

// Reconstructs an ITCH from raw wire bytes. bytes[0] is always message_type
// (the common initial sequence every member shares), which is enough to
// know how many of the remaining bytes belong to this message and how to
// interpret them. Returns a zeroed ITCH (header.message_type == NONE) on a
// too-short buffer or an unrecognized type byte.
inline ITCH deserialize_itch(std::span<const std::byte> bytes) noexcept {
    ITCH msg{};
    if (bytes.empty()) return msg;

    size_t n = 0;
    switch (static_cast<ItchMessageType>(bytes[0])) {
        case ItchMessageType::ADD_ORDER:              n = sizeof(ItchAddOrder);               break;
        case ItchMessageType::ORDER_EXECUTED:         n = sizeof(ItchOrderExecuted);          break;
        case ItchMessageType::ORDER_EXECUTED_W_PRICE: n = sizeof(ItchOrderExecutedWithPrice); break;
        case ItchMessageType::ORDER_CANCEL:           n = sizeof(ItchOrderCancel);            break;
        case ItchMessageType::ORDER_DELETE:           n = sizeof(ItchOrderDelete);            break;
        case ItchMessageType::ORDER_REPLACE:          n = sizeof(ItchOrderReplace);           break;
        case ItchMessageType::TRADE:                  n = sizeof(ItchTrade);                  break;
        default: return msg;
    }
    std::memcpy(&msg, bytes.data(), std::min(n, bytes.size()));
    return msg;
}

#endif //PROTOCOL_H
