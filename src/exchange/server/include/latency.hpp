//
// Created by cniew on 5/24/26.
//

#ifndef LATENCY_H
#define LATENCY_H

#include <chrono>
#include <cstdio>
#include <immintrin.h>
#include <iostream>
#include <thread>
#include <vector>
#include <hdr/hdr_histogram.h>

#include "config.hpp"
#include "order_types.hpp"

struct LatencySample {
    Timestamp t0, // read begins  (before recv syscall)
    t1, // read finished (after recv syscall)
    t2, // pushed inbound ring
    t3, // engine popped inbound ring
    t4, // engine pushed outbound ring
    t5, // server popped outbound ring
    t6, // write begins  (before send syscall)
    t7; // write finished (after send syscall)
};

//=============================================================================
// PendingLatency  (SPSC channel element: inbound thread → outbound thread)
//=============================================================================

struct PendingLatency {
    Timestamp read_begin_tsc;   // t0 - always set
    Timestamp recv_tsc;         // t1 - set under DIAGNOSTICS
    Timestamp server_push_tsc;  // t2 - set under DIAGNOSTICS
    Timestamp engine_pop_tsc;   // t3 - set under DIAGNOSTICS
    Timestamp engine_push_tsc;  // t4 - set under DIAGNOSTICS
    Timestamp server_pop;       // t5 - set under DIAGNOSTICS
    bool record;
};

class LatencyHandler {
public:
    LatencyHandler() {
        samples_.resize(LATENCY_SAMPLE_COUNT); // resize (not reserve) so operator[] into valid slots

        const Timestamp tsc1 = __rdtsc();
        const auto wall1 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Timestamp tsc2 = __rdtsc();
        const auto wall2 = std::chrono::steady_clock::now();

        ticks_per_ns_ = static_cast<double>(tsc2 - tsc1) /
            static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall2 - wall1).count());
    }

    ~LatencyHandler() {
        if (count_ == 0) {
            std::cout << "LatencyHandler: no samples collected\n";
            return;
        }

        hdr_histogram *end_to_end;
        hdr_init(1, 1'000'000'000, 5, &end_to_end);

#if DIAGNOSTICS
        hdr_histogram *recv_syscall, *inbound_push, *inbound_ring,
                       *engine_proc, *outbound_ring, *outbound_proc, *write_syscall;
        hdr_init(1, 1'000'000'000, 5, &recv_syscall);
        hdr_init(1, 1'000'000'000, 5, &inbound_push);
        hdr_init(1, 1'000'000'000, 5, &inbound_ring);
        hdr_init(1, 1'000'000'000, 5, &engine_proc);
        hdr_init(1, 1'000'000'000, 5, &outbound_ring);
        hdr_init(1, 1'000'000'000, 5, &outbound_proc);
        hdr_init(1, 1'000'000'000, 5, &write_syscall);
#endif

        for (size_t i{LATENCY_SAMPLE_DROP}; i < count_; ++i) {
            const auto& s = samples_[i];
            hdr_record_value(end_to_end, calc_latency(s.t0, s.t7));
#if DIAGNOSTICS
            hdr_record_value(recv_syscall,  calc_latency(s.t0, s.t1));
            hdr_record_value(inbound_push,  calc_latency(s.t1, s.t2));
            hdr_record_value(inbound_ring,  calc_latency(s.t2, s.t3));
            hdr_record_value(engine_proc,   calc_latency(s.t3, s.t4));
            hdr_record_value(outbound_ring, calc_latency(s.t4, s.t5));
            hdr_record_value(outbound_proc, calc_latency(s.t5, s.t6));
            hdr_record_value(write_syscall, calc_latency(s.t6, s.t7));
#endif
        }

        print_hist(end_to_end,    "begin read -> end write   (end-to-end)");
#if DIAGNOSTICS
        print_hist(recv_syscall,  "begin read -> end read    (recv syscall)");
        print_hist(inbound_push,  "end read -> push inbound  (inbound thread)");
        print_hist(inbound_ring,  "push inbound -> engine pop (ring transit)");
        print_hist(engine_proc,   "engine pop -> engine push (engine processing)");
        print_hist(outbound_ring, "engine push -> server pop (ring transit)");
        print_hist(outbound_proc, "pop outbound -> begin write (outbound thread)");
        print_hist(write_syscall, "begin write -> end write  (send syscall)");
#endif
    }

    void push_sample(const LatencySample& sample) {
        if (count_ < samples_.size()) { // guard against overflow past LATENCY_SAMPLE_COUNT
            samples_[count_++] = sample;
        }
    }

private:
    std::vector<LatencySample> samples_;
    size_t count_{};
    double ticks_per_ns_{};

    [[nodiscard]] int64_t calc_latency(const Timestamp t1, const Timestamp t2) const {
        const int64_t diff{static_cast<int64_t>(t2 - t1)};
        return diff / static_cast<int64_t>(ticks_per_ns_);
    }

    static void print_hist(hdr_histogram* h, const char* title) {
        static constexpr double      pcts[]   = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
        static constexpr const char* labels[] = {"p50   ", "p90   ", "p99   ", "p99.9 ", "p99.99", "max   "};
        std::printf("\n%s\n", title);
        for (int i = 0; i < 6; ++i)
            std::printf("  %s  %6lld ns\n", labels[i],
                        static_cast<long long>(hdr_value_at_percentile(h, pcts[i])));
    }
};

#endif //LATENCY_H