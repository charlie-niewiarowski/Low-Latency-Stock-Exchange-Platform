//
// Created by cniew on 5/24/26.
//

#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include "buffer.hpp"
#include "communication_types.hpp"
#include "protocol.hpp"
#include "../config/config.hpp"

struct ClientState {
    //===== identity =====
    uint32_t client_id = 0;
    int fd = -1;
    bool connecting = false;  // true while non-blocking connect() is pending

    //===== write buf =====
    Buffer<PIPELINE_DEPTH * INBOUND_BSIZE> write_buf;

    //===== read buf =====
    Buffer<PIPELINE_DEPTH * 2 * OUTBOUND_BSIZE> read_buf;

    //===== pipelining =====
    uint32_t acks_pending = 0;   // frames sent but not yet responded to
    uint64_t next_send_ns = 0;   // earliest time (CLOCK_MONOTONIC ns) to send next request

    // Reconnect backoff: when fd == -1, the earliest time to attempt reconnect.
    // Prevents a dropped/refused connection from turning into a tight connect()
    // spin loop that exhausts ephemeral ports.
    uint64_t reconnect_at_ns = 0;
    uint32_t reconnect_backoff_ms = 0;  // linear backoff, capped at RECONNECT_DELAY_MS

    // Pending request types: FIFO ring parallel to the in-flight pipeline.
    // pending_tail advances on every send; pending_head on every ACK consumed.
    MessageType pending_types[PIPELINE_DEPTH]{};
    uint32_t    pending_head = 0;
    uint32_t    pending_tail = 0;

    // Active order ring: order IDs from NEW ACKs, used as CANCEL/MODIFY targets.
    // active_write/active_count track the write side; active_read cycles reads.
    OrderId  active_orders[MAX_ACTIVE_ORDERS]{};
    uint32_t active_write = 0;
    uint32_t active_count = 0;
    uint32_t active_read  = 0;

    //===== stats =====
    uint64_t requests_sent = 0;
    uint64_t responses_ack = 0;
    uint64_t responses_match = 0;
    uint64_t responses_err = 0;
    int64_t orders_in_flight = 0;

};

#endif // CLIENT_STATE_H
