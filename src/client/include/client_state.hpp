//
// Created by cniew on 5/24/26.
//

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include "buffer.hpp"
#include "order_types.hpp"
#include "../config/config.hpp"

#include <cstddef>
#include <cstdint>

// Write side: one batch at a time -- the Login Request, a heartbeat, or up
// to MAX_BATCH_MESSAGES SoupBinTCP-framed OUCH messages (an Enter, the
// largest, is 3 + 47 bytes with no appendage).
inline constexpr size_t kWriteBufBytes = 4096;
// Read side: every in-flight Enter can fan out into several responses
// (Accepted, one Executed per fill, Canceled for an IOC/market remainder).
inline constexpr size_t kReadBufBytes = 64 * 1024;

struct ClientState {
    //===== identity =====
    uint32_t client_id = 0;
    int fd = -1;
    bool connecting = false;  // true while non-blocking connect() is pending
    bool logged_in = false;   // SoupBinTCP Login Accepted received on this connection

    //===== write buf =====
    Buffer<kWriteBufBytes> write_buf;

    //===== read buf =====
    Buffer<kReadBufBytes> read_buf;

    //===== pipelining =====
    // Enters sent but not yet answered. Only Enters count against
    // PIPELINE_DEPTH: each one always gets exactly one Accepted or Rejected,
    // while a Cancel/Replace may legitimately get no response at all.
    uint32_t acks_pending = 0;
    uint64_t next_send_ns = 0;   // earliest time (CLOCK_MONOTONIC ns) to send next request
    uint64_t last_sent_ns = 0;   // for SoupBinTCP client heartbeats

    // Reconnect backoff: when fd == -1, the earliest time to attempt reconnect.
    // Prevents a dropped/refused connection from turning into a tight connect()
    // spin loop that exhausts ephemeral ports.
    uint64_t reconnect_at_ns = 0;
    uint32_t reconnect_backoff_ms = 0;  // linear backoff, capped at RECONNECT_DELAY_MS

    // OUCH UserRefNums must be day-unique and strictly increasing, so this
    // is never reset -- not even on reconnect.
    uint32_t next_user_ref_num = 1;

    // SoupBinTCP sequence number of the next Sequenced Data packet expected.
    // Kept across reconnects: a re-login asks for it, so the exchange replays
    // whatever was missed while disconnected instead of everything since 1.
    uint64_t next_sequence = 1;

    // Live-order ring: UserRefNums (and sides) from Accepted/Replaced, used as
    // CANCEL/REPLACE targets. Entries aren't removed when an order dies; a
    // stale target just gets silently ignored by the exchange.
    struct ActiveOrder {
        uint32_t user_ref_num;
        Side     side;
    };
    ActiveOrder active_orders[MAX_ACTIVE_ORDERS]{};
    uint32_t active_write = 0;
    uint32_t active_count = 0;
    uint32_t active_read  = 0;

    //===== stats =====
    uint64_t requests_sent = 0;
    uint64_t responses_ack = 0;       // Order Accepted
    uint64_t responses_match = 0;     // Order Executed
    uint64_t responses_err = 0;       // Order Rejected
    uint64_t responses_canceled = 0;  // Order Canceled
    uint64_t responses_replaced = 0;  // Order Replaced
    int64_t orders_in_flight = 0;
};

#endif // CLIENT_STATE_H
