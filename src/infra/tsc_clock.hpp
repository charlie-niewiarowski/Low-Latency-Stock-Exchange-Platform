//
// Created by cniew on 10/18/26.
//

#ifndef TSC_CLOCK_H
#define TSC_CLOCK_H

#include "order_types.hpp"

#include <immintrin.h>
#include <time.h>

#include <chrono>
#include <cstdint>
#include <thread>

//=============================================================================
// TscClock -- wall-clock nanoseconds from rdtsc
//=============================================================================
// OUCH/ITCH timestamps are nanoseconds since midnight. Everything on the hot
// path already takes __rdtsc() for latency stamps, so wall-clock time is
// derived from that same counter rather than paying for clock_gettime()
// (which, with the kernel's tsc clocksource, reads the TSC anyway and adds
// its own scale/offset work on top). Requires an invariant TSC
// (constant_tsc + nonstop_tsc in /proc/cpuinfo), which also makes it
// consistent across cores.
//
// Calibrated once, on first use: the TSC frequency is measured against
// CLOCK_MONOTONIC_RAW over kCalibrationWindow, and one (tsc, CLOCK_REALTIME)
// pair anchors it to wall time. Conversion is then a subtract, a 64x64->128
// multiply and a shift. NTP adjustments after startup aren't picked up, and
// frequency error accumulates as drift (on the order of 1us per second of
// uptime per ppm of error) -- fine for a single-machine simulation, and
// fixable later by periodically re-anchoring if it ever matters.
//
// "Midnight" is local midnight of the process's time zone (Nasdaq's is US
// Eastern); a run spanning midnight wraps back to 0.

class TscClock {
public:
    // Calibrates on the first call (~kCalibrationWindow); call it once at
    // startup so no hot-path caller pays for that.
    static const TscClock &instance() {
        static const TscClock clock;
        return clock;
    }

    static Timestamp now_tsc() noexcept { return __rdtsc(); }

    [[nodiscard]] uint64_t ticks_to_ns(const uint64_t ticks) const noexcept {
        return static_cast<uint64_t>((static_cast<unsigned __int128>(ticks) * ns_per_tick_q32_) >> 32);
    }

    // Nanoseconds since midnight at the moment tsc was read.
    [[nodiscard]] uint64_t ns_since_midnight(const Timestamp tsc) const noexcept {
        const uint64_t elapsed = tsc > anchor_tsc_ ? ticks_to_ns(tsc - anchor_tsc_) : 0;
        uint64_t ns = anchor_ns_since_midnight_ + elapsed;
        if (ns >= kNsPerDay) [[unlikely]] {
            ns %= kNsPerDay;
        }
        return ns;
    }

    [[nodiscard]] uint64_t ns_since_midnight() const noexcept { return ns_since_midnight(now_tsc()); }

    [[nodiscard]] double tsc_hz() const noexcept { return tsc_hz_; }

private:
    static constexpr uint64_t kNsPerSec = 1'000'000'000;
    static constexpr uint64_t kNsPerDay = 86'400 * kNsPerSec;
    static constexpr auto kCalibrationWindow = std::chrono::milliseconds(50);

    struct Sample {
        Timestamp tsc;
        uint64_t  ns;
    };

    static uint64_t to_ns(const timespec &ts) {
        return static_cast<uint64_t>(ts.tv_sec) * kNsPerSec + static_cast<uint64_t>(ts.tv_nsec);
    }

    // Brackets clock_gettime between two rdtscs and keeps the tightest of a
    // few tries, taking the TSC midpoint, so a preemption or cache miss in
    // the middle doesn't skew the pairing.
    static Sample sample(const clockid_t clock_id, timespec *out = nullptr) {
        Sample best{};
        uint64_t best_width = UINT64_MAX;
        for (int i = 0; i < 16; ++i) {
            timespec ts{};
            const Timestamp before = __rdtsc();
            clock_gettime(clock_id, &ts);
            const Timestamp after = __rdtsc();
            if (after - before < best_width) {
                best_width = after - before;
                best = {before + (after - before) / 2, to_ns(ts)};
                if (out != nullptr) *out = ts;
            }
        }
        return best;
    }

    TscClock() {
        const Sample start = sample(CLOCK_MONOTONIC_RAW);
        std::this_thread::sleep_for(kCalibrationWindow);
        const Sample end = sample(CLOCK_MONOTONIC_RAW);

        tsc_hz_ = static_cast<double>(end.tsc - start.tsc) * static_cast<double>(kNsPerSec) /
                  static_cast<double>(end.ns - start.ns);
        ns_per_tick_q32_ = static_cast<uint64_t>(static_cast<double>(kNsPerSec) * 4294967296.0 / tsc_hz_);

        timespec wall{};
        const Sample anchor = sample(CLOCK_REALTIME, &wall);
        tm local{};
        localtime_r(&wall.tv_sec, &local);
        const uint64_t secs_since_midnight =
            static_cast<uint64_t>(local.tm_hour) * 3600 + static_cast<uint64_t>(local.tm_min) * 60 +
            static_cast<uint64_t>(local.tm_sec);

        anchor_tsc_ = anchor.tsc;
        anchor_ns_since_midnight_ = secs_since_midnight * kNsPerSec + static_cast<uint64_t>(wall.tv_nsec);
    }

    Timestamp anchor_tsc_ = 0;
    uint64_t  anchor_ns_since_midnight_ = 0;
    uint64_t  ns_per_tick_q32_ = 0; // ns per TSC tick, 32.32 fixed point
    double    tsc_hz_ = 0;
};

#endif //TSC_CLOCK_H
