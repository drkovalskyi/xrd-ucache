cmake_minimum_required(VERSION 3.20)
project(xrd-ucache VERSION 0.18.0 LANGUAGES CXX) # SemVer; single source of truth for the version

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/XRootDPin.cmake)
include(GNUInstallDirs)

option(UCACHE_BUILD_BENCH "Build benchmark workloads (requires ROOT)" ON)
option(UCACHE_BUILD_TESTS "Build unit/crash tests (fetches GoogleTest)" ON)
set(UCACHE_SANITIZE "" CACHE STRING "Sanitizers: address,undefined | thread | ''")
option(UCACHE_COVERAGE "Instrument for gcov/gcovr coverage" OFF)

if(UCACHE_SANITIZE)
  add_compile_options(-fsanitize=${UCACHE_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${UCACHE_SANITIZE})
endif()
if(UCACHE_COVERAGE)
  add_compile_options(--coverage -O0 -g)
  add_link_options(--coverage)
endif()

add_subdirectory(src/core)
add_subdirectory(src/transpose) # ROOT-free transposer (skipped without codec libs)
                                # (before tests: the transpose fuzz harness links it when present)

option(UCACHE_BUILD_PLUGIN "Build the XrdCl client plugin (needs XRootD headers)" ON)
if(UCACHE_BUILD_PLUGIN)
  add_subdirectory(src/plugin)
endif()

if(UCACHE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# bench/ is the private measurement harness; released source trees ship
# without it (the option stays ON for dev checkouts, where the dir exists).
if(UCACHE_BUILD_BENCH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/workloads/CMakeLists.txt)
  add_subdirectory(bench/workloads)
endif()

add_subdirectory(src/cli) # `ucache` CLI: status/ls/stats/evict/pin/verify — links the transposer when built

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Packaging.cmake) # el9 RPM + tarball (scripts/package-el9.sh)
