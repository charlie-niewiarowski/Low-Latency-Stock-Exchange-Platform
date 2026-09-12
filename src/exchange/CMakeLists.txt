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
FetchContent_Declare(
        dpdk_lib
        GIT_REPOSITORY https://github.com/charlie-niewiarowski/dpdk-wrapper-library
        GIT_TAG main
)
FetchContent_MakeAvailable(hdr_histogram dpdk_lib)

add_library(config INTERFACE)
target_include_directories(config INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/config)

set(EXCHANGE_SOURCES
        app/main.cpp
        app/exchange.cpp
        engine/src/engine.cpp
        engine/src/orderbook.cpp
        server/src/server.cpp
)

# Release
add_executable(exchange-release ${EXCHANGE_SOURCES})
target_link_libraries(exchange-release PRIVATE infra config hdr_histogram_static dpdk_lib)
target_compile_options(exchange-release PRIVATE
        -O3 -g -fno-omit-frame-pointer -march=native)

# Debug
add_executable(exchange-debug ${EXCHANGE_SOURCES})
target_link_libraries(exchange-debug PRIVATE infra config hdr_histogram_static dpdk_lib)
target_compile_options(exchange-debug PRIVATE
        -O3 -g3 -fno-omit-frame-pointer
        -fsanitize=address,undefined -march=native)
target_link_options(exchange-debug PRIVATE
        -fsanitize=address,undefined)