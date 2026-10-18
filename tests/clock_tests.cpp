//
// Created by cniew on 10/18/26.
//

#include "tsc_clock.hpp"

#include <time.h>

#include <chrono>
#include <cstdint>
#include <thread>

#include <gtest/gtest.h>

namespace {

uint64_t WallNsSinceMidnight() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    localtime_r(&ts.tv_sec, &local);
    return (static_cast<uint64_t>(local.tm_hour) * 3600 + static_cast<uint64_t>(local.tm_min) * 60 +
            static_cast<uint64_t>(local.tm_sec)) * 1'000'000'000 + static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

TEST(TscClock, TracksWallClock) {
    const TscClock &clock = TscClock::instance();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const uint64_t tsc_ns = clock.ns_since_midnight();
    const uint64_t wall_ns = WallNsSinceMidnight();

    // Same instant to within 100us (two reads, a few ns apart, plus
    // whatever calibration error accumulated over ~20ms).
    const int64_t diff = static_cast<int64_t>(tsc_ns) - static_cast<int64_t>(wall_ns);
    EXPECT_LT(std::abs(diff), 100'000) << "tsc=" << tsc_ns << " wall=" << wall_ns;
}

TEST(TscClock, MeasuresElapsedTime) {
    const TscClock &clock = TscClock::instance();
    const Timestamp start = TscClock::now_tsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const uint64_t elapsed_ns = clock.ticks_to_ns(TscClock::now_tsc() - start);

    EXPECT_GE(elapsed_ns, 10'000'000u);
    EXPECT_LT(elapsed_ns, 15'000'000u);
}

TEST(TscClock, IsMonotonic) {
    const TscClock &clock = TscClock::instance();
    uint64_t prev = clock.ns_since_midnight();
    for (int i = 0; i < 100'000; ++i) {
        const uint64_t now = clock.ns_since_midnight();
        ASSERT_GE(now, prev);
        prev = now;
    }
}

TEST(TscClock, StampBeforeAnchorClampsToAnchor) {
    const TscClock &clock = TscClock::instance();
    EXPECT_EQ(clock.ns_since_midnight(0), clock.ns_since_midnight(1));
}
