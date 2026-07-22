# ucache CLI: a single C++ binary linking libucache-core —
# no XRootD, no runtime Python. When the transposer library is available
# (codec libs present), `materialize` also works natively;
# otherwise it requires --import (the Python-bridge overlay). Installed to bin/.
add_executable(ucache main.cc DiskBench.cc)
target_compile_definitions(ucache PRIVATE UCACHE_VERSION="${PROJECT_VERSION}") # `ucache --version`
target_link_libraries(ucache PRIVATE ucache-core ${CMAKE_DL_LIBS}) # dl: doctor dlopen's the plugin
if (TARGET ucache-transpose)
  target_compile_definitions(ucache PRIVATE UCACHE_HAVE_TRANSPOSE=1)
  target_link_libraries(ucache PRIVATE ucache-transpose)
endif()
install(TARGETS ucache RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
