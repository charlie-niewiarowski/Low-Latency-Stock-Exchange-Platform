cmake_minimum_required(VERSION 3.25)

# Wire types + hot-path primitives shared by exchange/ and client/ (and any
# future project that talks to this exchange). Deliberately dependency-free —
# nothing here includes anything from exchange/ or client/ — so this directory
# can be lifted into its own repo later with no changes beyond this file.

add_library(infra INTERFACE)
target_include_directories(infra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
