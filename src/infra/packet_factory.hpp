//
// Created by cniew on 9/3/26.
//

#ifndef PACKET_FACTORY_HPP
#define PACKET_FACTORY_HPP

#include <packet.h>
#include <packet_pool.h>
#include <ethernet_header.h>
#include <ipv4_header.h>
#include <udp_header.h>
#include <tcp_header.h>

#include <rte_ether.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <netinet/in.h> // IPPROTO_TCP / IPPROTO_UDP

#include "protocol.hpp"


struct PacketEndpoint {
    rte_ether_addr mac;
    uint32_t       ip; // host byte order
};

inline rte_ether_addr to_rte_ether_addr(const MacAddress &mac) noexcept {
    rte_ether_addr addr{};
    std::memcpy(addr.addr_bytes, mac.data(), mac.size());
    return addr;
}


template <typename transport_type, typename payload_type>
concept ValidPacketCombo =
    (std::is_same_v<transport_type, TCPHeader> && std::is_same_v<payload_type, OUCH>) ||
    (std::is_same_v<transport_type, UDPHeader> && std::is_same_v<payload_type, ITCH>);


template <typename transport_type, typename payload_type>
    requires ValidPacketCombo<transport_type, payload_type>
struct packet_factory {
public:
    static dpdk::packet build_packet(
        dpdk::packet_pool &pool,
        const PacketEndpoint &src, const PacketEndpoint &dst,
        const transport_type &transport_payload,
        const payload_type &payload_payload);

    // src is learned from this same packet's own Ethernet/IPv4 headers --
    // it's how the caller finds out where to actually send a reply.
    // Default-constructed (src.ip == 0, transport/payload zeroed) if the
    // packet is too short, isn't IPv4, has IPv4 options, or its IP
    // protocol doesn't match transport_type -- callers must check that
    // rather than assume a parse always succeeds.
    struct DeserializedPacket {
        PacketEndpoint  src{};
        transport_type  transport{};
        payload_type    payload{};
    };

    static DeserializedPacket deserialize_packet(const dpdk::packet &packet);

private:
    static rte_udp_hdr *append_udp(dpdk::packet &pkt, const transport_type &transport_payload);
    static rte_tcp_hdr *append_tcp(dpdk::packet &pkt, const transport_type &transport_payload);

    static void append_ouch(dpdk::packet &pkt, const payload_type &payload);
    static void append_itch(dpdk::packet &pkt, const payload_type &payload);
};


template <typename transport_type, typename payload_type>
    requires ValidPacketCombo<transport_type, payload_type>
dpdk::packet packet_factory<transport_type, payload_type>::build_packet(
    dpdk::packet_pool &pool,
    const PacketEndpoint &src, const PacketEndpoint &dst,
    const transport_type &transport_payload,
    const payload_type &payload_payload)
{
    dpdk::packet pkt = pool.get();
    if (!pkt) return pkt;

    dpdk::build_ethernet_header(pkt, dst.mac, src.mac, RTE_ETHER_TYPE_IPV4);

    constexpr uint8_t next_proto = std::is_same_v<transport_type, TCPHeader> ? IPPROTO_TCP : IPPROTO_UDP;
    rte_ipv4_hdr *ip_hdr = dpdk::build_ipv4_header(pkt, src.ip, dst.ip, next_proto);

    if constexpr (std::is_same_v<transport_type, TCPHeader>) {
        rte_tcp_hdr *tcp_hdr = append_tcp(pkt, transport_payload);
        append_ouch(pkt, payload_payload);
        dpdk::finalize_ipv4_header(pkt, ip_hdr);
        dpdk::finalize_tcp_header(pkt, ip_hdr, tcp_hdr);
    } else {
        rte_udp_hdr *udp_hdr = append_udp(pkt, transport_payload);
        append_itch(pkt, payload_payload);
        dpdk::finalize_ipv4_header(pkt, ip_hdr);
        dpdk::finalize_udp_header(pkt, ip_hdr, udp_hdr);
    }

    return pkt;
}

template <typename transport_type, typename payload_type>
    requires ValidPacketCombo<transport_type, payload_type>
rte_udp_hdr *packet_factory<transport_type, payload_type>::append_udp(
    dpdk::packet &pkt, const transport_type &transport_payload)
{
    return dpdk::build_udp_header(pkt, transport_payload.src_port, transport_payload.dst_port);
}

template <typename transport_type, typename payload_type>
    requires ValidPacketCombo<transport_type, payload_type>
rte_tcp_hdr *packet_factory<transport_type, payload_type>::append_tcp(
    dpdk::packet &pkt, const transport_type &transport_payload)
{
    return dpdk::build_tcp_header(pkt, transport_payload.src_port, transport_payload.dst_port,
                                   transport_payload.seq, transport_payload.ack,
                                   transport_payload.flags, transport_payload.window);
}

template <typename transport_type, typename payload_type>
    requires ValidPacketCombo<transport_type, payload_type>
void packet_factory<transport_type, payload_type>::append_ouch(
    dpdk::packet &pkt, const payload_type &payload)
{
    pkt.append(serialize_ouch(payload));
}

template <typename transport_type, typename payload_type>
    requires ValidPacketCombo<transport_type, payload_type>
void packet_factory<transport_type, payload_type>::append_itch(
    dpdk::packet &pkt, const payload_type &payload)
{
    pkt.append(serialize_itch(payload));
}

template <typename transport_type, typename payload_type>
    requires ValidPacketCombo<transport_type, payload_type>
typename packet_factory<transport_type, payload_type>::DeserializedPacket
packet_factory<transport_type, payload_type>::deserialize_packet(const dpdk::packet &packet)
{
    DeserializedPacket result;

    if (packet.length() < kEthernetHeaderLen) {
        return result;
    }

    const auto *base = reinterpret_cast<const std::byte *>(packet.data());
    const EthernetHeader eth = deserialize_ethernet({base, kEthernetHeaderLen});

    // Only IPv4 is meaningful here -- ARP/IPv6/anything else isn't a
    // client this server knows how to reply to.
    if (eth.ether_type != RTE_ETHER_TYPE_IPV4) {
        return result;
    }

    if (packet.length() < kEthernetHeaderLen + kIPv4HeaderLen) {
        return result;
    }

    const IPv4Header ip = deserialize_ipv4({base + kEthernetHeaderLen, kIPv4HeaderLen});

    // No IPv4 options anywhere on this wire (see build_ipv4_header) -- an
    // IHL other than 5 means a header this deserializer doesn't support,
    // not a different transport offset to go chase.
    if ((ip.version_ihl & 0x0F) != 5) {
        return result;
    }

    constexpr uint8_t expected_proto = std::is_same_v<transport_type, TCPHeader> ? IPPROTO_TCP : IPPROTO_UDP;
    if (ip.protocol != expected_proto) {
        return result;
    }

    // Fixed-size headers throughout this library (no VLAN tag, no IPv4
    // options -- checked above -- no TCPHeader/UDPHeader options -- see
    // build_tcp_header/build_udp_header's own comments), so once Ethernet
    // and IPv4 are accounted for, the transport header and then the
    // application payload sit at fixed offsets from there.
    constexpr size_t transport_hdr_len =
        std::is_same_v<transport_type, TCPHeader> ? sizeof(rte_tcp_hdr) : sizeof(rte_udp_hdr);
    constexpr size_t header_len = kEthernetHeaderLen + kIPv4HeaderLen + transport_hdr_len;

    if (packet.length() < header_len) {
        return result;
    }

    const std::span<const std::byte> transport_span{
        base + kEthernetHeaderLen + kIPv4HeaderLen, transport_hdr_len};
    const std::span<const std::byte> payload_span{
        base + header_len, static_cast<size_t>(packet.length() - header_len)};

    if constexpr (std::is_same_v<transport_type, TCPHeader>) {
        result.transport = deserialize_tcp(transport_span);
    } else {
        result.transport = deserialize_udp(transport_span);
    }
    if constexpr (std::is_same_v<payload_type, OUCH>) {
        result.payload = deserialize_ouch(payload_span);
    } else {
        result.payload = deserialize_itch(payload_span);
    }

    result.src.mac = to_rte_ether_addr(eth.src_mac);
    result.src.ip  = ip.src_ip;

    return result;
}

#endif //PACKET_FACTORY_HPP
