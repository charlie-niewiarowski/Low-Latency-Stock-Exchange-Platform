//
// Created by charl on 1/13/2026.
//

#ifndef TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP
#define TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP

#include "protocol.hpp"
#include "socket.hpp"
#include "buffer.hpp"
#include "ring_buffer.hpp"
#include "latency.hpp"
#include "config.hpp"

#include <array>

//=============================================================================
// Buffer aliases
//=============================================================================

using ReadBuffer = Buffer<PIPELINE_DEPTH * INBOUND_BSIZE>;
using WriteBuffer = Buffer<PIPELINE_DEPTH * OUTBOUND_BSIZE>;
using StagingQueue = RingBuffer<OutboundMessage>;

//=============================================================================
// PendingError  (SPSC channel element: inbound thread → outbound thread)
//=============================================================================

struct PendingError {
    int fd;
    ServerError err;
};

//=============================================================================
// InboundState  (owned exclusively by the inbound thread)
//=============================================================================
//
//   Lifetime: created in registerConnection; reset in handleDisconnectsInbound
//   only after condemned_outbound_[fd] has been set (two-phase close).
//
//   The outbound thread must never access this object.

class InboundState {
public:
    explicit InboundState(EpollSocket sock) : sock_(std::move(sock)) {}

    InboundState(const InboundState&) = delete;
    InboundState& operator=(const InboundState&) = delete;
    InboundState(InboundState&&) = delete;
    InboundState& operator=(InboundState&&) = delete;

    ~InboundState() = default;

    [[nodiscard]] int fd() const { return sock_.get(); }

    ReadBuffer& read_buffer() { return read_buf_; }

    [[nodiscard]] const InboundMessage& inbound() const { return inbound_; }
    void set_inbound(const InboundMessage& m) { inbound_ = m; }

    [[nodiscard]] ClientId client_id() const { return inbound_.client_id; }

    void set_read_begin_tsc(Timestamp t) { read_begin_tsc_ = t; }
    [[nodiscard]] Timestamp read_begin_tsc() const { return read_begin_tsc_; }

private:
    const EpollSocket sock_;
    ReadBuffer read_buf_;
    InboundMessage inbound_{};
    Timestamp read_begin_tsc_{};
};

//=============================================================================
// OutboundState  (owned exclusively by the outbound thread)
//=============================================================================
//
//   Lifetime: created in registerConnection by inbound (safe: the happens-
//   before chain through the engine rings guarantees outbound sees it before
//   it first accesses the slot); reset by outbound in handleDisconnectsOutbound.
//
//   The inbound thread must never access this object after construction.

class OutboundState {
public:
    OutboundState() = default;

    OutboundState(const OutboundState&) = delete;
    OutboundState& operator=(const OutboundState&) = delete;
    OutboundState(OutboundState&&) = delete;
    OutboundState& operator=(OutboundState&&) = delete;

    ~OutboundState() = default;

    WriteBuffer& write_buffer() { return write_buf_; }

    void push_outbound(OutboundMessage msg) {
        if (staging_.push(msg)) ++pending_count_;
    }
    OutboundMessage pop_outbound();
    [[nodiscard]] bool has_pending_outbound() const { return pending_count_ > 0; }

    void push_latency(const PendingLatency& e) {
        if (latency_count_ < static_cast<int>(latency_batch_.size()))
            latency_batch_[latency_count_++] = e;
    }
    [[nodiscard]] int latency_count() const { return latency_count_; }
    [[nodiscard]] const PendingLatency& latency_entry(int i) const { return latency_batch_[i]; }
    void clear_latency_batch() { latency_count_ = 0; }

private:
    WriteBuffer write_buf_;
    // Single-threaded staging buffer; StagingQueue's atomics have no contention.
    StagingQueue staging_{RINGBUF_SIZE};
    // RingBuffer no longer exposes empty()/size(); track pending count ourselves.
    int pending_count_{0};

    std::array<PendingLatency, PIPELINE_DEPTH> latency_batch_{};
    int latency_count_{0};
};

#endif //TOYEXCHANGE_ORDER_GATEWAY_TYPES_HPP