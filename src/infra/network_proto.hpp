//
// Created by cniew on 5/16/26.
//

#ifndef NETWORK_PROTO_H
#define NETWORK_PROTO_H

#include "wire_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

//=============================================================================
// Link/network layer headers (packet_factory deserialize path)
//=============================================================================
// Host-order, application-facing views of the Ethernet/IPv4 headers that
// actually go out on the wire via dpdk::build_ethernet_header/build_ipv4_header
// (real rte_ether_hdr/rte_ipv4_hdr layouts, network byte order, no VLAN tag,
// no IPv4 options -- see ethernet_header.h/ipv4_header.h in dpdk_lib).
// deserialize_ethernet/deserialize_ipv4 read those exact wire layouts back
// out through wire::WireReader, which does the network-to-host conversion
// with memcpy-based loads (the source bytes aren't guaranteed to be aligned
// for the wider integer types). Serialization has no equivalent here: it
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

    wire::WireReader reader{bytes};
    reader(header.dst_mac);
    reader(header.src_mac);
    reader(header.ether_type);
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

    wire::WireReader reader{bytes};
    reader(header.version_ihl);
    reader.take(1);                 // type of service
    reader(header.total_length);
    reader.take(4);                 // identification, flags + fragment offset
    reader(header.ttl);
    reader(header.protocol);
    reader.take(2);                 // header checksum
    reader(header.src_ip);
    reader(header.dst_ip);
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
// order through wire::WireReader (see deserialize_ethernet/deserialize_ipv4).
// data_off is always 5 (20-byte header, no
// options; see build_tcp_header) and isn't surfaced on TCPHeader; cksum/
// tcp_urp aren't needed here either.
inline constexpr size_t kTcpWireHeaderLen = 20;

inline TCPHeader deserialize_tcp(std::span<const std::byte> bytes) noexcept {
    TCPHeader header{};
    if (bytes.size() < kTcpWireHeaderLen) return header;

    wire::WireReader reader{bytes};
    reader(header.src_port);
    reader(header.dst_port);
    reader(header.seq);
    reader(header.ack);
    reader.take(1);                 // data_off
    reader(header.flags);
    reader(header.window);
    return header;
}

struct UDPHeader {
    uint16_t src_port;
    uint16_t dst_port;
};

// What's on the wire is dpdk::build_udp_header's real rte_udp_hdr --
// src_port(2), dst_port(2), dgram_len(2), dgram_cksum(2); 8 bytes, packed,
// network byte order. Same treatment as deserialize_tcp above: read the
// real wire offsets, convert src_port/dst_port back to host order via WireReader;
// dgram_len/dgram_cksum aren't needed here.
inline constexpr size_t kUdpWireHeaderLen = 8;

inline UDPHeader deserialize_udp(std::span<const std::byte> bytes) noexcept {
    UDPHeader header{};
    if (bytes.size() < kUdpWireHeaderLen) return header;

    wire::WireReader reader{bytes};
    reader(header.src_port);
    reader(header.dst_port);
    return header;
}

#endif //NETWORK_PROTO_H
