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
target_link_libraries(server-tests PRIVATE infra dpdk_lib GTest::gtest_main)
gtest_discover_tests(server-tests)
