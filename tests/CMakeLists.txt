cmake_minimum_required(VERSION 3.25)

# Each binary below has its own main() and self-registers its cases through
# test_runner.hpp's run_tests() — no external framework. Not yet exercised by
# CI; see README's Tests section for the "not actively maintained" caveat.
# Depends on hdr_histogram_static, fetched by src/exchange/CMakeLists.txt, so
# this subdirectory must be added after add_subdirectory(src/exchange).

set(EXCHANGE_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src/exchange)

# ── engine ───────────────────────────────────────────────────────────────────
# Uses the TESTING-gated accessors on Engine/Orderbook (engine.hpp/orderbook.hpp),
# so this target alone compiles with TESTING=1; exchange-release/debug stay
# TESTING=0.

add_executable(engine-tests
        exchange/engine/engine_test.cpp
        ${EXCHANGE_SRC}/engine/src/engine.cpp
        ${EXCHANGE_SRC}/engine/src/orderbook.cpp
)
target_include_directories(engine-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EXCHANGE_SRC}/engine/include
        ${EXCHANGE_SRC}/config
)
target_compile_definitions(engine-tests PRIVATE TESTING=1)
target_link_libraries(engine-tests PRIVATE infra)
add_test(NAME engine-tests COMMAND engine-tests)

# ── server ───────────────────────────────────────────────────────────────────
# validation-tests exercises validation.hpp's pure functions directly, no
# server/engine wiring needed. integration/pipeline drive a real Server against
# a FakeEngine double (server_fixture.hpp), so they link server/connection_info
# (and hdr_histogram_static, pulled in transitively by latency.hpp).

add_executable(validation-tests
        exchange/server/test_validation.cpp
)
target_include_directories(validation-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${EXCHANGE_SRC}/server/include
)
target_link_libraries(validation-tests PRIVATE infra)
add_test(NAME validation-tests COMMAND validation-tests)

set(SERVER_FIXTURE_SOURCES
        ${EXCHANGE_SRC}/server/src/connection_info.cpp
        ${EXCHANGE_SRC}/server/src/server.cpp
)
set(SERVER_FIXTURE_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/exchange/server
        ${EXCHANGE_SRC}/server/include
        ${EXCHANGE_SRC}/config
)

add_executable(integration-tests
        exchange/server/test_integration.cpp
        ${SERVER_FIXTURE_SOURCES}
)
target_include_directories(integration-tests PRIVATE ${SERVER_FIXTURE_INCLUDES})
target_link_libraries(integration-tests PRIVATE infra hdr_histogram_static)
add_test(NAME integration-tests COMMAND integration-tests)

add_executable(server-pipeline-tests
        exchange/server/test_server_pipeline.cpp
        ${SERVER_FIXTURE_SOURCES}
)
target_include_directories(server-pipeline-tests PRIVATE ${SERVER_FIXTURE_INCLUDES})
target_link_libraries(server-pipeline-tests PRIVATE infra hdr_histogram_static)
add_test(NAME server-pipeline-tests COMMAND server-pipeline-tests)
