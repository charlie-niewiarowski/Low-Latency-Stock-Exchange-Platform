cmake_minimum_required(VERSION 3.25)
project(Client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# Lets this directory build standalone (e.g. a CLion profile rooted here)
# as well as via the top-level CMakeLists.txt, which already adds src/infra
# before this subdirectory.
if(NOT TARGET infra)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../infra ${CMAKE_CURRENT_BINARY_DIR}/infra)
endif()

add_executable(client-release
    app/main.cpp
    src/load_generator.cpp
)
add_executable(client-debug
        app/main.cpp
        src/load_generator.cpp
)

target_compile_options(client-release PRIVATE -O3 -march=native)
target_compile_options(client-debug PRIVATE -g -march=native)

target_include_directories(client-release PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # order_factory.hpp, client_state.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/config    # config.hpp
)
target_link_libraries(client-release PRIVATE infra)

target_include_directories(client-debug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include   # order_factory.hpp, client_state.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/config    # config.hpp
)
target_link_libraries(client-debug PRIVATE infra)
