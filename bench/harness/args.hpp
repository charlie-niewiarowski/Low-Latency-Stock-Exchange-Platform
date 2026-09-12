//
// Tiny shared CLI-flag scanner for bench/executables/*.cpp.
//
// Every benchmark executable needs "--flag value" parsing with a default; before
// this it was reimplemented per-executable (see git history: ring_buffer_bench.cpp
// had its own arg_u64(), orderbook_bench.cpp had its own strcmp loop). Factored
// out here so a new benchmark just includes this instead of rewriting it — the
// whole point of splitting executables/ from harness/.
//

#ifndef BENCH_HARNESS_ARGS_H
#define BENCH_HARNESS_ARGS_H

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace bench_harness {

inline uint64_t arg_u64(int argc, char** argv, const char* name, uint64_t dflt) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::strtoull(argv[i + 1], nullptr, 10);
    }
    return dflt;
}

inline int64_t arg_i64(int argc, char** argv, const char* name, int64_t dflt) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::strtoll(argv[i + 1], nullptr, 10);
    }
    return dflt;
}

} // namespace bench_harness

#endif // BENCH_HARNESS_ARGS_H
