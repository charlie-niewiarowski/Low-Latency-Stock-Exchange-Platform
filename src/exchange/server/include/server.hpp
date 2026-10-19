//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDERGATEWAY_HPP
#define TOYEXCHANGE_ORDERGATEWAY_HPP

#include "config.hpp"
#include "latency.hpp"
#include "order_entry.hpp"
#include "ouch_responder.hpp"
#include "session_log.hpp"
#include "soupbintcp_proto.hpp"

#include <runtime.h>
#include <tcp_stack.h>
#include <ring_buffer.hpp>
#include <absl/container/flat_hash_map.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/*
The server's mental model:

    DPDK rx -> tcp_receiver -> SoupBinTCP -> decode OUCH -> OrderEntry (validate, translate) -> engine inbound ring
                                   |                               |
                                   +--- login / logout / close ----+-> notes_ (sessions, records, gateway rejects)
                                                                            |
    DPDK tx <- tcp_sender <- session's SessionLog <- encode OUCH <- OuchResponder <- engine outbound ring

TCP is terminated by the DPDK library's tcp_stack, which has exactly the
same two-thread shape: inbound_ owns the tcp_receiver (and every rx queue
plus the control tx queue it ACKs on), outbound_ owns the tcp_sender (and
the data tx queue). The only state the threads share is the rings,
notes_, and retained_from_ (see below).

SoupBinTCP sessions are keyed on the Login Request's username: the first
login of a username creates its session (and ClientId -- its UserRefNum
space and orders); a later login resumes it, replaying from the requested
sequence number out of the session's SessionLog. One connection per
session at a time. A session's orders keep resting while it's
disconnected; what happens to them goes into its log for replay.

inbound_ owns everything about a connection's input: reassembling
SoupBinTCP packets across TCP segments, login, logout, and the client
heartbeat / login timeouts. It's the one that decides a connection is
over, and tells outbound_ (DISCONNECTED) to close it.

outbound_ owns everything sent: Login Accepted/Rejected, Sequenced Data
(appended to the session's log, then sent from it), and server
heartbeats. It never waits on a peer's TCP window -- the rule tcp_stack
requires so the inbound thread's ACK processing can always reopen it --
and drops a connection whose unsent backlog would outgrow its log.
*/

// Which DPDK port the server runs on and how its queues are used. The
// inbound thread polls polled_rx and sends TCP control segments on
// control_tx; the outbound thread sends data on data_tx.
struct ServerNetwork {
    struct Tap {
        std::string iface;       // the kernel-side interface the tap PMD creates
        uint32_t    kernel_ipv4; // its address, host order
        uint8_t     prefix_len;
        int         rps_core = -1; // where the kernel processes what we send (see TAP_RPS_CORE); -1: inline
    };

    dpdk::runtime_params  eal;
    uint16_t              rx_queues;
    uint16_t              tx_queues;
    std::vector<uint16_t> polled_rx;
    uint16_t              control_tx;
    uint16_t              data_tx;
    uint32_t              ipv4; // the server's address, host order
    uint16_t              listen_port;
    std::optional<Tap>    tap;  // set for a net_tap port

    // A DPDK-bound NIC on the PCI bus, with hugepages. Note tcp_stack
    // assumes a lossless path (no retransmission yet).
    static ServerNetwork nic();
    // A net_tap port: a kernel interface on this host, so kernel sockets
    // (the load generator) can connect. Needs CAP_NET_ADMIN.
    static ServerNetwork tap_interface(std::string iface = EXCHANGE_TAP_IFACE);
    // Whatever config.hpp's EXCHANGE_NETWORK picks.
    static ServerNetwork from_config();
    // server-tests: a software-only EAL with a net_ring0 vdev, which loops
    // tx queue i back into rx queue i. The server polls rx 0 and sends on
    // tx 1 (control) and tx 2 (data); the test plays the client on tx 0
    // and reads rx 1 and rx 2.
    static ServerNetwork loopback_test();
};

class Server {
    friend class ServerTest;

public:
    Server() = delete;
    Server(std::shared_ptr<InboundRing> inbound_ring,
           std::shared_ptr<OutboundRing> outbound_ring,
           ServerNetwork network = ServerNetwork::from_config());

    Server(const Server &) = delete;
    Server(Server &&) = delete;
    Server &operator=(const Server &) = delete;
    Server &operator=(Server &&) = delete;

    ~Server();

    // Starts the inbound and outbound threads and returns; stop() (or the
    // destructor) ends and joins them.
    void run();
    void stop();

private:
    // At most one connection per session, plus room for connections that
    // haven't logged in (or are being turned away).
    static constexpr uint16_t kMaxConnections = 2 * MAX_CLIENTS;

    ServerNetwork network_;
    dpdk::runtime dpdk_runtime_;
    dpdk::port &dpdk_port_;
    std::vector<std::shared_ptr<dpdk::rx_queue>> rx_queues_; // inbound-thread only
    dpdk::tcp_stack tcp_;
    dpdk::tcp_receiver tcp_rx_; // inbound-thread only
    dpdk::tcp_sender tcp_tx_;   // outbound-thread only

    std::shared_ptr<InboundRing> inbound_ring_;
    std::shared_ptr<OutboundRing> outbound_ring_;

    // inbound_ -> outbound_: session events plus OrderEntry's GatewayNotes,
    // on one ring so a login is always handled before the orders that
    // follow it.
    struct ServerNote {
        enum class Kind : uint8_t {
            GATEWAY,        // gateway carries an OrderEntry note
            LOGIN_ACCEPTED, // conn is now client_id's; replay from sequence (0: from the next new message)
            LOGIN_REJECTED, // send Login Rejected (reject_reason, a soup_reject code) on conn, then close it
            DISCONNECTED,   // conn is over (client_id: its session, if it had logged in): close it
        };
        Kind          kind;
        char          reject_reason;
        ClientId      client_id;
        dpdk::conn_id conn;
        uint64_t      sequence;
        GatewayNote   gateway;
    };
    RingBuffer<ServerNote> notes_{COMMUNICATION_RING_COUNT};

    // Per session: the oldest sequence number its log still holds, so
    // inbound_ can turn away a re-login asking for an older one before
    // accepting anything else on that connection. Written by outbound_.
    std::array<std::atomic<uint64_t>, MAX_CLIENTS + 1> retained_from_{};

    // Inbound-thread only.
    OrderEntry order_entry_{validation::PriceBand{.min = MIN_PRICE, .max = MAX_PRICE}};
    // Outbound-thread only.
    OuchResponder responder_;
    LatencyHandler latency_; // prints its histograms when the Server is destroyed

    //===== inbound-thread state =====

    // The largest SoupBinTCP packet a client may send (an Enter Order with
    // every appendage is well under this). Anything longer can't be
    // buffered across segments, so it ends the connection.
    static constexpr size_t kMaxInboundSoupPacket = 512;

    // One per tcp_stack slot (conn_id::slot).
    struct InboundConnection {
        dpdk::conn_id conn;
        bool          open = false;     // accepted, and DISCONNECTED not yet pushed
        bool          logged_in = false;
        bool          closing = false;  // turned away: ignore its input until it's gone
        ClientId      client_id = 0;
        Timestamp     opened_tsc = 0;
        Timestamp     last_recv_tsc = 0;
        // The start of a SoupBinTCP packet split across segments.
        uint16_t partial_len = 0;
        std::array<std::byte, kMaxInboundSoupPacket> partial{};
    };
    std::vector<InboundConnection> inbound_connections_ = std::vector<InboundConnection>(kMaxConnections);

    struct InboundSession {
        bool connected = false; // a connection is logged into it
    };
    std::array<InboundSession, MAX_CLIENTS + 1> inbound_sessions_{};
    // Normalized username (upper-case, space-padded, packed) -> its session.
    absl::flat_hash_map<uint64_t, ClientId> usernames_;
    ClientId next_client_id_ = 1; // 0 reserved as "no client"

    struct InboundStamps {
        Timestamp read_begin_tsc;
        Timestamp recv_tsc;
    };
    void on_tcp_event_(const dpdk::tcp_event &event, const InboundStamps &stamps);
    void on_data_(InboundConnection &connection, std::span<const std::byte> bytes, const InboundStamps &stamps);
    // false once the connection is no longer taking input.
    bool handle_soup_packet_(InboundConnection &connection, const SoupPacket &packet, const InboundStamps &stamps);
    void login_(InboundConnection &connection, std::span<const std::byte> payload);
    void reject_login_(InboundConnection &connection, char reason);
    void handle_ouch_(ClientId client_id, const OuchInboundFrame &msg, const InboundStamps &stamps);
    // Ends the connection from the inbound side: its session is free again
    // and outbound_ closes it. Safe to call more than once.
    void disconnect_(InboundConnection &connection);
    void check_timeouts_(Timestamp now);
    void push_note_(const ServerNote &note);

    //===== outbound-thread state =====

    // Server-generated, unsequenced packets (Login Accepted/Rejected, Server
    // Heartbeat) waiting to go out ahead of the connection's log bytes.
    static constexpr size_t kMaxControlPacket = 64;

    // One per tcp_stack slot.
    struct OutboundConnection {
        dpdk::conn_id conn;
        bool          open = false;     // ours to send on; close()d once this goes false
        bool          close_when_flushed = false;
        bool          dirty = false;    // in dirty_ (has something to send)
        ClientId      client_id = 0;    // the session it's logged into, if any
        Timestamp     last_sent_tsc = 0;
        uint8_t       control_len = 0;
        uint8_t       control_sent = 0;
        std::array<std::byte, kMaxControlPacket> control{};
        uint32_t      closed_generation = 0; // the last generation close()d in this slot
    };
    std::vector<OutboundConnection> outbound_connections_ = std::vector<OutboundConnection>(kMaxConnections);
    std::vector<uint32_t> dirty_; // slots with bytes to send

    struct OutboundSession {
        std::unique_ptr<SessionLog> log;   // created on the session's first login
        std::optional<uint32_t>     slot;  // the connection logged into it
        uint64_t                    cursor = 0; // log offset of the next byte to send that connection
        // Latency samples whose packets haven't fully gone out yet, oldest
        // first; each completes when cursor passes its end offset.
        struct PendingSample {
            uint64_t       end_offset;
            PendingLatency latency;
        };
        static constexpr size_t kMaxPendingSamples = 64;
        std::array<PendingSample, kMaxPendingSamples> samples{};
        size_t samples_head = 0, samples_count = 0;
    };
    std::array<OutboundSession, MAX_CLIENTS + 1> outbound_sessions_{};

    // Room for any one SoupBinTCP packet the server sends: the largest is a
    // Sequenced Data packet around an Order Accepted carrying every appendage.
    static constexpr size_t kMaxSoupPacket = 256;
    std::array<std::byte, kMaxSoupPacket> scratch_{};

    void on_note_(const ServerNote &note);
    void accept_login_(const ServerNote &note);
    // Starts outbound_'s state for a connection it's about to send on.
    OutboundConnection &open_connection_(dpdk::conn_id conn);
    void send_ouch_(const OuchResponse &response, const PendingLatency *latency);
    // Queues a control packet on a connection that has none pending.
    void send_control_(OutboundConnection &connection, std::span<const std::byte> packet);
    // Appends one Sequenced Data packet to the session (and so to its
    // connection's send queue), with the latency sample it completes, if any.
    void send_sequenced_(ClientId client_id, std::span<const std::byte> packet, const PendingLatency *latency);
    void mark_dirty_(OutboundConnection &connection);
    // Sends what each dirty connection has, as far as the peer's window allows.
    void flush_();
    // false while the connection still has bytes left to send.
    bool flush_connection_(OutboundConnection &connection);
    void close_(dpdk::conn_id conn);
    void heartbeat_(Timestamp now);

    std::atomic<bool> shutdown_{false};
    std::jthread inbound_thread_;
    std::jthread outbound_thread_;

    void inbound_();
    void outbound_();
};

#endif //TOYEXCHANGE_ORDERGATEWAY_HPP
