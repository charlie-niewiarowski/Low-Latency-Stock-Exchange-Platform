//
// Created by cniew on 10/18/26.
//
// SessionLog -- one SoupBinTCP session's Sequenced Data packets, kept for
// re-login replay.
//
// SoupBinTCP promises a client every sequenced message in order across
// connection failures: a client that reconnects asks for its next expected
// sequence number and the server resends from there. Real venues keep the
// whole day in an on-disk journal. Here every session keeps a fixed-size
// in-memory ring of the packets exactly as they went out, so replay covers
// a short outage (seconds at full load) at bounded memory, and the oldest
// messages fall off as new ones arrive. A request for one that has fallen
// off is answered with Login Rejected (see Server).
//
// The ring is also the connection's send buffer: the outbound thread
// appends a packet here and then sends from a byte cursor into the ring,
// so a peer whose TCP window is full costs no extra copy and never blocks
// the thread -- the cursor just catches up later.
//
// Offsets are absolute byte positions in the session's stream (they never
// wrap); the ring holds the last capacity() bytes of it. Sequence numbers
// start at 1, as SoupBinTCP's do. Outbound-thread only.
//

#ifndef SESSION_LOG_H
#define SESSION_LOG_H

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

class SessionLog {
public:
    // capacity_bytes must be a power of two, at least kBytesPerIndexEntry. Each retained message also
    // takes an index entry; the index holds capacity_bytes / kBytesPerIndexEntry
    // of them, which only binds for messages smaller than that.
    explicit SessionLog(const size_t capacity_bytes)
        : capacity_(capacity_bytes),
          index_capacity_(capacity_bytes / kBytesPerIndexEntry),
          bytes_(std::make_unique_for_overwrite<std::byte[]>(capacity_bytes)),
          index_(std::make_unique_for_overwrite<uint64_t[]>(index_capacity_)) {
        assert(std::has_single_bit(capacity_bytes) && index_capacity_ > 0);
        // Touch every page now rather than taking the faults one at a time
        // on the hot path.
        std::memset(bytes_.get(), 0, capacity_);
        std::memset(index_.get(), 0, index_capacity_ * sizeof(uint64_t));
    }

    SessionLog(const SessionLog &) = delete;
    SessionLog &operator=(const SessionLog &) = delete;

    static constexpr size_t kBytesPerIndexEntry = 32;

    [[nodiscard]] size_t capacity() const { return capacity_; }

    // The sequence number the next append gets, and the oldest one still held
    // (== next_sequence() when nothing is).
    [[nodiscard]] uint64_t next_sequence() const { return next_seq_; }
    [[nodiscard]] uint64_t first_sequence() const { return first_seq_; }

    // Absolute offset one past the last byte appended.
    [[nodiscard]] uint64_t end_offset() const { return end_; }

    // Where message seq starts, if it's still held; next_sequence() maps to
    // end_offset() (nothing to replay, start with the next new message).
    [[nodiscard]] std::optional<uint64_t> offset_of(const uint64_t seq) const {
        if (seq < first_seq_ || seq > next_seq_) return std::nullopt;
        if (seq == next_seq_) return end_;
        return index_[seq & (index_capacity_ - 1)];
    }

    // Whether appending len more bytes would overwrite anything at or after
    // offset -- i.e. bytes a connection hasn't been sent yet.
    [[nodiscard]] bool would_overwrite(const uint64_t offset, const size_t len) const {
        return end_ + len - offset > capacity_;
    }

    // Appends one whole packet as the next sequence number, dropping the
    // oldest messages to make room. packet must be smaller than capacity().
    void append(const std::span<const std::byte> packet) {
        assert(packet.size() < capacity_);
        while (first_seq_ < next_seq_ &&
               (end_ + packet.size() - index_[first_seq_ & (index_capacity_ - 1)] > capacity_ ||
                next_seq_ - first_seq_ >= index_capacity_)) {
            ++first_seq_;
        }

        index_[next_seq_ & (index_capacity_ - 1)] = end_;
        const size_t at = end_ & (capacity_ - 1);
        const size_t first = std::min(packet.size(), capacity_ - at);
        std::memcpy(bytes_.get() + at, packet.data(), first);
        std::memcpy(bytes_.get(), packet.data() + first, packet.size() - first);

        end_ += packet.size();
        ++next_seq_;
    }

    // The bytes from offset onwards that sit contiguously in the ring: up to
    // end_offset(), or to the ring's wrap point (call again from there for
    // the rest). offset must still be held: offset_of(first_sequence()) <=
    // offset <= end_offset().
    [[nodiscard]] std::span<const std::byte> contiguous_from(const uint64_t offset) const {
        assert(offset <= end_ && end_ - offset <= capacity_);
        const size_t at = offset & (capacity_ - 1);
        const size_t len = std::min<uint64_t>(end_ - offset, capacity_ - at);
        return {bytes_.get() + at, len};
    }

private:
    size_t capacity_;
    size_t index_capacity_;
    std::unique_ptr<std::byte[]> bytes_;
    std::unique_ptr<uint64_t[]> index_; // seq -> its start offset, by seq & (index_capacity_ - 1)

    uint64_t first_seq_ = 1;
    uint64_t next_seq_ = 1;
    uint64_t end_ = 0;
};

#endif // SESSION_LOG_H
