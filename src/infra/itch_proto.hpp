//
// Created by cniew on 5/16/26.
//

#ifndef ITCH_PROTO_H
#define ITCH_PROTO_H

#include "wire_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

//=============================================================================
// ITCH 5.0 (market data, exchange -> subscribers, one-way over UDPHeader multicast)
//=============================================================================
// Covers the per-order lifecycle messages this engine actually produces
// (add/execute/cancel/delete/replace/trade). Reference-data and market-wide
// administrative messages (System Event, Stock Directory, Stock Trading
// Action, MWCB, IPO Quoting Period, LULD, NOII, ...) are out of scope: this
// engine has no notion of trading sessions, halts, or multiple listed
// instruments' metadata to report.
//
// Same split as ouch_proto.hpp: the structs are decoded, host-order views;
// encode_itch/decode_itch produce/consume the spec wire layout (big-endian,
// unpadded, fixed length per message type -- ITCH has no optional fields).
// Every message leads with ItchHeader (message_type, stock_locate, tracking
// number, timestamp) at the same offset, giving the union a common initial
// sequence.
//
// ITCH prices are 4 bytes with 4 implied decimal places, unlike OUCH's
// 8-byte prices.

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
    ItchMessageType message_type;
    uint16_t        stock_locate;
    uint16_t        tracking_number;
    uint64_t        timestamp;   // nanoseconds since midnight; only the low 48 bits go on the wire

    template <typename Self, typename IO>
    static void core(Self &h, IO &io) {
        io(h.message_type); io(h.stock_locate); io(h.tracking_number); io.u48(h.timestamp);
    }
};

// Each message exposes core(msg, io): every field, in wire order, header
// first (see wire_codec.hpp).

struct ItchAddOrder {
    ItchHeader      header{.message_type = ItchMessageType::ADD_ORDER, .stock_locate = 0, .tracking_number = 0, .timestamp = 0};
    uint64_t        order_reference_number;
    char            buy_sell_indicator;
    uint32_t        shares;
    ItchStockSymbol stock;
    uint32_t        price;

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        ItchHeader::core(m.header, io);
        io(m.order_reference_number); io(m.buy_sell_indicator); io(m.shares); io(m.stock); io(m.price);
    }
};

struct ItchOrderExecuted {
    ItchHeader header{.message_type = ItchMessageType::ORDER_EXECUTED, .stock_locate = 0, .tracking_number = 0, .timestamp = 0};
    uint64_t   order_reference_number;
    uint32_t   executed_shares;
    uint64_t   match_number;

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        ItchHeader::core(m.header, io);
        io(m.order_reference_number); io(m.executed_shares); io(m.match_number);
    }
};

struct ItchOrderExecutedWithPrice {
    ItchHeader header{.message_type = ItchMessageType::ORDER_EXECUTED_W_PRICE, .stock_locate = 0, .tracking_number = 0, .timestamp = 0};
    uint64_t   order_reference_number;
    uint32_t   executed_shares;
    uint64_t   match_number;
    char       printable;   // 'Y' or 'N'
    uint32_t   execution_price;

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        ItchHeader::core(m.header, io);
        io(m.order_reference_number); io(m.executed_shares); io(m.match_number); io(m.printable);
        io(m.execution_price);
    }
};

struct ItchOrderCancel {
    ItchHeader header{.message_type = ItchMessageType::ORDER_CANCEL, .stock_locate = 0, .tracking_number = 0, .timestamp = 0};
    uint64_t   order_reference_number;
    uint32_t   canceled_shares;

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        ItchHeader::core(m.header, io);
        io(m.order_reference_number); io(m.canceled_shares);
    }
};

struct ItchOrderDelete {
    ItchHeader header{.message_type = ItchMessageType::ORDER_DELETE, .stock_locate = 0, .tracking_number = 0, .timestamp = 0};
    uint64_t   order_reference_number;

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        ItchHeader::core(m.header, io);
        io(m.order_reference_number);
    }
};

struct ItchOrderReplace {
    ItchHeader header{.message_type = ItchMessageType::ORDER_REPLACE, .stock_locate = 0, .tracking_number = 0, .timestamp = 0};
    uint64_t   original_order_reference_number;
    uint64_t   new_order_reference_number;
    uint32_t   shares;
    uint32_t   price;

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        ItchHeader::core(m.header, io);
        io(m.original_order_reference_number); io(m.new_order_reference_number); io(m.shares);
        io(m.price);
    }
};

struct ItchTrade {
    ItchHeader      header{.message_type = ItchMessageType::TRADE, .stock_locate = 0, .tracking_number = 0, .timestamp = 0};
    uint64_t        order_reference_number;
    char            buy_sell_indicator;
    uint32_t        shares;
    ItchStockSymbol stock;
    uint32_t        price;
    uint64_t        match_number;

    template <typename Self, typename IO>
    static void core(Self &m, IO &io) {
        ItchHeader::core(m.header, io);
        io(m.order_reference_number); io(m.buy_sell_indicator); io(m.shares); io(m.stock);
        io(m.price); io(m.match_number);
    }
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
                     .tracking_number = 0, .timestamp = 0} {}
};

//=============================================================================
// Wire codec
//=============================================================================

namespace itch_detail {

template <typename M>
size_t encoded_size(const M &msg) noexcept {
    wire::WireSizer sizer;
    M::core(msg, sizer);
    return sizer.size();
}

template <typename M>
size_t encode(const M &msg, std::span<std::byte> out) noexcept {
    wire::WireWriter writer{out};
    M::core(msg, writer);
    return writer.ok() ? writer.written() : 0;
}

// ITCH messages are fixed-length, so anything but an exact fit is malformed.
template <typename M>
bool decode(std::span<const std::byte> bytes, M &msg) noexcept {
    wire::WireReader reader{bytes};
    M::core(msg, reader);
    return reader.ok() && reader.remaining() == 0;
}

// Calls fn with whichever member of msg is active; returns fallback for NONE.
template <typename Msg, typename F, typename R>
R visit(Msg &msg, F &&fn, R fallback) {
    switch (msg.header.message_type) {
        case ItchMessageType::NONE:                   return fallback;
        case ItchMessageType::ADD_ORDER:              return fn(msg.add_order);
        case ItchMessageType::ORDER_EXECUTED:         return fn(msg.order_executed);
        case ItchMessageType::ORDER_EXECUTED_W_PRICE: return fn(msg.order_executed_with_price);
        case ItchMessageType::ORDER_CANCEL:           return fn(msg.order_cancel);
        case ItchMessageType::ORDER_DELETE:           return fn(msg.order_delete);
        case ItchMessageType::ORDER_REPLACE:          return fn(msg.order_replace);
        case ItchMessageType::TRADE:                  return fn(msg.trade);
    }
    return fallback; // defensive: only reachable via an out-of-range enum value
}

} // namespace itch_detail

// Exact number of bytes encode_itch will write for msg; 0 for NONE.
inline size_t encoded_size(const ITCH &msg) noexcept {
    return itch_detail::visit(msg, [](const auto &m) { return itch_detail::encoded_size(m); }, size_t{0});
}

// Writes msg's spec wire bytes into out. Returns the number of bytes
// written, or 0 if msg is NONE or out is too small (see encoded_size).
inline size_t encode_itch(const ITCH &msg, std::span<std::byte> out) noexcept {
    return itch_detail::visit(msg, [&](const auto &m) { return itch_detail::encode(m, out); }, size_t{0});
}

// Decodes exactly one message spanning all of bytes. Returns a NONE ITCH
// on an empty buffer, an unrecognized type byte, or a length that doesn't
// match the message type.
inline ITCH decode_itch(std::span<const std::byte> bytes) noexcept {
    ITCH msg{};
    if (bytes.empty()) return msg;

    // Construct the right (default-initialized) member first so visit
    // dispatches on it, then decode into it.
    switch (static_cast<ItchMessageType>(bytes[0])) {
        case ItchMessageType::ADD_ORDER:              msg.add_order = ItchAddOrder{};                             break;
        case ItchMessageType::ORDER_EXECUTED:         msg.order_executed = ItchOrderExecuted{};                   break;
        case ItchMessageType::ORDER_EXECUTED_W_PRICE: msg.order_executed_with_price = ItchOrderExecutedWithPrice{}; break;
        case ItchMessageType::ORDER_CANCEL:           msg.order_cancel = ItchOrderCancel{};                       break;
        case ItchMessageType::ORDER_DELETE:           msg.order_delete = ItchOrderDelete{};                       break;
        case ItchMessageType::ORDER_REPLACE:          msg.order_replace = ItchOrderReplace{};                     break;
        case ItchMessageType::TRADE:                  msg.trade = ItchTrade{};                                    break;
        default:                                      return msg;
    }

    const bool ok = itch_detail::visit(msg, [&](auto &m) { return itch_detail::decode(bytes, m); }, false);
    return ok ? msg : ITCH{};
}

#endif //ITCH_PROTO_H
