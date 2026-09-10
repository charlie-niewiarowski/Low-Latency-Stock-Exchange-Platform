cmake_minimum_required(VERSION 3.25)

# Thin driver mains that each wire one object under test into a benchmark loop
# (bench/harness/ holds the CLI-parsing etc. they share) — the "N" side of the
# N objects x M tools benchmark split; bench/scripts/*.py (the "M" tools, e.g.
# perf_on_executable.py) drive these executables generically. Added after
# add_subdirectory(src/exchange) — ring-buffer-bench depends on the
# hdr_histogram_static target fetched there.

set(EXCHANGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src/exchange)
set(HARNESS_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/../harness)

find_package(Threads REQUIRED)

# ── ring-buffer-bench — RDTSC-timed RingBuffer<T> push/pop, driven by
# bench/scripts/ring_buffer.py ──────────────────────────────────────────────
add_executable(ring-buffer-bench ring_buffer_bench.cpp)
target_include_directories(ring-buffer-bench PRIVATE ${HARNESS_INCLUDE})
target_link_libraries(ring-buffer-bench PRIVATE infra hdr_histogram_static Threads::Threads)
target_compile_options(ring-buffer-bench PRIVATE -O3 -g -fno-omit-frame-pointer -march=native)

# ── orderbook-bench — drives Orderbook::process() directly (no Engine/Server),
# meant to be run under `perf stat` via bench/scripts/orderbook_perf.py.
# Deliberately only depends on infra + config (no hdr_histogram/dpdk) so it
# builds without touching FetchContent. ──────────────────────────────────────
add_executable(orderbook-bench
        orderbook_bench.cpp
        ${EXCHANGE_SRC}/engine/src/orderbook.cpp
)
target_include_directories(orderbook-bench PRIVATE
        ${EXCHANGE_SRC}/engine/include
        ${HARNESS_INCLUDE}
)
target_link_libraries(orderbook-bench PRIVATE infra config)
target_compile_definitions(orderbook-bench PRIVATE TESTING=1)
target_compile_options(orderbook-bench PRIVATE -O3 -g -fno-omit-frame-pointer -march=native)
