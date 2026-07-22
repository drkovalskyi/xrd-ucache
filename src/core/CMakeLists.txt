# libucache-core: pure C++17, no XRootD dependency.
# Static with PIC — the plugin .so links it.
add_library(ucache-core STATIC
  CacheStore.cc
  Config.cc
  FileEntry.cc
  IOBackend.cc
  Log.cc
  MetaFile.cc
  ReplicaFile.cc
  ReplicaStore.cc
  Stats.cc
  Trace.cc
  UrlKey.cc
  vendor/crc32c.cc
  vendor/sha256.cc
)
set_target_properties(ucache-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ucache-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(ucache-core PUBLIC cxx_std_17)
# scanObjects' parallel shard walk uses std::thread.
find_package(Threads REQUIRED)
target_link_libraries(ucache-core PUBLIC Threads::Threads)

# Fault-injection backend for tests and later phases' test drivers.
add_library(ucache-core-testing STATIC testing/FaultIO.cc)
set_target_properties(ucache-core-testing PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ucache-core-testing PUBLIC ucache-core)
