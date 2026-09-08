cmake_minimum_required(VERSION 3.25)
project(ClientExchangeSim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

enable_testing()

# Wire types + hot-path primitives shared by exchange/ and client/, kept
# dependency-free so it can be lifted into its own repo for future trading
# systems without touching anything else here.
add_subdirectory(src/infra)

# Exchange server (matching engine + TCP gateway). Binary dir pinned to
# build/exchange (not build/src/exchange) so existing tooling (bench/,
# docker-compose.yml, README) that shells out to ./build/exchange/... keeps
# working unchanged.
add_subdirectory(src/exchange exchange)

# Load-generator client. Same binary-dir pinning rationale as above.
add_subdirectory(src/client client)

# Standalone RingBuffer<T> microbenchmark (bench/ring_buffer.py); after
# exchange/ so it can link the hdr_histogram_static target fetched there.
add_subdirectory(bench/cpp)

# Unit/integration tests; after exchange/ for the same hdr_histogram_static reason.
add_subdirectory(tests)
