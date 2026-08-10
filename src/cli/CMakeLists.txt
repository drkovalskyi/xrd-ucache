# ucache CLI: a single C++ binary linking libucache-core —
# no XRootD, no runtime Python. When the transposer library is available
# (codec libs present), `materialize` also works natively;
# otherwise it requires --import (the Python-bridge overlay). Installed to bin/.
add_executable(ucache main.cc DiskBench.cc)
target_compile_definitions(ucache PRIVATE UCACHE_VERSION="${PROJECT_VERSION}") # `ucache --version`

# Build identity for measurement records. The version alone cannot distinguish
# builds: it is a constant between releases, so several different binaries all
# claim the same number and a benchmark record cannot say which produced it.
# `--dirty` is the load-bearing part — a bare commit would name code that was
# not what ran whenever the tree had uncommitted edits.
set(UCACHE_BUILD_ID "${PROJECT_VERSION}")
find_package(Git QUIET)
if (GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --always
                  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                  OUTPUT_VARIABLE _ucache_desc OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _ucache_desc_rc)
  if (_ucache_desc_rc EQUAL 0 AND _ucache_desc)
    set(UCACHE_BUILD_ID "${_ucache_desc}")
  endif()
  # Without these the value is captured once and then silently goes stale: a
  # rebuild after new commits would embed a confidently wrong identity, which
  # is worse than none. HEAD and index cover commits and dirtiness; the refs
  # cover TAGGING, which writes neither — a clean tree at a freshly tagged
  # commit once stamped the PREVIOUS tag because of exactly that gap. Loose
  # tags land under refs/tags (the directory mtime moves), packed ones in
  # packed-refs; watch both because git chooses either.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
               "${CMAKE_SOURCE_DIR}/.git/HEAD" "${CMAKE_SOURCE_DIR}/.git/index"
               "${CMAKE_SOURCE_DIR}/.git/refs/tags" "${CMAKE_SOURCE_DIR}/.git/packed-refs")
endif()
target_compile_definitions(ucache PRIVATE UCACHE_BUILD_ID="${UCACHE_BUILD_ID}")
target_link_libraries(ucache PRIVATE ucache-core ${CMAKE_DL_LIBS}) # dl: doctor dlopen's the plugin
if (TARGET ucache-transpose)
  target_compile_definitions(ucache PRIVATE UCACHE_HAVE_TRANSPOSE=1)
  target_link_libraries(ucache PRIVATE ucache-transpose)
endif()
install(TARGETS ucache RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
