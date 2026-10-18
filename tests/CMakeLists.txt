cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── directories ───────────────────────────────────────────────────────────────────
set(ROOT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)
set(EXCHANGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src/exchange)

# ── gtest ───────────────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)

FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

# ── engine ───────────────────────────────────────────────────────────────────
# Uses the TESTING-gated accessors on Engine/Orderbook (engine.hpp/orderbook.hpp),
# so this target alone compiles with TESTING=1; exchange-release/debug stay
# TESTING=0.

set(ENGINE_TEST_SOURCES
        engine_test.cpp
        ${EXCHANGE_SRC}/engine/src/engine.cpp
        ${EXCHANGE_SRC}/engine/src/orderbook.cpp
)
set(ENGINE_TEST_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EXCHANGE_SRC}/engine/include
        ${ROOT_SRC}/infra
        ${EXCHANGE_SRC}/config
)

add_executable(engine-tests ${ENGINE_TEST_SOURCES})
target_include_directories(engine-tests PRIVATE ${ENGINE_TEST_INCLUDES})
target_compile_definitions(engine-tests PRIVATE TESTING=1)
target_link_libraries(engine-tests PRIVATE infra GTest::gtest_main)
gtest_discover_tests(engine-tests)

# ── server ───────────────────────────────────────────────────────────────────
# server.hpp/server.cpp talk to the DPDK runtime (runtime.h, packet_factory.hpp),
# so this target links dpdk_lib alongside infra — engine-tests needs neither.

set(SERVER_TEST_SOURCES
        server_test.cpp
        ${EXCHANGE_SRC}/server/src/server.cpp
)
set(SERVER_TEST_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EXCHANGE_SRC}/server/include
        ${ROOT_SRC}/infra
        ${EXCHANGE_SRC}/config
)

add_executable(server-tests ${SERVER_TEST_SOURCES})
target_include_directories(server-tests PRIVATE ${SERVER_TEST_INCLUDES})
# TESTING=1 switches Server's EAL bring-up (server.cpp) to a software-only
# vdev instead of assuming a real hugepage-backed, PCI-bound NIC.
target_compile_definitions(server-tests PRIVATE TESTING=1)
target_link_libraries(server-tests PRIVATE infra dpdk_lib absl::flat_hash_map GTest::gtest_main)
gtest_discover_tests(server-tests)

# ── proto ────────────────────────────────────────────────────────────────────
# Pure OUCH/ITCH wire codecs (ouch_proto.hpp/itch_proto.hpp, header-only in
# src/infra) -- no DPDK, no config, so nothing beyond infra + gtest.

add_executable(proto-tests proto_tests.cpp)
target_link_libraries(proto-tests PRIVATE infra GTest::gtest_main)
gtest_discover_tests(proto-tests)

# ── packet_factory ───────────────────────────────────────────────────────────
# packet_factory.hpp (src/infra) talk to the DPDK runtime
# directly (packet_pool, ethernet/ipv4 header builders), so this target
# links dpdk_lib alongside infra like server-tests does -- but it has no
# TESTING-gated code path of its own (no config.hpp, no Server), so it
# brings up its own minimal EAL params in-test rather than needing TESTING=1.

add_executable(packet-factory-tests packet_factory_tests.cpp)
target_include_directories(packet-factory-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${ROOT_SRC}/infra
)
target_link_libraries(packet-factory-tests PRIVATE infra dpdk_lib GTest::gtest_main)
gtest_discover_tests(packet-factory-tests)
