//
// Created by cniew on 5/24/26.
//

#ifndef LATENCY_H
#define LATENCY_H

#include "config.hpp"
#include "order_types.hpp"
#include "tsc_clock.hpp"

#include <hdr/hdr_histogram.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

//=============================================================================
// Request latency through the exchange, t0..t7
//=============================================================================
// One sample per request, taken when the outbound thread hands the request's
// answer -- the first event the engine produced for it (EngineEvent::
// answers_request: ACCEPTED for an Enter, REPLACED or CANCELED for a Replace,
// CANCELED for a Cancel) -- to the NIC. Fills, IOC remainders and the
// counterparty's executions aren't sampled (they're fan-out of a request
// already counted), nor are gateway rejects (they never reach the engine) or
// engine rejects (an error path, not the latency being measured).
//
// Every stage boundary is a raw rdtsc, converted with TscClock:
//   t0  inbound: before the rx burst poll that returned the request's packet
//   t1  inbound: rx burst returned
//   t2  inbound: request pushed onto the engine ring
//   t3  engine:  request popped
//   t4  engine:  answering event pushed (an Enter is answered by ACCEPTED,
//                which goes out before matching -- so t3 -> t4 is time to
//                acknowledge, not the whole match)
//   t5  outbound: event popped
//   t6  outbound: tx burst holding the reply begins
//   t7  outbound: that tx burst has been fully handed to the NIC
//
// Recorded straight into HDR histograms as replies go out (no sample buffer,
// so a run of any length is fully covered at fixed memory), after skipping
// the first LATENCY_SAMPLE_DROP samples as warm-up. The end-to-end histogram
// is always kept; the seven per-stage ones only with DIAGNOSTICS. Printed when
// the handler is destroyed -- at exchange shutdown -- in the format
// bench/scripts/benchlib.py parses (section titles contain " -> ").

struct LatencySample {
    Timestamp t0, t1, t2, t3, t4, t5, t6, t7; // see above
};

// What the outbound thread knows about a sample before its reply is sent
// (t6/t7 come from the tx burst that carries it).
struct PendingLatency {
    Timestamp t0, t1, t2, t3, t4, t5;
};

class LatencyHandler {
public:
    LatencyHandler() : clock_(TscClock::instance()) {
        for (hdr_histogram *&h : histograms_) {
            hdr_init(1, kMaxTrackableNs, 3, &h);
        }
    }

    LatencyHandler(const LatencyHandler &) = delete;
    LatencyHandler &operator=(const LatencyHandler &) = delete;

    ~LatencyHandler() {
        print();
        for (hdr_histogram *h : histograms_) {
            hdr_close(h);
        }
    }

    // Outbound thread only.
    void record(const LatencySample &s) {
        const uint64_t seen = seen_.load(std::memory_order_relaxed) + 1;
        seen_.store(seen, std::memory_order_relaxed);
        if (seen <= LATENCY_SAMPLE_DROP) return;
        recorded_.store(recorded_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

        record_(kEndToEnd, s.t0, s.t7);
#if DIAGNOSTICS
        record_(kRxBurst,        s.t0, s.t1);
        record_(kInboundThread,  s.t1, s.t2);
        record_(kInboundRing,    s.t2, s.t3);
        record_(kEngine,         s.t3, s.t4);
        record_(kOutboundRing,   s.t4, s.t5);
        record_(kOutboundThread, s.t5, s.t6);
        record_(kTxBurst,        s.t6, s.t7);
#endif
    }

    // Readable from any thread (tests); the outbound thread is the only writer.
    [[nodiscard]] uint64_t samples_seen() const { return seen_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t samples_recorded() const { return recorded_.load(std::memory_order_relaxed); }

private:
    enum Stage : size_t {
        kEndToEnd, kRxBurst, kInboundThread, kInboundRing, kEngine, kOutboundRing, kOutboundThread, kTxBurst,
        kStageCount,
    };

    // Titles are matched by bench/scripts/benchlib.py's SECTION_ORDER -- change both together.
    static constexpr std::array<const char *, kStageCount> kTitles{
        "begin read -> end write   (end-to-end)",
        "begin read -> end read    (rx burst)",
        "end read -> push inbound  (inbound thread)",
        "push inbound -> engine pop (ring transit)",
        "engine pop -> engine push (engine processing)",
        "engine push -> server pop (ring transit)",
        "pop outbound -> begin write (outbound thread)",
        "begin write -> end write  (tx burst)",
    };

    static constexpr int64_t kMaxTrackableNs = 1'000'000'000; // 1 s; anything longer is clamped

    void record_(const Stage stage, const Timestamp from, const Timestamp to) {
        // Invariant TSC keeps cores in step, so to < from shouldn't happen; clamp rather than wrap.
        const int64_t ns = to > from ? static_cast<int64_t>(clock_.ticks_to_ns(to - from)) : 0;
        hdr_record_value(histograms_[stage], ns < kMaxTrackableNs ? ns : kMaxTrackableNs);
    }

    void print() const {
        if (samples_recorded() == 0) {
            std::printf("LatencyHandler: no samples collected (%llu seen, first %llu dropped as warm-up)\n",
                        static_cast<unsigned long long>(samples_seen()),
                        static_cast<unsigned long long>(LATENCY_SAMPLE_DROP));
            return;
        }

        print_hist(histograms_[kEndToEnd], kTitles[kEndToEnd]);
#if DIAGNOSTICS
        for (size_t stage = kRxBurst; stage < kStageCount; ++stage) {
            print_hist(histograms_[stage], kTitles[stage]);
        }
#endif
        std::fflush(stdout);
    }

    static void print_hist(const hdr_histogram *h, const char *title) {
        static constexpr double      pcts[]   = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
        static constexpr const char *labels[] = {"p50   ", "p90   ", "p99   ", "p99.9 ", "p99.99", "max   "};
        std::printf("\n%s\n", title);
        for (int i = 0; i < 6; ++i)
            std::printf("  %s  %6lld ns\n", labels[i],
                        static_cast<long long>(hdr_value_at_percentile(h, pcts[i])));
    }

    const TscClock &clock_;
    std::array<hdr_histogram *, kStageCount> histograms_{};
    std::atomic<uint64_t> seen_{0};
    std::atomic<uint64_t> recorded_{0};
};

#endif //LATENCY_H
