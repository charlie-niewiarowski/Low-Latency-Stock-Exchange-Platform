//
// Created by cniew on 10/18/26.
//

#ifndef WIRE_CODEC_H
#define WIRE_CODEC_H

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

//=============================================================================
// Big-endian field codecs shared by ouch_proto.hpp / itch_proto.hpp
//=============================================================================
// OUCH and ITCH are both "every numeric field is a big-endian binary
// integer, every alpha field is raw left-justified bytes, fields sit back to
// back with no padding". Each message type describes its field order exactly
// once (a static core(msg, io) on the struct) and that one description is
// driven by three interchangeable IO objects below: WireSizer (how many bytes
// would this take), WireWriter (host struct -> wire bytes), WireReader (wire
// bytes -> host struct). Keeping a single field list per message is what
// keeps encode, decode and size from drifting apart.
//
// Supported field types: integers and enums (written big-endian at their own
// width), and std::array of single-byte elements (copied verbatim). io.u48()
// covers ITCH's 6-byte timestamp, the one field whose wire width doesn't
// match any C++ integer type.

namespace wire {

template <typename T>
struct is_byte_array : std::false_type {};
template <typename E, size_t N>
struct is_byte_array<std::array<E, N>> : std::bool_constant<sizeof(E) == 1> {};

template <typename T>
concept WireField = std::is_integral_v<T> || std::is_enum_v<T> || is_byte_array<T>::value;

template <typename T>
inline constexpr size_t wire_size_v = sizeof(T); // no padding inside any WireField type

inline constexpr size_t kU48Len = 6;

class WireSizer {
public:
    template <WireField T>
    void operator()(const T &) noexcept { size_ += wire_size_v<T>; }
    void u48(uint64_t) noexcept { size_ += kU48Len; }

    [[nodiscard]] size_t size() const noexcept { return size_; }

private:
    size_t size_ = 0;
};

class WireWriter {
public:
    explicit WireWriter(std::span<std::byte> out) noexcept : out_(out) {}

    template <WireField T>
    void operator()(const T &value) noexcept {
        std::byte *dst = claim(wire_size_v<T>);
        if (dst == nullptr) return;

        if constexpr (is_byte_array<T>::value) {
            std::memcpy(dst, value.data(), value.size());
        } else if constexpr (std::is_enum_v<T>) {
            store(dst, static_cast<std::underlying_type_t<T>>(value));
        } else {
            store(dst, value);
        }
    }

    void u48(uint64_t value) noexcept {
        std::byte *dst = claim(kU48Len);
        if (dst == nullptr) return;
        for (size_t i = 0; i < kU48Len; ++i) {
            dst[i] = static_cast<std::byte>(value >> (8 * (kU48Len - 1 - i)));
        }
    }

    // Reserves n bytes to be filled in later (e.g. a length prefix whose
    // value isn't known until what follows it has been written).
    std::byte *skip(size_t n) noexcept { return claim(n); }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] size_t written() const noexcept { return pos_; }

    template <std::integral T>
    static void store(std::byte *dst, T value) noexcept {
        if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        std::memcpy(dst, &value, sizeof(T));
    }

private:
    std::byte *claim(size_t n) noexcept {
        if (!ok_ || out_.size() - pos_ < n) {
            ok_ = false;
            return nullptr;
        }
        std::byte *dst = out_.data() + pos_;
        pos_ += n;
        return dst;
    }

    std::span<std::byte> out_;
    size_t pos_ = 0;
    bool ok_ = true;
};

class WireReader {
public:
    explicit WireReader(std::span<const std::byte> in) noexcept : in_(in) {}

    template <WireField T>
    void operator()(T &value) noexcept {
        const std::byte *src = claim(wire_size_v<T>);
        if (src == nullptr) return;

        if constexpr (is_byte_array<T>::value) {
            std::memcpy(value.data(), src, value.size());
        } else if constexpr (std::is_enum_v<T>) {
            value = static_cast<T>(load<std::underlying_type_t<T>>(src));
        } else {
            value = load<T>(src);
        }
    }

    void u48(uint64_t &value) noexcept {
        const std::byte *src = claim(kU48Len);
        if (src == nullptr) return;
        value = 0;
        for (size_t i = 0; i < kU48Len; ++i) {
            value = (value << 8) | static_cast<uint8_t>(src[i]);
        }
    }

    // Hands back the next n bytes as a view and steps past them.
    std::span<const std::byte> take(size_t n) noexcept {
        const std::byte *src = claim(n);
        return src == nullptr ? std::span<const std::byte>{} : std::span<const std::byte>{src, n};
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] size_t remaining() const noexcept { return in_.size() - pos_; }

    template <std::integral T>
    static T load(const std::byte *src) noexcept {
        T value;
        std::memcpy(&value, src, sizeof(T));
        if constexpr (sizeof(T) > 1 && std::endian::native == std::endian::little) {
            value = std::byteswap(value);
        }
        return value;
    }

private:
    const std::byte *claim(size_t n) noexcept {
        if (!ok_ || in_.size() - pos_ < n) {
            ok_ = false;
            return nullptr;
        }
        const std::byte *src = in_.data() + pos_;
        pos_ += n;
        return src;
    }

    std::span<const std::byte> in_;
    size_t pos_ = 0;
    bool ok_ = true;
};

} // namespace wire

#endif //WIRE_CODEC_H
