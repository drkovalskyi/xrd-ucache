# ucache-transpose: ROOT-free format parser + transcoder for the
# replica tier. Needs the four codec libraries ROOT payloads use;
# where any is missing the transposer simply isn't built — materialization
# then requires the Python bridge (fail-open at the packaging level).
# lzma/z/zstd are required; lz4 is OPTIONAL — it is only needed to DECODE
# L4-compressed source payloads (rare; targets are always ZSTD-1). Without
# it those payloads make the parser refuse: fail-open, no transpose.
find_library(UCACHE_LZMA lzma)
find_library(UCACHE_ZLIB z)
find_library(UCACHE_ZSTD zstd)
find_library(UCACHE_LZ4 lz4)
find_path(UCACHE_LZMA_INC lzma.h)
find_path(UCACHE_ZSTD_INC zstd.h)
find_path(UCACHE_LZ4_INC lz4.h)

if (UCACHE_LZMA AND UCACHE_ZLIB AND UCACHE_ZSTD AND UCACHE_LZMA_INC AND UCACHE_ZSTD_INC)
  add_library(ucache-transpose STATIC TreeMeta.cc RNTupleMeta.cc RNTupleRewrite.cc Transposer.cc)
  set_target_properties(ucache-transpose PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(ucache-transpose
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${UCACHE_LZMA_INC} ${UCACHE_ZSTD_INC})
  target_compile_features(ucache-transpose PUBLIC cxx_std_17)
  target_link_libraries(ucache-transpose PUBLIC ucache-core
    ${UCACHE_LZMA} ${UCACHE_ZLIB} ${UCACHE_ZSTD})
  if (UCACHE_LZ4 AND UCACHE_LZ4_INC)
    target_compile_definitions(ucache-transpose PRIVATE UCACHE_HAVE_LZ4=1)
    target_include_directories(ucache-transpose PRIVATE ${UCACHE_LZ4_INC})
    target_link_libraries(ucache-transpose PUBLIC ${UCACHE_LZ4})
    message(STATUS "ucache-transpose: enabled (with lz4 decode)")
  else()
    message(STATUS "ucache-transpose: enabled (no lz4 — L4 sources refused)")
  endif()

  add_executable(ucache-tree-dump tree_dump.cc)
  target_link_libraries(ucache-tree-dump PRIVATE ucache-transpose)

  add_executable(ucache-rntuple-dump rntuple_dump.cc)
  target_link_libraries(ucache-rntuple-dump PRIVATE ucache-transpose)

  add_executable(ucache-rntuple-rewrite rntuple_rewrite.cc)
  target_link_libraries(ucache-rntuple-rewrite PRIVATE ucache-transpose)
else()
  message(STATUS "ucache-transpose: SKIPPED (lzma/z/zstd incomplete)")
endif()
