# libXrdClUCache: the XrdCl client plugin.
# Built against the pinned XRootD client (cmake/XRootDPin.cmake):
# headers + libXrdCl from UCACHE_XROOTD_ROOT.
find_path(XRDCL_INCLUDE_DIR XrdCl/XrdClPlugInInterface.hh
          PATHS ${UCACHE_XROOTD_ROOT}/include/xrootd REQUIRED)
find_library(XRDCL_LIBRARY XrdCl
             PATHS ${UCACHE_XROOTD_ROOT}/lib64 ${UCACHE_XROOTD_ROOT}/lib REQUIRED)
ucache_require_xrootd_version("${XRDCL_INCLUDE_DIR}")

add_library(XrdClUCache SHARED
  Executor.cc
  OpenRetry.cc
  UCacheFactory.cc
  UCacheFile.cc
)
target_include_directories(XrdClUCache SYSTEM PRIVATE ${XRDCL_INCLUDE_DIR}) # SYSTEM: old-header warnings must not trip -Werror (5.6 floor pin)
target_link_libraries(XrdClUCache PRIVATE ucache-core ${XRDCL_LIBRARY})
# Match XrdCl plugin naming convention (libXrdClUCache.so, no version suffix
# needed — the conf file carries the absolute path).
set_target_properties(XrdClUCache PROPERTIES PREFIX "lib" SUFFIX ".so")
# Non-unloadable: XrdCl's PlugInManager dlcloses plugin libs at teardown, but
# our detached executor threads (and the atexit stats dump) still execute
# code from this .so — unmapping it is a guaranteed exit-time SEGV
# (found by TSan). DF_1_NODELETE makes dlclose a no-op for this library.
target_link_options(XrdClUCache PRIVATE "-Wl,-z,nodelete")

# Relocatable install: drop the baked LCG build RPATH in favour of
# $ORIGIN; the host's libXrdCl is resolved by ld.so as a normal NEEDED lib. The
# dev-host LCG build is intentionally NOT relocatable (its libstdc++ lives only
# under /cvmfs) — the canonical install is against the host xrootd-client-devel,
# exercised by the clean-container test.
set_target_properties(XrdClUCache PROPERTIES INSTALL_RPATH "$ORIGIN")
install(TARGETS XrdClUCache LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

# ucache-netbench: origin random-read baseline. Lives here (not in
# src/cli) because it needs XrdCl, which the CLI deliberately does not link.
find_package(Threads REQUIRED)
add_executable(ucache-netbench NetBench.cc)
target_include_directories(ucache-netbench SYSTEM PRIVATE ${XRDCL_INCLUDE_DIR})
target_link_libraries(ucache-netbench PRIVATE ${XRDCL_LIBRARY} Threads::Threads)
set_target_properties(ucache-netbench PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS ucache-netbench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
