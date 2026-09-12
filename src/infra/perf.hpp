//
// Created by cniew on 5/27/26.
//

#ifndef INFRA_PERF_H
#define INFRA_PERF_H

#include <cstring>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <bits/cpu-set.h>

//=============================================================================
// software prefetch helpers
//=============================================================================
//
// Thin wrappers over __builtin_prefetch(addr, rw, locality). rw = 0 (read) / 1
// (write); locality 3 = keep in all cache levels (high temporal reuse), which
// fits the hot-path objects these warm (per-connection state, price levels).
// Compiled to nothing when PREFETCH == 0 so the hints can be A/B benchmarked.
// Consumers define PREFETCH (e.g. via their own config.hpp) before including
// this header; it defaults to off so infra/ has no required dependents.

#ifndef PREFETCH
#define PREFETCH 0
#endif

#if PREFETCH
inline void prefetch_read (const void* p) { __builtin_prefetch(p, 0, 3); }
inline void prefetch_write(const void* p) { __builtin_prefetch(p, 1, 3); }
#else
inline void prefetch_read (const void*) {}
inline void prefetch_write(const void*) {}
#endif

inline void pin_to_core(const int core) {
    // pthread_* functions return the error number directly on failure (0 on
    // success) — they do NOT follow the errno/-1 syscall convention, so the
    // return value itself (not errno) is what strerror() needs here.
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core, &cpu_set);
    if (const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set); rc != 0) {
        std::cerr << "pthread_setaffinity_np failed: " << strerror(rc) << std::endl;
    }

    sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param); rc != 0) {
        std::cerr << "pthread_setschedparam failed: " << strerror(rc)
                   << " (needs CAP_SYS_NICE or a raised RLIMIT_RTPRIO; falling back to SCHED_OTHER)"
                   << std::endl;
    }
}

#endif //INFRA_PERF_H
