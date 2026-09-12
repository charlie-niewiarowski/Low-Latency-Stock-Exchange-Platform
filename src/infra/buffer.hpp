//
// Created by cniew on 5/16/26.
//

// Header-only fixed-capacity byte buffer.  The template parameter N is the
// capacity in bytes; both the read and write buffers in ConnectionInfo use
// this template with different N values.

// Read path (inbound):
//   read_from() -> view() / advance()

// Write path (outbound):
//   append()  - copies a serialised frame into the tail of the buffer
//   write_to() - drains bytes starting at bytes_written_
//   try_reset() - resets indices once all bytes have been sent

#ifndef BUFFER_H
#define BUFFER_H

#include <cstring>
#include <string_view>
#include <sys/socket.h>

template <size_t N>
class Buffer {
public:
    Buffer() = default;

    //===== read path =====
    [[nodiscard]] size_t length() const { return len_ - read_off_; }
    [[nodiscard]] size_t remaining_capacity() const { return N - len_; }
    [[nodiscard]] std::string_view view() const { return {data_ + read_off_, len_ - read_off_}; }

    ssize_t read_from(const int fd) {
        // Compact only when the physical tail is full but the logical front has been consumed.
        // Moves at most (INBOUND_BSIZE - 1) leftover bytes — a partial message tail.
        if (read_off_ > 0 && len_ == N) {
            const size_t remaining = len_ - read_off_;
            if (remaining > 0)
                std::memmove(data_, data_ + read_off_, remaining);
            len_ = remaining;
            read_off_ = 0;
        }
        ssize_t n = recv(fd, data_ + len_, N - len_, 0);
        if (n > 0) len_ += static_cast<size_t>(n);
        return n;
    }

    void advance(const size_t n) {
        read_off_ += n;
        if (read_off_ >= len_) { read_off_ = 0; len_ = 0; }
    }

    //===== write path =====
    [[nodiscard]] size_t bytes_written() const { return bytes_written_; }
    [[nodiscard]] size_t remaining_to_send() const { return len_ - bytes_written_; }
    [[nodiscard]] bool has_room(size_t n) const { return len_ + n <= N; }

    bool append(const char* src, const size_t n) {
        if (!has_room(n)) return false;
        std::memcpy(data_ + len_, src, n);
        len_ += n;
        return true;
    }

    ssize_t write_to(const int fd) {
        ssize_t n = send(fd, data_ + bytes_written_, len_ - bytes_written_, MSG_NOSIGNAL);
        if (n > 0) bytes_written_ += static_cast<size_t>(n);
        return n;
    }

    void try_reset() {
        if (bytes_written_ == len_) {
            len_ = 0;
            bytes_written_ = 0;
        }
    }

    //===== utils =====
    void clear() { len_ = 0; bytes_written_ = 0; read_off_ = 0; }

private:
    char data_[N]{};
    size_t len_{0};
    size_t bytes_written_{0};
    size_t read_off_{0};
};

#endif //BUFFER_H
