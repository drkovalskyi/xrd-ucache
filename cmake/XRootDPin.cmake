# Single source of truth for the pinned XRootD versions.
# CLIENT stack (what the plugin builds and links against; plugin-ABI target):
#   LCG_108 x86_64-el9-gcc14-opt — XRootD 5.8.3, ROOT 6.36.02, gcc 14.2.
set(UCACHE_XROOTD_VERSION "5.8.3")
set(UCACHE_XROOTD_ROOT
    "/cvmfs/sft.cern.ch/lcg/releases/xrootd/5.8.3-1e12b/x86_64-el9-gcc14-opt"
    CACHE PATH "XRootD client install (headers + libXrdCl)")
# SERVER side of the bench harness (origin + XCache instances): host 5.9.6.
set(UCACHE_XROOTD_SERVER_VERSION "5.9.6")
# Minimum acceptable for building/linking.
set(UCACHE_XROOTD_MINIMUM "5.6")

# Fail configure if the discovered XRootD client is older than the plugin-ABI
# minimum — the compile-time half of fail-open: a too-old XrdCl
# must fail loudly at build, not silently at runtime. Called from the plugin
# CMakeLists with the found include dir, so core-only builds don't require XRootD.
function(ucache_require_xrootd_version include_dir)
  set(_hdr "${include_dir}/XrdVersion.hh")
  if(NOT EXISTS "${_hdr}")
    message(FATAL_ERROR "ucache: XrdVersion.hh not found under ${include_dir}")
  endif()
  file(STRINGS "${_hdr}" _lines REGEX "define[ \t]+XrdVERSION")
  set(_found "")
  foreach(_l IN LISTS _lines)
    if(_l MATCHES "\"v?([0-9]+\\.[0-9]+\\.[0-9]+)")
      set(_found "${CMAKE_MATCH_1}")
      break()
    endif()
  endforeach()
  if(NOT _found)
    message(WARNING "ucache: could not parse XRootD version from ${_hdr}; skipping min-version gate")
    return()
  endif()
  if(_found VERSION_LESS "${UCACHE_XROOTD_MINIMUM}")
    message(FATAL_ERROR
      "ucache: XRootD client ${_found} is older than the required ${UCACHE_XROOTD_MINIMUM} "
      "(plugin ABI). Install a newer xrootd-client-devel or set UCACHE_XROOTD_ROOT.")
  endif()
  message(STATUS "ucache: XRootD client ${_found} (>= ${UCACHE_XROOTD_MINIMUM} required) — OK")
endfunction()
