# GoogleTest via FetchContent, pinned.
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(ucache-unit-tests
  unit/crc32c_test.cc
  unit/sha256_test.cc
  unit/config_test.cc
  unit/log_test.cc
  unit/iobackend_test.cc
  unit/urlkey_test.cc
  unit/pagebitmap_test.cc
  unit/stats_test.cc
  unit/metafile_test.cc
  unit/fileentry_test.cc
  unit/cachestore_test.cc
  unit/replica_test.cc
  unit/differential_test.cc
  unit/concurrency_test.cc
  unit/fault_test.cc
  unit/open_retry_test.cc
  unit/executor_test.cc
  ${CMAKE_SOURCE_DIR}/src/plugin/OpenRetry.cc  # pure policy TU (no XrdCl)
  ${CMAKE_SOURCE_DIR}/src/plugin/Executor.cc   # thread pool (no XrdCl)
)
target_link_libraries(ucache-unit-tests PRIVATE ucache-core ucache-core-testing gtest gtest_main)
target_include_directories(ucache-unit-tests PRIVATE ${CMAKE_SOURCE_DIR}/src/plugin)
if (TARGET ucache-transpose) # learner + seek-width + hole-verification contracts
  target_sources(ucache-unit-tests PRIVATE unit/learner_test.cc unit/transpose_width_test.cc
                 unit/transpose_hole_test.cc)
  target_link_libraries(ucache-unit-tests PRIVATE ucache-transpose)
  target_include_directories(ucache-unit-tests PRIVATE ${CMAKE_SOURCE_DIR}/src/cli) # CacheSource.h
endif()

add_executable(ucache-crash-harness crash/crash_harness.cc)
target_link_libraries(ucache-crash-harness PRIVATE ucache-core)

# Multi-process eviction soak. Long-running; LABEL "soak" so the
# normal gate excludes it (run explicitly via `ctest -L soak`, tune with
# UCACHE_SOAK_SECONDS/PROCS/MAX_BYTES). Default duration is short (60s) so a
# stray full-`ctest` run is a smoke, not a 30-minute stall.
add_executable(ucache-soak-harness soak/soak_harness.cc)
target_link_libraries(ucache-soak-harness PRIVATE ucache-core)

include(GoogleTest)
gtest_discover_tests(ucache-unit-tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 600)

# Crash suite as a ctest entry; iterations via UCACHE_CRASH_ITERS (default 200).
add_test(NAME crash-suite COMMAND ucache-crash-harness)
set_tests_properties(crash-suite PROPERTIES TIMEOUT 1800)

add_test(NAME soak-suite COMMAND ucache-soak-harness)
set_tests_properties(soak-suite PROPERTIES TIMEOUT 2400 LABELS soak)

# Plugin-level fuzz (needs a running origin; driven by tests/integration/).
if(UCACHE_BUILD_PLUGIN)
  add_executable(ucache-plugin-fuzz integration/plugin_fuzz.cc)
  target_include_directories(ucache-plugin-fuzz SYSTEM PRIVATE ${XRDCL_INCLUDE_DIR})
  target_link_libraries(ucache-plugin-fuzz PRIVATE ${XRDCL_LIBRARY})
endif()

# Transposer mutational fuzz: parser/builder fail-open under hostile
# bytes. The buffer stage is self-contained; the file stage runs when
# UCACHE_FUZZ_FILE points at a small ROOT-written seed (the local gate
# runner wires one in where `root` can generate it).
if(TARGET ucache-transpose)
  add_executable(ucache-transpose-fuzz fuzz/transpose_fuzz.cc)
  target_link_libraries(ucache-transpose-fuzz PRIVATE ucache-transpose)
  add_test(NAME transpose-fuzz COMMAND ucache-transpose-fuzz)
  set_tests_properties(transpose-fuzz PROPERTIES TIMEOUT 900)
endif()
