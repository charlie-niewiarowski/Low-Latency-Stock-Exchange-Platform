//
// Created by cniew on 5/27/26.
//

#ifndef LIB_H
#define LIB_H

#include <cstring>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <bits/cpu-set.h>

#include "config.hpp"

//=============================================================================
// software prefetch helpers
//=============================================================================
//
// Thin wrappers over __builtin_prefetch(addr, rw, locality). rw = 0 (read) / 1
// (write); locality 3 = keep in all cache levels (high temporal reuse), which
// fits the hot-path objects these warm (per-connection state, price levels).
// Compiled to nothing when PREFETCH == 0 so the hints can be A/B benchmarked.

#if PREFETCH
inline void prefetch_read (const void* p) { __builtin_prefetch(p, 0, 3); }
inline void prefetch_write(const void* p) { __builtin_prefetch(p, 1, 3); }
#else
inline void prefetch_read (const void*) {}
inline void prefetch_write(const void*) {}
#endif

inline void pin_to_core(const int core) {
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    CPU_SET(core, &cpu_set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set) == -1) {
        std::cerr << "pthread_setaffinity_np failed: " << strerror(errno) << std::endl;
    }

    sched_param param{};
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == -1) {
        std::cerr << "pthread_setschedparam failed: " << strerror(errno) << std::endl;
    }
}

#endif //LIB_H
