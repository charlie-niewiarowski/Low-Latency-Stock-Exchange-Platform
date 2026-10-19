//
// Created by cniew on 10/18/26.
//
// SessionLog: the per-session replay ring behind SoupBinTCP re-login.
//

#include "session_log.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

std::vector<std::byte> Packet(const size_t len, const char fill) {
    return std::vector<std::byte>(len, static_cast<std::byte>(fill));
}

// Everything from offset to the end, across the wrap.
std::string ReadFrom(const SessionLog &log, uint64_t offset) {
    std::string out;
    while (offset < log.end_offset()) {
        const auto bytes = log.contiguous_from(offset);
        out.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        offset += bytes.size();
    }
    return out;
}

} // namespace

TEST(SessionLogTest, StartsEmptyAtSequenceOne) {
    const SessionLog log(1024);
    EXPECT_EQ(log.first_sequence(), 1u);
    EXPECT_EQ(log.next_sequence(), 1u);
    EXPECT_EQ(log.end_offset(), 0u);
    EXPECT_EQ(log.offset_of(1), 0u);   // the next new message
    EXPECT_FALSE(log.offset_of(2));     // never sent
}

TEST(SessionLogTest, NumbersMessagesAndFindsEachOne) {
    SessionLog log(1024);
    log.append(Packet(10, 'a'));
    log.append(Packet(20, 'b'));
    log.append(Packet(5, 'c'));

    EXPECT_EQ(log.next_sequence(), 4u);
    EXPECT_EQ(log.offset_of(1), 0u);
    EXPECT_EQ(log.offset_of(2), 10u);
    EXPECT_EQ(log.offset_of(3), 30u);
    EXPECT_EQ(log.offset_of(4), 35u);
    EXPECT_EQ(ReadFrom(log, *log.offset_of(2)), std::string(20, 'b') + std::string(5, 'c'));
}

TEST(SessionLogTest, DropsTheOldestToMakeRoom) {
    SessionLog log(256);
    for (char c = 'a'; c <= 'h'; ++c) log.append(Packet(40, c)); // 320 bytes into 256

    // Only the last six whole messages fit.
    EXPECT_EQ(log.first_sequence(), 3u);
    EXPECT_FALSE(log.offset_of(2));
    EXPECT_EQ(log.offset_of(3), 80u);
    std::string expected;
    for (char c = 'c'; c <= 'h'; ++c) expected += std::string(40, c);
    EXPECT_EQ(ReadFrom(log, *log.offset_of(3)), expected);
}

TEST(SessionLogTest, ReadsAcrossTheWrap) {
    SessionLog log(64);
    log.append(Packet(50, 'a'));
    log.append(Packet(30, 'b')); // straddles the end of the ring

    ASSERT_EQ(log.first_sequence(), 2u);
    const uint64_t offset = *log.offset_of(2);
    EXPECT_EQ(log.contiguous_from(offset).size(), 14u); // up to the wrap
    EXPECT_EQ(ReadFrom(log, offset), std::string(30, 'b'));
}

TEST(SessionLogTest, IndexBindsForTinyMessages) {
    SessionLog log(64); // 64 / 32 = 2 index entries
    log.append(Packet(1, 'a'));
    log.append(Packet(1, 'b'));
    log.append(Packet(1, 'c'));
    EXPECT_EQ(log.first_sequence(), 2u);
    EXPECT_EQ(ReadFrom(log, *log.offset_of(2)), "bc");
}

TEST(SessionLogTest, ReportsWhenAnAppendWouldOverwriteUnsentBytes) {
    SessionLog log(64);
    log.append(Packet(40, 'a'));
    EXPECT_FALSE(log.would_overwrite(0, 24));  // exactly fills the ring
    EXPECT_TRUE(log.would_overwrite(0, 25));
    EXPECT_FALSE(log.would_overwrite(40, 64)); // everything sent already
}
