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
        ${EXCHANGE_SRC}/server/src/order_entry.cpp
        ${EXCHANGE_SRC}/server/src/ouch_responder.cpp
)
set(SERVER_TEST_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EXCHANGE_SRC}/server/include
        ${ROOT_SRC}/infra
        ${EXCHANGE_SRC}/config
)

add_executable(server-tests ${SERVER_TEST_SOURCES})
target_include_directories(server-tests PRIVATE ${SERVER_TEST_INCLUDES})
# TESTING=1 opens up Server to the test fixture and leaves its threads
# unpinned. The SoupBinTCP limits shrink so replay-log overflow and the
# login/idle timeouts can be exercised in a test's time.
target_compile_definitions(server-tests PRIVATE
        TESTING=1
        SOUP_REPLAY_BYTES=4096
        SOUP_IDLE_TIMEOUT_MS=2000
        SOUP_LOGIN_TIMEOUT_MS=2000)
target_link_libraries(server-tests PRIVATE infra dpdk_lib absl::flat_hash_map hdr_histogram_static GTest::gtest_main)
# Each discovered test is its own process, and two EALs at once collide on
# DPDK's runtime files: serialize everything that brings one up.
gtest_discover_tests(server-tests PROPERTIES RESOURCE_LOCK dpdk_eal)

# ── proto ────────────────────────────────────────────────────────────────────
# Pure OUCH/ITCH wire codecs (ouch_proto.hpp/itch_proto.hpp, header-only in
# src/infra) -- no DPDK, no config, so nothing beyond infra + gtest.

add_executable(proto-tests proto_tests.cpp)
target_link_libraries(proto-tests PRIVATE infra GTest::gtest_main)
gtest_discover_tests(proto-tests)

# ── clock ────────────────────────────────────────────────────────────────────
add_executable(clock-tests clock_tests.cpp)
target_link_libraries(clock-tests PRIVATE infra GTest::gtest_main)
gtest_discover_tests(clock-tests)

# ── gateway ──────────────────────────────────────────────────────────────────
# The server's protocol halves -- OrderEntry (OUCH -> engine), OuchResponder
# (engine -> OUCH), SessionLog (replay) and validation.hpp -- have no DPDK dependency, so they're
# tested without bringing up an EAL.

add_executable(gateway-tests
        order_entry_tests.cpp
        ouch_responder_tests.cpp
        session_log_tests.cpp
        ${EXCHANGE_SRC}/server/src/order_entry.cpp
        ${EXCHANGE_SRC}/server/src/ouch_responder.cpp
)
target_include_directories(gateway-tests PRIVATE ${EXCHANGE_SRC}/server/include)
target_link_libraries(gateway-tests PRIVATE infra absl::flat_hash_map GTest::gtest_main)
gtest_discover_tests(gateway-tests)

# ── client ───────────────────────────────────────────────────────────────────
# The real LoadGenerator against an in-test SoupBinTCP/OUCH server on kernel
# loopback (EXCHANGE_HOST, overridden to 127.0.0.1, : EXCHANGE_PORT).

find_package(Threads REQUIRED)
add_executable(client-tests
        client_tests.cpp
        ${ROOT_SRC}/client/src/load_generator.cpp
)
target_include_directories(client-tests PRIVATE
        ${ROOT_SRC}/client/include
        ${ROOT_SRC}/client/config
)
target_compile_definitions(client-tests PRIVATE EXCHANGE_HOST="127.0.0.1")
target_link_libraries(client-tests PRIVATE infra Threads::Threads GTest::gtest_main)
# Every test binds the same port.
gtest_discover_tests(client-tests PROPERTIES RESOURCE_LOCK exchange_port)

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
gtest_discover_tests(packet-factory-tests PROPERTIES RESOURCE_LOCK dpdk_eal)
