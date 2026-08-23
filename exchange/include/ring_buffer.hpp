//
// Created by cniew on 5/16/26.
//

#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <memory>
#include <atomic>
#include <cassert>

#include "communication_types.hpp"

template <typename T>
class RingBuffer {
    // Producer-owned line: write_idx_ is the only field the producer writes,
    // and cached_read_idx_ is a plain (non-atomic) local copy of read_idx_ the
    // producer uses to avoid an acquire load — and the cross-core cache-line
    // fetch it costs — on every single push(). It's only refreshed from the
    // real read_idx_ when the cache says the ring might be full, so in the
    // common (non-full) case push() never touches the consumer's cache line.
    alignas(64) std::atomic<size_t> write_idx_{0};
    size_t cached_read_idx_{0};

    // Consumer-owned line: mirror of the above for pop().
    alignas(64) std::atomic<size_t> read_idx_{0};
    size_t cached_write_idx_{0};

    // Read-only after construction and shared by both sides — never written
    // again, so no coherency cost from sharing one line between them.
    alignas(64) std::unique_ptr<T[]> buffer;
    uint32_t mask;
public:
    explicit RingBuffer(uint32_t N) : // N must be a power of two because of mask
        buffer(std::make_unique<T[]>(N)),
        mask(N - 1)
    {
        assert((N & (N - 1)) == 0);
    }

    [[nodiscard]] bool push(const T& item);
    [[nodiscard]] bool push(T&& item);
    [[nodiscard]] bool pop(T& item);

    // Point-in-time occupancy snapshot. Exact when called from the single
    // thread that owns both ends of this ring (e.g. the per-connection
    // StagingQueue, pushed and popped only from the outbound thread) —
    // there's no second thread for the value to race against. Only an
    // *approximation* if read from a thread other than this ring's
    // producer/consumer pair (e.g. polling a cross-thread SPSC ring like
    // in_ring_/out_ring_ from a third thread), since the owning side can
    // move the instant after the read returns. Nothing in this codebase
    // currently does the latter.
    [[nodiscard]] size_t size() const;
    [[nodiscard]] bool empty() const;
};

template<typename T>
bool RingBuffer<T>::push(const T& item) {
    const size_t write_idx = write_idx_.load(std::memory_order_relaxed);
    const size_t next_write = (write_idx + 1) & mask;

    if (next_write == cached_read_idx_) [[unlikely]] {
        cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
        if (next_write == cached_read_idx_) return false;
    }

    buffer[write_idx] = item;
    write_idx_.store(next_write, std::memory_order_release);
    return true;
}

template<typename T>
bool RingBuffer<T>::push(T&& item) {
    const size_t write_idx = write_idx_.load(std::memory_order_relaxed);
    const size_t next_write = (write_idx + 1) & mask;

    if (next_write == cached_read_idx_) [[unlikely]] {
        cached_read_idx_ = read_idx_.load(std::memory_order_acquire);
        if (next_write == cached_read_idx_) return false;
    }

    buffer[write_idx] = std::move(item);
    write_idx_.store(next_write, std::memory_order_release);
    return true;
}

template<typename T>
bool RingBuffer<T>::pop(T &item) {
    const size_t read_idx = read_idx_.load(std::memory_order_relaxed);

    if (read_idx == cached_write_idx_) [[unlikely]] {
        cached_write_idx_ = write_idx_.load(std::memory_order_acquire);
        if (read_idx == cached_write_idx_) return false;
    }

    item = std::move(buffer[read_idx]);

    const size_t next_read = (read_idx + 1) & mask;
    read_idx_.store(next_read, std::memory_order_release);
    return true;
}

template<typename T>
size_t RingBuffer<T>::size() const {
    const size_t write_idx = write_idx_.load(std::memory_order_acquire);
    const size_t read_idx = read_idx_.load(std::memory_order_acquire);

    if (write_idx >= read_idx) return write_idx - read_idx;
    return (mask + 1) + (write_idx - read_idx);
}

template<typename T>
bool RingBuffer<T>::empty() const {
    return write_idx_.load(std::memory_order_acquire) == read_idx_.load(std::memory_order_acquire);
}

using InboundRing = RingBuffer<InboundMessage>;
using OutboundRing = RingBuffer<OutboundMessage>;

#endif //RINGBUFFER_H
