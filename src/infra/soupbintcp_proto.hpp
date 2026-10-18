//
// Created by cniew on 10/18/26.
//

#ifndef SOUPBINTCP_PROTO_H
#define SOUPBINTCP_PROTO_H

#include "ouch_proto.hpp"
#include "wire_codec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

//=============================================================================
// SoupBinTCP 3.00 (session layer that OUCH rides on)
//=============================================================================
// Every logical packet is a 2-byte big-endian length (counting everything
// after it: the type byte plus the payload), a 1-byte packet type, and the
// payload. Logical packets don't line up with TCP segments -- the stream
// may split or coalesce them -- so readers go through parse_soup_packet,
// which says whether a whole packet is available yet.
//
// Flow: the client opens with Login Request; the server answers Login
// Accepted (then Sequenced Data, one higher-level message each, numbered
// implicitly from the sequence number in Login Accepted) or Login Rejected.
// After login the client sends Unsequenced Data. Each side sends a
// heartbeat whenever it has sent nothing for more than kSoupHeartbeatInterval.
//
// Numeric fields are ASCII decimal, left-padded with spaces. The session id
// in Login Accepted is left-padded too; username/password are right-padded.

inline constexpr size_t kSoupHeaderLen = 3; // length(2) + type(1)
inline constexpr uint64_t kSoupHeartbeatIntervalNs = 1'000'000'000;

enum class SoupPacketType : char {
    // either direction
    DEBUG            = '+',
    // server -> client
    LOGIN_ACCEPTED   = 'A',
    LOGIN_REJECTED   = 'J',
    SEQUENCED_DATA   = 'S',
    SERVER_HEARTBEAT = 'H',
    END_OF_SESSION   = 'Z',
    // client -> server
    LOGIN_REQUEST    = 'L',
    UNSEQUENCED_DATA = 'U',
    CLIENT_HEARTBEAT = 'R',
    LOGOUT_REQUEST   = 'O',
};

// Login Rejected reason codes.
namespace soup_reject {
    inline constexpr char kNotAuthorized       = 'A';
    inline constexpr char kSessionNotAvailable = 'S';
} // namespace soup_reject

using SoupUsername  = std::array<char, 6>;
using SoupPassword  = std::array<char, 10>;
using SoupSessionId = std::array<char, 10>;

struct SoupLoginRequest {
    SoupUsername  username;
    SoupPassword  password;
    SoupSessionId requested_session;          // all blanks: the currently active session
    uint64_t      requested_sequence_number;  // next one wanted; 0 = most recent
};

struct SoupLoginAccepted {
    SoupSessionId session;
    uint64_t      sequence_number; // of the next Sequenced Data packet
};

inline constexpr size_t kSoupLoginRequestLen  = 6 + 10 + 10 + 20;
inline constexpr size_t kSoupLoginAcceptedLen = 10 + 20;
inline constexpr size_t kSoupNumericLen       = 20;

// Session ids are right-justified on the wire (unlike username/password).
inline SoupSessionId soup_session_id(const std::string_view id) noexcept {
    SoupSessionId out;
    out.fill(' ');
    const size_t n = std::min(id.size(), out.size());
    for (size_t i = 0; i < n; ++i) out[out.size() - n + i] = id[i];
    return out;
}

//=============================================================================
// Encoding
//=============================================================================

namespace soup_detail {

// Writes the 3-byte header for a packet with payload_len bytes of payload.
inline bool write_header(const SoupPacketType type, const size_t payload_len, std::span<std::byte> out) noexcept {
    if (out.size() < kSoupHeaderLen + payload_len || payload_len + 1 > UINT16_MAX) return false;
    wire::WireWriter writer{out};
    writer(static_cast<uint16_t>(payload_len + 1));
    writer(type);
    return writer.ok();
}

inline void write_numeric(uint64_t value, std::span<std::byte> out) noexcept {
    for (size_t i = out.size(); i-- > 0;) {
        if (value == 0 && i + 1 < out.size()) {
            out[i] = std::byte{' '};
            continue;
        }
        out[i] = static_cast<std::byte>('0' + value % 10);
        value /= 10;
    }
}

inline std::optional<uint64_t> read_numeric(const std::span<const std::byte> in) noexcept {
    size_t i = 0;
    while (i < in.size() && in[i] == std::byte{' '}) ++i;
    if (i == in.size()) return std::nullopt;

    uint64_t value = 0;
    for (; i < in.size() && in[i] != std::byte{' '}; ++i) {
        const auto c = static_cast<char>(in[i]);
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    for (; i < in.size(); ++i) {
        if (in[i] != std::byte{' '}) return std::nullopt; // trailing spaces only
    }
    return value;
}

template <size_t N>
void write_alpha(const std::array<char, N> &value, std::span<std::byte> out) noexcept {
    std::memcpy(out.data(), value.data(), N);
}

template <size_t N>
std::array<char, N> read_alpha(const std::span<const std::byte> in) noexcept {
    std::array<char, N> out{};
    std::memcpy(out.data(), in.data(), N);
    return out;
}

} // namespace soup_detail

// Each encoder returns bytes written, or 0 if out is too small.

// Heartbeats, Logout Request, End of Session: header only.
inline size_t encode_soup_empty(const SoupPacketType type, std::span<std::byte> out) noexcept {
    return soup_detail::write_header(type, 0, out) ? kSoupHeaderLen : 0;
}

inline size_t encode_soup_login_request(const SoupLoginRequest &login, std::span<std::byte> out) noexcept {
    if (!soup_detail::write_header(SoupPacketType::LOGIN_REQUEST, kSoupLoginRequestLen, out)) return 0;
    std::span<std::byte> payload = out.subspan(kSoupHeaderLen, kSoupLoginRequestLen);
    soup_detail::write_alpha(login.username, payload.subspan(0, 6));
    soup_detail::write_alpha(login.password, payload.subspan(6, 10));
    soup_detail::write_alpha(login.requested_session, payload.subspan(16, 10));
    soup_detail::write_numeric(login.requested_sequence_number, payload.subspan(26, kSoupNumericLen));
    return kSoupHeaderLen + kSoupLoginRequestLen;
}

inline size_t encode_soup_login_accepted(const SoupLoginAccepted &accepted, std::span<std::byte> out) noexcept {
    if (!soup_detail::write_header(SoupPacketType::LOGIN_ACCEPTED, kSoupLoginAcceptedLen, out)) return 0;
    std::span<std::byte> payload = out.subspan(kSoupHeaderLen, kSoupLoginAcceptedLen);
    soup_detail::write_alpha(accepted.session, payload.subspan(0, 10));
    soup_detail::write_numeric(accepted.sequence_number, payload.subspan(10, kSoupNumericLen));
    return kSoupHeaderLen + kSoupLoginAcceptedLen;
}

inline size_t encode_soup_login_rejected(const char reason, std::span<std::byte> out) noexcept {
    if (!soup_detail::write_header(SoupPacketType::LOGIN_REJECTED, 1, out)) return 0;
    out[kSoupHeaderLen] = static_cast<std::byte>(reason);
    return kSoupHeaderLen + 1;
}

// OUCH client -> exchange, in an Unsequenced Data packet.
inline size_t encode_soup_unsequenced(const OuchInboundFrame &msg, std::span<std::byte> out) noexcept {
    const size_t len = encoded_size(msg);
    if (len == 0 || !soup_detail::write_header(SoupPacketType::UNSEQUENCED_DATA, len, out)) return 0;
    return encode_inbound_ouch(msg, out.subspan(kSoupHeaderLen, len)) == len ? kSoupHeaderLen + len : 0;
}

// OUCH exchange -> client, in a Sequenced Data packet.
inline size_t encode_soup_sequenced(const OuchOutboundFrame &msg, std::span<std::byte> out) noexcept {
    const size_t len = encoded_size(msg);
    if (len == 0 || !soup_detail::write_header(SoupPacketType::SEQUENCED_DATA, len, out)) return 0;
    return encode_outbound_ouch(msg, out.subspan(kSoupHeaderLen, len)) == len ? kSoupHeaderLen + len : 0;
}

//=============================================================================
// Decoding
//=============================================================================

struct SoupPacket {
    SoupPacketType             type;
    std::span<const std::byte> payload;
};

enum class SoupParseStatus : uint8_t {
    PACKET,      // packet is the next logical packet; consumed bytes belong to it
    INCOMPLETE,  // not all of the next packet has arrived yet
    MALFORMED,   // a zero length -- the stream can't be resynchronized
};

struct SoupParseResult {
    SoupParseStatus status;
    SoupPacket      packet{};
    size_t          consumed = 0;
};

// Looks at the front of a byte stream for one whole logical packet.
inline SoupParseResult parse_soup_packet(const std::span<const std::byte> stream) noexcept {
    if (stream.size() < 2) return {SoupParseStatus::INCOMPLETE};

    const auto length = wire::WireReader::load<uint16_t>(stream.data());
    if (length == 0) return {SoupParseStatus::MALFORMED};
    if (stream.size() < size_t{2} + length) return {SoupParseStatus::INCOMPLETE};

    return {
        .status   = SoupParseStatus::PACKET,
        .packet   = {.type = static_cast<SoupPacketType>(stream[2]), .payload = stream.subspan(kSoupHeaderLen, length - 1u)},
        .consumed = size_t{2} + length,
    };
}

inline std::optional<SoupLoginRequest> decode_soup_login_request(const std::span<const std::byte> payload) noexcept {
    if (payload.size() != kSoupLoginRequestLen) return std::nullopt;
    const std::optional<uint64_t> sequence = soup_detail::read_numeric(payload.subspan(26, kSoupNumericLen));
    if (!sequence) return std::nullopt;
    return SoupLoginRequest{
        .username                  = soup_detail::read_alpha<6>(payload.subspan(0, 6)),
        .password                  = soup_detail::read_alpha<10>(payload.subspan(6, 10)),
        .requested_session         = soup_detail::read_alpha<10>(payload.subspan(16, 10)),
        .requested_sequence_number = *sequence,
    };
}

inline std::optional<SoupLoginAccepted> decode_soup_login_accepted(const std::span<const std::byte> payload) noexcept {
    if (payload.size() != kSoupLoginAcceptedLen) return std::nullopt;
    const std::optional<uint64_t> sequence = soup_detail::read_numeric(payload.subspan(10, kSoupNumericLen));
    if (!sequence) return std::nullopt;
    return SoupLoginAccepted{
        .session         = soup_detail::read_alpha<10>(payload.subspan(0, 10)),
        .sequence_number = *sequence,
    };
}

#endif //SOUPBINTCP_PROTO_H
