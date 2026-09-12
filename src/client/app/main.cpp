//
// Created by cniew on 5/24/26.
//
// Exchange load-generator entry point.
//
// Usage:
//   client [num_clients] [rng_seed]
//
//   num_clients  – concurrent TCPHeader connections to open (default: 10)
//   rng_seed     – optional RNG seed for reproducibility (default: random)
//
// Runs until SIGINT / SIGTERM, then prints aggregate stats and throughput.
//

#include "../include/load_generator.hpp"
#include "../config/config.hpp"

#include <csignal>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

static LoadGenerator* g_orch = nullptr;

static void on_signal(int) {
    if (g_orch) g_orch->stop();
}

// Returns monotonic time in nanoseconds.
static uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL
         + static_cast<uint64_t>(ts.tv_nsec);
}

int main(const int argc, char* argv[]) {
    int num_clients = 10;
    uint64_t rng_seed = std::random_device{}();

    if (argc >= 2) {
        num_clients = std::atoi(argv[1]);
        if (num_clients <= 0) {
            std::fprintf(stderr, "Usage: %s [num_clients] [rng_seed]\n", argv[0]);
            return 1;
        }
    }
    if (argc >= 3) {
        rng_seed = static_cast<uint64_t>(std::strtoull(argv[2], nullptr, 10));
    }

    #if DIAGNOSTICS
    std::fprintf(stderr,
        "=== Exchange load generator ===\n"
        "  clients  : %d\n"
        "  seed     : %lu\n"
        "  target   : %s:%d\n"
        "  thread id: %d\n"
        "Ctrl-C to stop.\n\n",
        num_clients, rng_seed,
        EXCHANGE_HOST, EXCHANGE_PORT,
        gettid());
    #else
    std::fprintf(stderr,
        "=== Exchange load generator ===\n"
        "  clients : %d\n"
        "  seed    : %lu\n"
        "  target  : %s:%d\n"
        "Ctrl-C to stop.\n\n",
        num_clients, rng_seed,
        EXCHANGE_HOST, EXCHANGE_PORT);
    #endif

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    LoadGenerator orch{num_clients, rng_seed};
    g_orch = &orch;

    // Pin the event-loop thread to CLIENT_CORE.
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(CLIENT_CORE, &cpu_set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set) != 0)
        std::fprintf(stderr, "[WARN] pthread_setaffinity_np: %s\n", std::strerror(errno));

    // DEBUG: testing whether client-side SCHED_OTHER scheduling latency is
    // gating round-trip throughput (the exchange side alone was confirmed
    // not to be the bottleneck — its threads spin at millions of iters/sec).
    sched_param sp{};
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp); rc != 0)
        std::fprintf(stderr, "[WARN] pthread_setschedparam: %s\n", std::strerror(rc));

    const uint64_t t0 = now_ns();
    orch.run();
    const uint64_t t1 = now_ns();

    const double elapsed_s = static_cast<double>(t1 - t0) / 1e9;

    const auto [requests_sent,
                responses_ack,
                responses_match,
                responses_err,
                orders_in_flight]
    = orch.stats();

    // matches not included
    const uint64_t total_responses = responses_ack + responses_err;

    // Guard against divide-by-zero if the user stops immediately.
    const double inv = elapsed_s > 0.0 ? 1.0 / elapsed_s : 0.0;

    std::fprintf(stderr,
        "\n=== Stats ===\n"
        "  elapsed         : %.3f s\n"
        "  requests sent   : %llu\n"
        "  ACK responses   : %llu\n"
        "  MATCH responses : %llu\n"
        "  ERR responses   : %llu\n"
        "  orders in flight: %lld\n"
        "  --- throughput ---\n"
        "  requests/s      : %.0f\n"
        "  ACK/s           : %.0f\n"
        "  MATCH/s         : %.0f\n"
        "  ERR/s           : %.0f\n"
        "  total resp/s    : %.0f\n",
        elapsed_s,
        static_cast<unsigned long long>(requests_sent),
        static_cast<unsigned long long>(responses_ack),
        static_cast<unsigned long long>(responses_match),
        static_cast<unsigned long long>(responses_err),
        -1LL * static_cast<long long>(orders_in_flight),
        static_cast<double>(requests_sent)   * inv,
        static_cast<double>(responses_ack)   * inv,
        static_cast<double>(responses_match) * inv,
        static_cast<double>(responses_err)   * inv,
        static_cast<double>(total_responses)   * inv);

    return 0;
}