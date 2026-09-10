//
// ring_buffer_bench.cpp — standalone push/pop latency + throughput harness for
// src/infra/ring_buffer.hpp, driven by bench/scripts/ring_buffer.py.
//
// This does NOT go through the TCP client/exchange pair the rest of bench/
// drives — it links RingBuffer<T> directly and times push()/pop() with RDTSC,
// the same primitive src/exchange/server/include/latency.hpp uses on the real
// hot path. Three modes (see usage()):
//
//   op    same-thread push-then-pop; no cross-core effects at all.
//   spsc  producer/consumer pinned to separate physical cores, mirroring how
//         the exchange actually uses the ring (inbound thread -> engine).
//   sat   both threads spinning flat out; sustained ops/s + push reject rate.
//
// Measurement hygiene (why the numbers should be trustworthy):
//   - RDTSC is bracketed with LFENCE/RDTSCP per the standard ordering idiom
//     (see tsc_begin/tsc_end below) so neither the timed op nor neighboring
//     loop iterations can leak across the timestamp reads.
//   - All per-sample bookkeeping (ns conversion, hdr_record_value, vector
//     growth) happens AFTER the timed loop, against a pre-sized array — the
//     only things inside the timed bracket are the RDTSC reads and the ring
//     op itself.
//   - spsc timestamps a message on the producer's core and reads it back on
//     the consumer's core, so the raw delta includes whatever constant skew
//     exists between the two cores' TSCs. We measure that skew with a
//     ping-pong handshake before the timed run and subtract it (see
//     calib_producer_side/calib_consumer_side), instead of silently
//     reporting a biased number.
//
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <immintrin.h>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

#include <hdr/hdr_histogram.h>

#include "args.hpp"
#include "ring_buffer.hpp"

//=============================================================================
// timestamping
//=============================================================================
// LFENCE;RDTSC to open a timed region, RDTSCP;LFENCE to close it — the
// standard idiom (Intel's benchmarking whitepaper, Linux's rdtsc_ordered())
// for prints that don't want the CPU reordering real work across the reads.

static inline uint64_t tsc_begin() {
    _mm_lfence();
    return __rdtsc();
}

static inline uint64_t tsc_end() {
    unsigned aux;
    const uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

//=============================================================================
// core pinning
//=============================================================================

static void pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::fprintf(stderr, "[warn] pthread_setaffinity_np(core=%d) failed: %s\n",
                     core, strerror(errno));
    }
    sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        std::fprintf(stderr, "[warn] SCHED_FIFO unavailable (need CAP_SYS_NICE); "
                              "continuing at normal priority\n");
    }
}

static bool tsc_flags_ok() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    bool constant = false, nonstop = false;
    while (std::getline(f, line)) {
        if (line.rfind("flags", 0) == 0) {
            constant = line.find("constant_tsc") != std::string::npos;
            nonstop  = line.find("nonstop_tsc")  != std::string::npos;
            break;
        }
    }
    std::printf("constant_tsc: %s\n", constant ? "yes" : "no");
    std::printf("nonstop_tsc: %s\n", nonstop ? "yes" : "no");
    return constant && nonstop;
}

// Calibrate ticks-per-ns the same way LatencyHandler does: RDTSC delta over a
// known wall-clock sleep.
static double calibrate_ticks_per_ns() {
    const uint64_t t0 = tsc_begin();
    const auto w0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const uint64_t t1 = tsc_end();
    const auto w1 = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count();
    return static_cast<double>(t1 - t0) / static_cast<double>(ns);
}

//=============================================================================
// payload — sized to match InboundMessage/OutboundMessage (56 bytes) so the
// benchmark pays the same buffer[idx] = item copy-assignment cost the real
// ring pays; carries its own send timestamp for the spsc transit test.
//=============================================================================

struct Payload {
    uint64_t tsc;
    uint64_t seq;
    uint8_t filler[40];
};
static_assert(sizeof(Payload) == 56);

//=============================================================================
// hdr_histogram plumbing (mirrors exchange/server/include/latency.hpp)
//=============================================================================

static hdr_histogram* make_hist() {
    hdr_histogram* h;
    hdr_init(1, 1'000'000'000, 5, &h);
    return h;
}

static void print_hist(hdr_histogram* h, const char* title) {
    static constexpr double pcts[] = {50.0, 90.0, 99.0, 99.9, 99.99, 100.0};
    static constexpr const char* labels[] = {"p50   ", "p90   ", "p99   ", "p99.9 ", "p99.99", "max   "};
    std::printf("\n%s\n", title);
    for (int i = 0; i < 6; ++i)
        std::printf("  %s  %6lld ns\n", labels[i],
                    static_cast<long long>(hdr_value_at_percentile(h, pcts[i])));
}

static void record_all(hdr_histogram* h, const std::vector<int64_t>& ticks, double ticks_per_ns) {
    for (const int64_t t : ticks)
        hdr_record_value(h, static_cast<int64_t>(static_cast<double>(t) / ticks_per_ns));
}

//=============================================================================
// mode: op — same-thread push-then-pop, no cross-core effects
//=============================================================================

static void mode_op(uint32_t capacity, uint64_t iters, uint64_t warmup, int cpu) {
    pin_to_core(cpu);
    const double ticks_per_ns = calibrate_ticks_per_ns();
    std::printf("ticks_per_ns: %.4f\n", ticks_per_ns);

    RingBuffer<Payload> ring(capacity);
    Payload item{};
    Payload out{};

    for (uint64_t i = 0; i < warmup; ++i) {
        item.seq = i;
        (void)ring.push(item); // capacity guarantees success (never > 1 item queued)
        out = *ring.pop();
    }

    std::vector<int64_t> push_ticks(iters), pop_ticks(iters), rt_ticks(iters);
    for (uint64_t i = 0; i < iters; ++i) {
        item.seq = warmup + i;

        asm volatile("" ::: "memory");
        const uint64_t t0 = tsc_begin();
        asm volatile("" ::: "memory");
        (void)ring.push(item); // same-thread push-then-pop: capacity guarantees success
        asm volatile("" ::: "memory");
        const uint64_t t1 = tsc_end();

        asm volatile("" ::: "memory");
        const uint64_t t2 = tsc_begin();
        asm volatile("" ::: "memory");
        out = *ring.pop(); // same-thread push-then-pop: capacity guarantees success
        asm volatile("" ::: "memory");
        const uint64_t t3 = tsc_end();

        push_ticks[i] = static_cast<int64_t>(t1 - t0);
        pop_ticks[i]  = static_cast<int64_t>(t3 - t2);
        rt_ticks[i]   = static_cast<int64_t>(t3 - t0);
    }

    hdr_histogram* h_push = make_hist();
    hdr_histogram* h_pop  = make_hist();
    hdr_histogram* h_rt   = make_hist();
    record_all(h_push, push_ticks, ticks_per_ns);
    record_all(h_pop,  pop_ticks,  ticks_per_ns);
    record_all(h_rt,   rt_ticks,   ticks_per_ns);

    print_hist(h_push, "push begin -> push end     (enqueue op)");
    print_hist(h_pop,  "pop begin -> pop end       (dequeue op)");
    print_hist(h_rt,   "push begin -> pop end      (round-trip op)");
    std::printf("checksum: %llu\n", static_cast<unsigned long long>(out.seq));
}

//=============================================================================
// mode: spsc — cross-core producer/consumer, mirrors real ring usage
//=============================================================================

struct Calib {
    alignas(64) std::atomic<uint64_t> ping{0};
    alignas(64) std::atomic<uint64_t> pong{0};
};

// Ping-pong clock-offset estimate (Cristian's algorithm): the round with the
// smallest RTT is the least likely to have been delayed by scheduling/cache
// jitter, so its offset sample is the best estimate of the constant skew
// between the two cores' TSCs. Producer side.
static void calib_producer_side(Calib& c, int rounds, int64_t& best_offset) {
    int64_t best_rtt = INT64_MAX;
    best_offset = 0;
    for (int r = 0; r < rounds; ++r) {
        const uint64_t ta = tsc_begin();
        c.ping.store(ta, std::memory_order_release);
        uint64_t tb;
        while ((tb = c.pong.load(std::memory_order_acquire)) == 0) {}
        const uint64_t tc = tsc_end();
        c.pong.store(0, std::memory_order_relaxed);

        const auto rtt = static_cast<int64_t>(tc - ta);
        const auto offset = static_cast<int64_t>(tb) - static_cast<int64_t>((ta + tc) / 2);
        if (rtt < best_rtt) { best_rtt = rtt; best_offset = offset; }
    }
}

static void calib_consumer_side(Calib& c, int rounds) {
    for (int r = 0; r < rounds; ++r) {
        uint64_t ta;
        while ((ta = c.ping.load(std::memory_order_acquire)) == 0) {}
        const uint64_t tb = tsc_begin();
        c.pong.store(tb, std::memory_order_release);
        c.ping.store(0, std::memory_order_relaxed);
    }
}

static void mode_spsc(uint32_t capacity, uint64_t iters, uint64_t warmup,
                       int producer_cpu, int consumer_cpu, uint64_t rate) {
    const double ticks_per_ns = calibrate_ticks_per_ns();
    std::printf("ticks_per_ns: %.4f\n", ticks_per_ns);
    tsc_flags_ok();

    RingBuffer<Payload> ring(capacity);
    Calib calib;
    std::atomic<bool> ready_p{false}, ready_c{false}, go{false};
    std::atomic<int64_t> offset_ticks{0};

    std::vector<int64_t> lat_ticks(iters);
    uint64_t neg_count = 0;

    std::thread producer([&] {
        pin_to_core(producer_cpu);
        int64_t best_offset;
        calib_producer_side(calib, 4000, best_offset);
        offset_ticks.store(best_offset, std::memory_order_relaxed);

        ready_p.store(true, std::memory_order_release);
        while (!ready_c.load(std::memory_order_acquire)) {}
        while (!go.load(std::memory_order_acquire)) {}

        Payload item{};
        const uint64_t interval = rate ? static_cast<uint64_t>(ticks_per_ns * 1e9 / static_cast<double>(rate)) : 0;
        uint64_t deadline = tsc_begin();

        for (uint64_t i = 0; i < warmup; ++i) {
            item.seq = i;
            item.tsc = tsc_begin();
            while (!ring.push(item)) {}
        }
        for (uint64_t i = 0; i < iters; ++i) {
            if (interval) {
                deadline += interval;
                while (tsc_begin() < deadline) {}
            }
            item.seq = warmup + i;
            for (;;) {
                asm volatile("" ::: "memory");
                const uint64_t t0 = tsc_begin();
                item.tsc = t0;
                asm volatile("" ::: "memory");
                if (ring.push(item)) break;
            }
        }
    });

    std::thread consumer([&] {
        pin_to_core(consumer_cpu);
        calib_consumer_side(calib, 4000);

        ready_c.store(true, std::memory_order_release);
        while (!ready_p.load(std::memory_order_acquire)) {}
        go.store(true, std::memory_order_release);

        Payload out{};
        for (uint64_t i = 0; i < warmup; ++i) {
            std::optional<Payload> v;
            while (!(v = ring.pop())) {}
            out = *v;
        }
        const int64_t off = offset_ticks.load(std::memory_order_relaxed);
        for (uint64_t i = 0; i < iters; ++i) {
            for (;;) {
                if (auto v = ring.pop()) { out = *v; break; }
            }
            asm volatile("" ::: "memory");
            const uint64_t t1 = tsc_end();
            asm volatile("" ::: "memory");
            int64_t d = static_cast<int64_t>(t1 - out.tsc) - off;
            if (d < 1) { d = 1; ++neg_count; }
            lat_ticks[i] = d;
        }
    });

    producer.join();
    consumer.join();

    std::printf("tsc_offset_ns: %.1f\n", static_cast<double>(offset_ticks.load()) / ticks_per_ns);
    std::printf("negative_after_correction: %llu (%.4f%%)\n",
                static_cast<unsigned long long>(neg_count),
                100.0 * static_cast<double>(neg_count) / static_cast<double>(iters));

    // Unlike op-mode, spsc is asymmetrically fragile: a *single* scheduler
    // preemption of either thread (unavoidable here — SCHED_FIFO needs
    // CAP_SYS_NICE, see the warning above) lets the queue build a backlog,
    // and every item queued behind it inherits that stall as latency once the
    // thread resumes and drains through — so one bad preemption can smear
    // across a large share of the samples, not just one. min_ns is a much
    // more robust "how fast can this ring actually go" number than the
    // percentiles on a contended host; stall_suspect flags samples that look
    // environmental rather than algorithmic (anything past a few us has no
    // business being ring-transit latency on modern x86).
    int64_t min_tick = lat_ticks.empty() ? 0 : lat_ticks[0];
    for (const int64_t t : lat_ticks) if (t < min_tick) min_tick = t;
    const auto stall_threshold_ticks = static_cast<int64_t>(5000.0 * ticks_per_ns); // 5us
    uint64_t stall_suspect = 0;
    for (const int64_t t : lat_ticks) if (t > stall_threshold_ticks) ++stall_suspect;

    std::printf("min_ns: %.1f\n", static_cast<double>(min_tick) / ticks_per_ns);
    std::printf("stall_suspect_count: %llu (%.4f%%)\n",
                static_cast<unsigned long long>(stall_suspect),
                100.0 * static_cast<double>(stall_suspect) / static_cast<double>(iters));

    hdr_histogram* h = make_hist();
    record_all(h, lat_ticks, ticks_per_ns);
    print_hist(h, "producer push -> consumer pop  (spsc ring transit)");
}

//=============================================================================
// mode: sat — saturation throughput
//=============================================================================

static void mode_sat(uint32_t capacity, uint64_t iters, int producer_cpu, int consumer_cpu) {
    RingBuffer<Payload> ring(capacity);
    std::atomic<bool> ready_p{false}, ready_c{false}, go{false};
    std::atomic<uint64_t> push_attempts{0};
    std::chrono::steady_clock::time_point t_start, t_end;

    std::thread producer([&] {
        pin_to_core(producer_cpu);
        ready_p.store(true, std::memory_order_release);
        while (!ready_c.load(std::memory_order_acquire)) {}
        while (!go.load(std::memory_order_acquire)) {}

        Payload item{};
        uint64_t attempts = 0;
        for (uint64_t i = 0; i < iters; ++i) {
            item.seq = i;
            while (!ring.push(item)) { ++attempts; }
            ++attempts;
        }
        push_attempts.store(attempts, std::memory_order_relaxed);
    });

    std::thread consumer([&] {
        pin_to_core(consumer_cpu);
        ready_c.store(true, std::memory_order_release);
        while (!ready_p.load(std::memory_order_acquire)) {}
        t_start = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);

        Payload out{};
        for (uint64_t i = 0; i < iters; ++i) {
            std::optional<Payload> v;
            while (!(v = ring.pop())) {}
            out = *v;
        }
        t_end = std::chrono::steady_clock::now();
    });

    producer.join();
    consumer.join();

    const double secs = std::chrono::duration<double>(t_end - t_start).count();
    const uint64_t attempts = push_attempts.load();
    std::printf("ops_s: %.2f\n", static_cast<double>(iters) / secs);
    std::printf("push_fail_ratio: %.6f\n",
                static_cast<double>(attempts - iters) / static_cast<double>(attempts));
}

//=============================================================================
// CLI
//=============================================================================

static void usage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s <op|spsc|sat> [--capacity N] [--iters N] [--warmup N]\n"
        "          [--producer-cpu N] [--consumer-cpu N] [--rate N]\n", argv0);
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }
    const std::string mode = argv[1];
    using bench_harness::arg_u64;

    const auto capacity     = static_cast<uint32_t>(arg_u64(argc, argv, "--capacity", 524288));
    const auto warmup       = arg_u64(argc, argv, "--warmup", 50'000);
    const auto producer_cpu = static_cast<int>(arg_u64(argc, argv, "--producer-cpu", 4));
    const auto consumer_cpu = static_cast<int>(arg_u64(argc, argv, "--consumer-cpu", 5));
    const auto rate         = arg_u64(argc, argv, "--rate", 0);

    if (mode == "op") {
        mode_op(capacity, arg_u64(argc, argv, "--iters", 2'000'000), warmup, producer_cpu);
    } else if (mode == "spsc") {
        mode_spsc(capacity, arg_u64(argc, argv, "--iters", 1'000'000), warmup,
                  producer_cpu, consumer_cpu, rate);
    } else if (mode == "sat") {
        mode_sat(capacity, arg_u64(argc, argv, "--iters", 5'000'000), producer_cpu, consumer_cpu);
    } else {
        usage(argv[0]);
        return 1;
    }
    return 0;
}
