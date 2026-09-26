# libXrdClUCache: the XrdCl client plugin.
# Built against the pinned XRootD client (cmake/XRootDPin.cmake):
# headers + libXrdCl from UCACHE_XROOTD_ROOT.
# UCACHE_XROOTD_ROOT is authoritative when it resolves: search it FIRST with
# NO_DEFAULT_PATH so an ambient CMAKE_PREFIX_PATH (toolchain views) cannot
# substitute a different libXrdCl than the headers being compiled against.
find_path(XRDCL_INCLUDE_DIR XrdCl/XrdClPlugInInterface.hh
          PATHS ${UCACHE_XROOTD_ROOT}/include/xrootd NO_DEFAULT_PATH)
# Fallback: discover it on the default search path. PATH_SUFFIXES matters —
# every packaged XRootD puts the headers under <prefix>/include/xrootd, not
# <prefix>/include, so without it this stage can only succeed on a hand-built
# install and CMAKE_PREFIX_PATH alone (MacPorts, conda, Homebrew, a distro
# package) fails to configure with the headers sitting right there.
find_path(XRDCL_INCLUDE_DIR XrdCl/XrdClPlugInInterface.hh PATH_SUFFIXES xrootd REQUIRED)
find_library(XRDCL_LIBRARY XrdCl
             PATHS ${UCACHE_XROOTD_ROOT}/lib64 ${UCACHE_XROOTD_ROOT}/lib NO_DEFAULT_PATH)
find_library(XRDCL_LIBRARY XrdCl REQUIRED)
ucache_require_xrootd_version("${XRDCL_INCLUDE_DIR}" XRDCL_PLUGIN_SUFFIX)

# Optional: a second XRootD client install, of the OTHER major version, to build
# the plugin against as well. A compiled plugin serves one client major only —
# the major decides the client library it links (libXrdCl.so.3 for 5,
# libXrdCl.so.6 for 6), the timeout type in every method it overrides, and the
# version the client checks before calling it — so a package that serves both
# carries two builds, and each client picks its own by file name (below).
set(UCACHE_XROOTD_EXTRA_ROOT "" CACHE PATH
    "A second XRootD client install, of a different major version: the plugin is built against it too")
if(UCACHE_XROOTD_EXTRA_ROOT)
  find_path(XRDCL_EXTRA_INCLUDE_DIR XrdCl/XrdClPlugInInterface.hh
            PATHS ${UCACHE_XROOTD_EXTRA_ROOT}/include/xrootd NO_DEFAULT_PATH REQUIRED)
  find_library(XRDCL_EXTRA_LIBRARY XrdCl
               PATHS ${UCACHE_XROOTD_EXTRA_ROOT}/lib64 ${UCACHE_XROOTD_EXTRA_ROOT}/lib
               NO_DEFAULT_PATH REQUIRED)
  ucache_require_xrootd_version("${XRDCL_EXTRA_INCLUDE_DIR}" XRDCL_EXTRA_PLUGIN_SUFFIX)
  if(XRDCL_EXTRA_PLUGIN_SUFFIX STREQUAL XRDCL_PLUGIN_SUFFIX)
    message(FATAL_ERROR
      "ucache: UCACHE_XROOTD_EXTRA_ROOT is XRootD ${XRDCL_EXTRA_PLUGIN_SUFFIX}, the same major as "
      "UCACHE_XROOTD_ROOT — both builds would be libXrdClUCache-${XRDCL_PLUGIN_SUFFIX}.so. Give it a "
      "client of the other major version, or leave it unset.")
  endif()
endif()

# One plugin build against one XRootD client. The file is named after the
# client major it serves, libXrdClUCache-<major>.so, which is how XRootD names
# its own plugins: a conf that says `lib = <dir>/libXrdClUCache.so` makes a
# client open <dir>/libXrdClUCache-<its major>.so, and only if that file does
# not exist the plain name. So one conf serves 5.x and 6.x clients alike, each
# loading the build made for it, and nothing needs to know which is installed.
function(ucache_add_plugin target include_dir library suffix)
  if(NOT suffix)
    message(FATAL_ERROR "ucache: cannot tell which XRootD major ${include_dir} is "
                        "(no XRDPLUGIN_SOVERSION or version in XrdVersion.hh)")
  endif()
  add_library(${target} SHARED
    Announce.cc
    CopyDetect.cc
    Executor.cc
    OpenRetry.cc
    UCacheFactory.cc
    UCacheFile.cc
  )
  target_include_directories(${target} SYSTEM PRIVATE ${include_dir}) # SYSTEM: old-header warnings must not trip -Werror (5.6 floor pin)
  # The version uCache announces to servers at login (same source of truth as
  # `ucache --version`).
  target_compile_definitions(${target} PRIVATE UCACHE_VERSION="${PROJECT_VERSION}")
  # CMAKE_DL_LIBS: the copy detector's loader queries (dladdr, dlopen) -- empty
  # where the C library already provides them.
  target_link_libraries(${target} PRIVATE ucache-core ${library} ${CMAKE_DL_LIBS})
  # Read-ahead needs the tree parser (and so the codecs it decompresses the
  # metadata record with); a codec-less build ships the plugin without it.
  if (TARGET ucache-transpose)
    target_sources(${target} PRIVATE Prefetch.cc ColdRun.cc ReadRule.cc)
    target_link_libraries(${target} PRIVATE ucache-transpose)
    target_compile_definitions(${target} PRIVATE UCACHE_HAVE_PREFETCH=1 UCACHE_HAVE_COLDRUN=1)
  endif()
  # .so on every platform, macOS included: XrdCl plugin naming, and the conf
  # names the file.
  set_target_properties(${target} PROPERTIES PREFIX "lib" SUFFIX ".so"
                        OUTPUT_NAME "XrdClUCache-${suffix}")
  # Non-unloadable: XrdCl's PlugInManager dlcloses plugin libs at teardown, but
  # our detached executor threads (and the atexit stats dump) still execute
  # code from this .so — unmapping it is a guaranteed exit-time SEGV
  # (found by TSan). DF_1_NODELETE makes dlclose a no-op for this library.
  # The flag is GNU-ld only. Where it does not exist the library asks the loader
  # for the same guarantee at init instead, and the definition below is what tells
  # it which case it is in — so the two halves cannot drift apart.
  if (NOT APPLE)
    target_link_options(${target} PRIVATE "-Wl,-z,nodelete")
    target_compile_definitions(${target} PRIVATE UCACHE_LINK_NODELETE=1)
  endif()

  # Relocatable install: drop the baked LCG build RPATH in favour of
  # $ORIGIN; the host's libXrdCl is resolved by ld.so as a normal NEEDED lib. The
  # dev-host LCG build is intentionally NOT relocatable (its libstdc++ lives only
  # under /cvmfs) — the canonical install is against the host xrootd-client-devel,
  # exercised by the clean-container test.
  set_target_properties(${target} PROPERTIES INSTALL_RPATH "$ORIGIN")
  install(TARGETS ${target} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endfunction()

ucache_add_plugin(XrdClUCache "${XRDCL_INCLUDE_DIR}" "${XRDCL_LIBRARY}" "${XRDCL_PLUGIN_SUFFIX}")
if(UCACHE_XROOTD_EXTRA_ROOT)
  ucache_add_plugin(XrdClUCacheExtra "${XRDCL_EXTRA_INCLUDE_DIR}" "${XRDCL_EXTRA_LIBRARY}"
                    "${XRDCL_EXTRA_PLUGIN_SUFFIX}")
endif()
# The build tree also carries the plain name, pointing at the main build, for
# tools that open the plugin by path. It is never installed: an installed plain
# file would be what a client of a major with no build of its own falls back
# to, and it would then refuse it.
add_custom_command(TARGET XrdClUCache POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E create_symlink $<TARGET_FILE_NAME:XrdClUCache>
          $<TARGET_FILE_DIR:XrdClUCache>/libXrdClUCache.so
  VERBATIM)

# ucache-netbench: origin random-read baseline. Lives here (not in
# src/cli) because it needs XrdCl, which the CLI deliberately does not link.
find_package(Threads REQUIRED)
add_executable(ucache-netbench NetBench.cc)
target_include_directories(ucache-netbench SYSTEM PRIVATE ${XRDCL_INCLUDE_DIR})
target_link_libraries(ucache-netbench PRIVATE ${XRDCL_LIBRARY} Threads::Threads)
set_target_properties(ucache-netbench PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
install(TARGETS ucache-netbench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
