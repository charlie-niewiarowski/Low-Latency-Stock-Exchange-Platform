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
// Link/network layer headers (packet_factory deserialize path)
//=============================================================================
// Host-order, application-facing views of the Ethernet/IPv4 headers that
// actually go out on the wire via dpdk::build_ethernet_header/build_ipv4_header
// (real rte_ether_hdr/rte_ipv4_hdr layouts, network byte order, no VLAN tag,
// no IPv4 options -- see ethernet_header.h/ipv4_header.h in dpdk_lib).
// deserialize_ethernet/deserialize_ipv4 read those exact wire layouts back
// out and do the network-to-host conversion by hand (byte-at-a-time, not a
// type-punned read) since the source bytes aren't guaranteed to be aligned
// for the wider integer types. Serialization has no equivalent here: it
// needs the finished packet's length for total_length/checksum, which
// doesn't fit the "struct in, span out" shape below, so it stays exactly
// where it already correctly lives -- dpdk::build_ethernet_header/
// build_ipv4_header/finalize_ipv4_header in dpdk_lib.

using MacAddress = std::array<uint8_t, 6>;

struct EthernetHeader {
    MacAddress dst_mac{};
    MacAddress src_mac{};
    uint16_t   ether_type = 0; // host byte order, e.g. 0x0800 for IPv4
};

inline constexpr size_t kEthernetHeaderLen = 14; // dst(6) + src(6) + ethertype(2), no VLAN tag

inline EthernetHeader deserialize_ethernet(std::span<const std::byte> bytes) noexcept {
    EthernetHeader header;
    if (bytes.size() < kEthernetHeaderLen) return header;

    const auto *raw = reinterpret_cast<const uint8_t *>(bytes.data());
    std::memcpy(header.dst_mac.data(), raw, header.dst_mac.size());
    std::memcpy(header.src_mac.data(), raw + 6, header.src_mac.size());
    header.ether_type = static_cast<uint16_t>((raw[12] << 8) | raw[13]);
    return header;
}

struct IPv4Header {
    uint8_t  version_ihl = 0; // upper nibble version, lower nibble header length in 32-bit words
    uint8_t  ttl          = 0;
    uint8_t  protocol     = 0; // IPPROTO_TCP / IPPROTO_UDP
    uint16_t total_length = 0; // host byte order
    uint32_t src_ip       = 0; // host byte order
    uint32_t dst_ip       = 0; // host byte order
};

// Fixed 20-byte header, no options -- matches what build_ipv4_header always
// writes. version_ihl on a parsed header should read 0x45; anything else
// means either options (unsupported) or a packet not worth trusting.
inline constexpr size_t kIPv4HeaderLen = 20;

inline IPv4Header deserialize_ipv4(std::span<const std::byte> bytes) noexcept {
    IPv4Header header;
    if (bytes.size() < kIPv4HeaderLen) return header;

    const auto *raw = reinterpret_cast<const uint8_t *>(bytes.data());
    header.version_ihl  = raw[0];
    header.ttl          = raw[8];
    header.protocol     = raw[9];
    header.total_length = static_cast<uint16_t>((raw[2] << 8) | raw[3]);
    header.src_ip = (static_cast<uint32_t>(raw[12]) << 24) | (static_cast<uint32_t>(raw[13]) << 16) |
                    (static_cast<uint32_t>(raw[14]) << 8)  |  static_cast<uint32_t>(raw[15]);
    header.dst_ip = (static_cast<uint32_t>(raw[16]) << 24) | (static_cast<uint32_t>(raw[17]) << 16) |
                    (static_cast<uint32_t>(raw[18]) << 8)  |  static_cast<uint32_t>(raw[19]);
    return header;
}

//=============================================================================
// Transport protocols (market_feed / packet_factory)
//=============================================================================
// Minimal transport-layer headers packet_factory writes onto outgoing DPDK
// packets. OUCH rides on TCPHeader (order entry); ITCH rides on UDPHeader (market data,
// multicast).

struct TCPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq    = 0;
    uint32_t ack    = 0;
    uint8_t  flags  = 0;      // bitwise-OR of the sender's TCP_*_FLAG bits
    uint16_t window = 65535;
};

// What's actually on the wire at this offset is dpdk::build_tcp_header's
// real, network-byte-order rte_tcp_hdr -- src_port(2), dst_port(2),
// sent_seq(4), recv_ack(4), data_off(1), tcp_flags(1), rx_win(2), cksum(2),
// tcp_urp(2); 20 bytes, packed -- not TCPHeader's own layout, so this reads
// those exact wire offsets and converts each multi-byte field back to host
// order by hand (not a type-punned read -- see deserialize_ethernet/
// deserialize_ipv4 for why). data_off is always 5 (20-byte header, no
// options; see build_tcp_header) and isn't surfaced on TCPHeader; cksum/
// tcp_urp aren't needed here either.
inline constexpr size_t kTcpWireHeaderLen = 20;

inline TCPHeader deserialize_tcp(std::span<const std::byte> bytes) noexcept {
    TCPHeader header{};
    if (bytes.size() < kTcpWireHeaderLen) return header;

    const auto *raw = reinterpret_cast<const uint8_t *>(bytes.data());
    header.src_port = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    header.dst_port = static_cast<uint16_t>((raw[2] << 8) | raw[3]);
    header.seq = (static_cast<uint32_t>(raw[4]) << 24) | (static_cast<uint32_t>(raw[5]) << 16) |
                 (static_cast<uint32_t>(raw[6]) << 8)  |  static_cast<uint32_t>(raw[7]);
    header.ack = (static_cast<uint32_t>(raw[8]) << 24) | (static_cast<uint32_t>(raw[9]) << 16) |
                 (static_cast<uint32_t>(raw[10]) << 8)  |  static_cast<uint32_t>(raw[11]);
    header.flags  = raw[13];
    header.window = static_cast<uint16_t>((raw[14] << 8) | raw[15]);
    return header;
}

struct UDPHeader {
    uint16_t src_port;
    uint16_t dst_port;
};

// What's on the wire is dpdk::build_udp_header's real rte_udp_hdr --
// src_port(2), dst_port(2), dgram_len(2), dgram_cksum(2); 8 bytes, packed,
// network byte order. Same treatment as deserialize_tcp above: read the
// real wire offsets, convert src_port/dst_port back to host order by hand;
// dgram_len/dgram_cksum aren't needed here.
inline constexpr size_t kUdpWireHeaderLen = 8;

inline UDPHeader deserialize_udp(std::span<const std::byte> bytes) noexcept {
    UDPHeader header{};
    if (bytes.size() < kUdpWireHeaderLen) return header;

    const auto *raw = reinterpret_cast<const uint8_t *>(bytes.data());
    header.src_port = static_cast<uint16_t>((raw[0] << 8) | raw[1]);
    header.dst_port = static_cast<uint16_t>((raw[2] << 8) | raw[3]);
    return header;
}

//=============================================================================
// OUCH 5.0 (order entry, client <-> exchange, carried over TCPHeader)
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
// ITCH 5.0 (market data, exchange -> subscribers, one-way over UDPHeader multicast)
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
