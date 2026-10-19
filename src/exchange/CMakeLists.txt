cmake_minimum_required(VERSION 3.25)
project(Exchange LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# Lets this directory build standalone (e.g. a CLion profile rooted here)
# as well as via the top-level CMakeLists.txt, which already adds src/infra
# before this subdirectory.
if(NOT TARGET infra)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../infra ${CMAKE_CURRENT_BINARY_DIR}/infra)
endif()

include(FetchContent)

FetchContent_Declare(
        hdr_histogram
        GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
        GIT_TAG 0.11.9
)
# Pinned: the server is built on its tcp_stack (passive-open TCP termination)
# and tap helpers. Its own gtest suite stays out of this build.
FetchContent_Declare(
        dpdk_lib
        GIT_REPOSITORY https://github.com/charlie-niewiarowski/dpdk-wrapper-library
        GIT_TAG a4d01b4332b40231a00668e18539467bb134ac97
)
set(DPDK_LIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(hdr_histogram dpdk_lib)

# Only absl::flat_hash_map is actually linked below; EXCLUDE_FROM_ALL keeps
# every other abseil target (strings, time, flags, logging, ...) out of the
# default build set so only flat_hash_map and its transitive deps get built.
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL OFF)
FetchContent_Declare(
        abseil-cpp
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG 20240722.0
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(abseil-cpp)

add_library(config INTERFACE)
target_include_directories(config INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/config)

set(EXCHANGE_SOURCES
        app/main.cpp
        app/exchange.cpp
        engine/src/engine.cpp
        engine/src/orderbook.cpp
        server/src/server.cpp
        server/src/order_entry.cpp
        server/src/ouch_responder.cpp
)

# Release
add_executable(exchange-release ${EXCHANGE_SOURCES})
target_link_libraries(exchange-release PRIVATE infra config hdr_histogram_static dpdk_lib absl::flat_hash_map)
target_compile_options(exchange-release PRIVATE
        -O3 -g -fno-omit-frame-pointer -march=native)

# Debug
add_executable(exchange-debug ${EXCHANGE_SOURCES})
target_link_libraries(exchange-debug PRIVATE infra config hdr_histogram_static dpdk_lib absl::flat_hash_map)
# GCC 12's UBSan null/nonnull checks make abseil's raw_hash_set fail to
# compile (its constexpr function-pointer comparisons stop being constant
# expressions), so those three checks are off; the rest of UBSan stays on.
target_compile_options(exchange-debug PRIVATE
        -O3 -g3 -fno-omit-frame-pointer
        -fsanitize=address,undefined
        -fno-sanitize=null,nonnull-attribute,returns-nonnull-attribute
        -march=native)
target_link_options(exchange-debug PRIVATE
        -fsanitize=address,undefined)