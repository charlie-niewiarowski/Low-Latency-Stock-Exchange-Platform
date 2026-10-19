//
// Created by cniew on 10/18/26.
//
// A scripted TCP client for driving a real Server over net_ring0 (see
// ServerNetwork::loopback_test): the kernel's side of a connection, played
// with crafted segments.
//
// net_ring loops tx queue i back into rx queue i. The server polls rx 0 and
// sends control segments (SYN-ACK, pure ACKs, RSTs) on tx 1 and data/FINs on
// tx 2, so a Wire injects on tx 0 and reads rx 1 and rx 2, handing each
// segment to the TcpPeer it's addressed to (by port). Single-threaded: the
// test thread drives every peer.
//

#ifndef TCP_PEER_HPP
#define TCP_PEER_HPP

#include "soupbintcp_proto.hpp"

#include <ethernet_header.h>
#include <ipv4_header.h>
#include <packet_pool.h>
#include <rx_queue.h>
#include <tcp_header.h>
#include <tx_queue.h>
#include <rte_ip.h>
#include <rte_tcp.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace tcp_peer {

inline constexpr auto kTimeout = std::chrono::milliseconds(2000);
inline constexpr uint32_t kPeerIp = RTE_IPV4(10, 77, 0, 1);
inline const rte_ether_addr kPeerMac{{0x02, 0, 0, 0, 0, 0x01}};

// One segment, as the server sent it.
struct Segment {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  flags;
    uint16_t window;
    std::vector<std::byte> payload;
};

class Wire {
public:
    Wire(std::shared_ptr<dpdk::tx_queue> to_server, std::shared_ptr<dpdk::rx_queue> control,
         std::shared_ptr<dpdk::rx_queue> data, std::shared_ptr<dpdk::packet_pool> pool,
         const rte_ether_addr server_mac, const uint32_t server_ip, const uint16_t server_port)
        : to_server_(std::move(to_server)), control_(std::move(control)), data_(std::move(data)),
          pool_(std::move(pool)), server_mac_(server_mac), server_ip_(server_ip), server_port_(server_port) {}

    [[nodiscard]] uint16_t server_port() const { return server_port_; }

    bool inject(const uint16_t from_port, const uint32_t seq, const uint32_t ack, const uint8_t flags,
                const uint16_t window, const std::span<const std::byte> payload,
                const std::optional<uint16_t> to_port = std::nullopt) {
        dpdk::packet pkt = pool_->get();
        if (!pkt) return false;
        dpdk::build_ethernet_header(pkt, server_mac_, kPeerMac, RTE_ETHER_TYPE_IPV4);
        rte_ipv4_hdr *ip = dpdk::build_ipv4_header(pkt, kPeerIp, server_ip_, IPPROTO_TCP);
        rte_tcp_hdr *tcp = dpdk::build_tcp_header(pkt, from_port, to_port.value_or(server_port_), seq, ack,
                                                  flags, window);
        if (!payload.empty() && pkt.append(dpdk::packet::Payload(payload.data(), payload.size())) == nullptr) {
            return false;
        }
        dpdk::finalize_ipv4_header(pkt, ip);
        dpdk::finalize_tcp_header(pkt, ip, tcp);
        std::array<dpdk::packet, 1> pkts{std::move(pkt)};
        return to_server_->send_burst(pkts) == 1;
    }

    // Raw bytes, not even Ethernet.
    bool inject_raw(const std::span<const std::byte> bytes) {
        dpdk::packet pkt = pool_->get();
        if (!pkt || pkt.append(dpdk::packet::Payload(bytes.data(), bytes.size())) == nullptr) return false;
        std::array<dpdk::packet, 1> pkts{std::move(pkt)};
        return to_server_->send_burst(pkts) == 1;
    }

    // Moves everything the server has sent into per-port inboxes.
    void pump() {
        for (const auto &queue : {control_, data_}) {
            dpdk::packet_burst burst = queue->receive_burst();
            for (const dpdk::packet &pkt : burst) {
                if (auto segment = parse(pkt)) inboxes_[segment->dst_port].push_back(std::move(*segment));
            }
        }
    }

    std::deque<Segment> &inbox(const uint16_t port) { return inboxes_[port]; }

    void drain() {
        pump();
        inboxes_.clear();
    }

private:
    static std::optional<Segment> parse(const dpdk::packet &pkt) {
        if (pkt.length() < sizeof(rte_ether_hdr) + sizeof(rte_ipv4_hdr) + sizeof(rte_tcp_hdr)) return std::nullopt;
        const auto *eth = reinterpret_cast<const rte_ether_hdr *>(pkt.data());
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) return std::nullopt;
        const auto *ip = reinterpret_cast<const rte_ipv4_hdr *>(eth + 1);
        if (ip->next_proto_id != IPPROTO_TCP) return std::nullopt;
        const auto *tcp = reinterpret_cast<const rte_tcp_hdr *>(
            reinterpret_cast<const uint8_t *>(ip) + rte_ipv4_hdr_len(ip));
        const size_t data_off = (tcp->data_off >> 4) * 4u;
        const size_t l4_len = rte_be_to_cpu_16(ip->total_length) - rte_ipv4_hdr_len(ip);
        const auto *payload = reinterpret_cast<const std::byte *>(tcp) + data_off;
        return Segment{
            .src_port = rte_be_to_cpu_16(tcp->src_port),
            .dst_port = rte_be_to_cpu_16(tcp->dst_port),
            .seq = rte_be_to_cpu_32(tcp->sent_seq),
            .ack = rte_be_to_cpu_32(tcp->recv_ack),
            .flags = tcp->tcp_flags,
            .window = rte_be_to_cpu_16(tcp->rx_win),
            .payload = {payload, payload + (l4_len - data_off)},
        };
    }

    std::shared_ptr<dpdk::tx_queue> to_server_;
    std::shared_ptr<dpdk::rx_queue> control_;
    std::shared_ptr<dpdk::rx_queue> data_;
    std::shared_ptr<dpdk::packet_pool> pool_;
    rte_ether_addr server_mac_;
    uint32_t server_ip_;
    uint16_t server_port_;
    std::unordered_map<uint16_t, std::deque<Segment>> inboxes_;
};

// One SoupBinTCP packet from the server.
struct SoupReply {
    SoupPacketType         type;
    std::vector<std::byte> payload;
};

class TcpPeer {
public:
    TcpPeer(Wire &wire, const uint16_t port, const uint16_t window = 65535)
        : wire_(wire), port_(port), window_(window) {}

    [[nodiscard]] uint16_t port() const { return port_; }
    [[nodiscard]] bool fin_received() const { return fin_received_; }
    [[nodiscard]] bool reset_received() const { return reset_received_; }

    // The three-way handshake.
    bool connect() {
        if (!wire_.inject(port_, snd_nxt_ - 1, 0, RTE_TCP_SYN_FLAG, window_, {})) return false;
        const auto deadline = std::chrono::steady_clock::now() + kTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            wire_.pump();
            auto &inbox = wire_.inbox(port_);
            while (!inbox.empty()) {
                const Segment segment = std::move(inbox.front());
                inbox.pop_front();
                if (segment.flags == (RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG) && segment.ack == snd_nxt_) {
                    rcv_nxt_ = segment.seq + 1;
                    return ack();
                }
            }
        }
        return false;
    }

    // One segment carrying bytes.
    bool send(const std::span<const std::byte> bytes) {
        if (!wire_.inject(port_, snd_nxt_, rcv_nxt_, RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG, window_, bytes)) return false;
        snd_nxt_ += static_cast<uint32_t>(bytes.size());
        return true;
    }

    // Advertises a new receive window (0 stops the server sending).
    bool set_window(const uint16_t window) {
        window_ = window;
        return ack();
    }

    bool send_fin() {
        if (!wire_.inject(port_, snd_nxt_, rcv_nxt_, RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG, window_, {})) return false;
        ++snd_nxt_;
        return true;
    }

    bool send_rst() { return wire_.inject(port_, snd_nxt_, rcv_nxt_, RTE_TCP_RST_FLAG | RTE_TCP_ACK_FLAG, 0, {}); }

    // Takes in whatever the server has sent this peer, ACKing it.
    void poll() {
        wire_.pump();
        bool advanced = false;
        auto &inbox = wire_.inbox(port_);
        while (!inbox.empty()) {
            Segment segment = std::move(inbox.front());
            inbox.pop_front();
            if (segment.flags & RTE_TCP_RST_FLAG) {
                reset_received_ = true;
                continue;
            }
            // Data arrives in order on a lossless ring; skip anything already had.
            if (!segment.payload.empty() && segment.seq == rcv_nxt_) {
                stream_.insert(stream_.end(), segment.payload.begin(), segment.payload.end());
                rcv_nxt_ += static_cast<uint32_t>(segment.payload.size());
                advanced = true;
            }
            if ((segment.flags & RTE_TCP_FIN_FLAG) && segment.seq + segment.payload.size() == rcv_nxt_) {
                fin_received_ = true;
                ++rcv_nxt_;
                advanced = true;
            }
        }
        if (advanced) ack();
    }

    // The next SoupBinTCP packet from the server, reassembled across
    // segments; heartbeats are skipped unless asked for.
    std::optional<SoupReply> receive(const bool skip_heartbeats = true,
                                     const std::chrono::milliseconds timeout = kTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            poll();
            while (true) {
                const SoupParseResult parsed = parse_soup_packet(stream_);
                if (parsed.status != SoupParseStatus::PACKET) break;
                SoupReply reply{parsed.packet.type, {parsed.packet.payload.begin(), parsed.packet.payload.end()}};
                stream_.erase(stream_.begin(), stream_.begin() + static_cast<std::ptrdiff_t>(parsed.consumed));
                if (skip_heartbeats && reply.type == SoupPacketType::SERVER_HEARTBEAT) continue;
                return reply;
            }
        } while (std::chrono::steady_clock::now() < deadline);
        return std::nullopt;
    }

    // Waits for the server's FIN (or RST).
    bool wait_closed(const std::chrono::milliseconds timeout = kTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!fin_received_ && !reset_received_ && std::chrono::steady_clock::now() < deadline) poll();
        return fin_received_ || reset_received_;
    }

    // Bytes received but not yet returned by receive().
    [[nodiscard]] size_t buffered() const { return stream_.size(); }

private:
    bool ack() { return wire_.inject(port_, snd_nxt_, rcv_nxt_, RTE_TCP_ACK_FLAG, window_, {}); }

    Wire &wire_;
    uint16_t port_;
    uint16_t window_;
    uint32_t snd_nxt_ = 0x1000'0001; // ISN + 1
    uint32_t rcv_nxt_ = 0;
    bool fin_received_ = false;
    bool reset_received_ = false;
    std::vector<std::byte> stream_;
};

} // namespace tcp_peer

#endif // TCP_PEER_HPP
