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
#include <netinet/in.h> // IPPROTO_TCP / IPPROTO_UDP

#include "protocol.hpp"


struct PacketEndpoint {
    rte_ether_addr mac;
    uint32_t       ip; // host byte order
};


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

    static payload_type deserialize_packet(const dpdk::packet &packet);

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
payload_type packet_factory<transport_type, payload_type>::deserialize_packet(const dpdk::packet &packet)
{
    // Fixed-size headers throughout this library (no VLAN tag, no IPv4
    // options, no TCPHeader options -- see build_ipv4_header/build_tcp_header's
    // own comments), so the application payload always starts at the same
    // fixed offset from the front of the frame.
    constexpr size_t transport_hdr_len =
        std::is_same_v<transport_type, TCPHeader> ? sizeof(rte_tcp_hdr) : sizeof(rte_udp_hdr);
    constexpr size_t header_len = sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + transport_hdr_len;

    if (packet.length() <= header_len) return payload_type{};

    const auto *payload_bytes = reinterpret_cast<const std::byte *>(packet.data() + header_len);
    const std::span<const std::byte> payload_span{
        payload_bytes, static_cast<size_t>(packet.length() - header_len)};

    if constexpr (std::is_same_v<payload_type, OUCH>) {
        return deserialize_ouch(payload_span);
    } else {
        return deserialize_itch(payload_span);
    }
}

#endif //PACKET_FACTORY_HPP
