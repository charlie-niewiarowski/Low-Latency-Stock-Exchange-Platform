cmake_minimum_required(VERSION 3.25)

# Standalone microbenchmark for exchange/include/ring_buffer.hpp — driven by
# bench/ring_buffer.py, not part of the exchange/client pair. Depends on the
# hdr_histogram_static target fetched by exchange/CMakeLists.txt, so this
# subdirectory must be added after add_subdirectory(exchange).

find_package(Threads REQUIRED)

add_executable(ring-buffer-bench ring_buffer_bench.cpp)
target_include_directories(ring-buffer-bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../exchange/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../infra
)
target_link_libraries(ring-buffer-bench PRIVATE hdr_histogram_static Threads::Threads)
target_compile_options(ring-buffer-bench PRIVATE -O3 -g -fno-omit-frame-pointer -march=native)
