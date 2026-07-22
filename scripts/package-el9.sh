#!/usr/bin/env bash
# Build the prebuilt el9 artifacts: xrd-ucache-<v>-1.el9.x86_64.rpm + tarball.
#
# Deliberately does NOT use an LCG toolchain: packages must be built with the
# HOST toolchain (system gcc/cmake) so the binaries need only stock-EL9
# libstdc++/glibc. XRootD headers: host xrootd-client-devel when present
# (canonical, matches USER_GUIDE §1), else the pinned LCG install — headers
# only; either way the linked soname is the same libXrdCl.so.3 that EPEL's
# xrootd-client-libs provides, and the ABI floor (>= 5.6) is enforced at
# configure time by cmake/XRootDPin.cmake.
#
# lz4 is intentionally absent (matches every gated build to date): the
# transposer refuses LZ4-compressed *source* payloads — fail-open, page cache
# unaffected. Rebuild from source with lz4-devel if you need L4-source decode.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=${UCACHE_PKG_BUILD_DIR:-build-pkg}
CMAKE=/usr/bin/cmake
CPACK=/usr/bin/cpack
[ -x "$CMAKE" ] && [ -x "$CPACK" ] || { echo "need system cmake+cpack (dnf install cmake)"; exit 1; }
[ -x /usr/bin/g++ ] || { echo "need system g++ (dnf install gcc-c++)"; exit 1; }
command -v rpmbuild >/dev/null || { echo "need rpmbuild (dnf install rpm-build)"; exit 1; }

# The artifact must be built against the OLDEST supported XrdCl headers (the
# 5.6 ABI floor), NOT the newest available: XrdCl's plug-in handshake accepts
# a plugin only when the plugin's build version <= the client's. Verified
# live: a 5.8.3-built plugin is REFUSED by CMSSW's 5.6.4
# client ("Plugin version client v5.6.4 is incompatible ... must be <=
# 5.6.x") and the job silently runs uncached. Headers/link-lib only — no
# runtime path leaks into the artifacts: INSTALL_RPATH is $ORIGIN and deps
# are recorded by soname (libXrdCl.so.3, identical across 5.6-5.9).
XRD_FLOOR=/cvmfs/cms.cern.ch/el9_amd64_gcc12/external/xrootd/5.6.4-9891f6fd76a9cee982d9ea3c7ac53fcd
XRDROOT_ARGS=()
if [ -n "${UCACHE_PKG_XROOTD_ROOT:-}" ]; then
  XRDROOT_ARGS=(-DUCACHE_XROOTD_ROOT="$UCACHE_PKG_XROOTD_ROOT")
elif [ -e "$XRD_FLOOR/include/xrootd/XrdCl/XrdClPlugInInterface.hh" ]; then
  XRDROOT_ARGS=(-DUCACHE_XROOTD_ROOT="$XRD_FLOOR")
else
  echo "WARNING: 5.6 floor headers unavailable (no CVMFS?) — set UCACHE_PKG_XROOTD_ROOT"
  echo "to a 5.6.x install, or the artifact will be refused by older XrdCl clients."
  if [ -e /usr/include/xrootd/XrdCl/XrdClPlugInInterface.hh ]; then
    XRDROOT_ARGS=(-DUCACHE_XROOTD_ROOT=/usr)
  fi
fi

env -u LD_LIBRARY_PATH -u CC -u CXX \
  "$CMAKE" -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DUCACHE_BUILD_BENCH=OFF -DUCACHE_BUILD_TESTS=OFF \
  "${XRDROOT_ARGS[@]}"
env -u LD_LIBRARY_PATH "$CMAKE" --build "$BUILD" -j"$(nproc)"
rm -f "$BUILD"/xrd-ucache-*.rpm "$BUILD"/xrd-ucache-*.tar.gz  # stale versions would be re-copied below

# Hard gate: the shipped plugin must carry a 5.6.x handshake version, or old
# clients (CMSSW externals) will refuse it — exactly the bug this pin fixes.
PLUGVER=$(strings "$BUILD/src/plugin/libXrdClUCache.so" \
          | grep -oE "@V:XrdClUCache v[0-9.]+" | head -1 | grep -oE "v[0-9.]+" || true)
case "${UCACHE_PKG_XROOTD_ROOT:+skip}$PLUGVER" in
  skip*) echo "NOTE: custom UCACHE_PKG_XROOTD_ROOT — plugin handshake version: $PLUGVER" ;;
  v5.6.*) echo "plugin handshake version: $PLUGVER (5.6 floor — accepted by all >=5.6 clients)" ;;
  *) echo "FATAL: plugin handshake version '$PLUGVER' is not the 5.6 floor"; exit 1 ;;
esac

# Host quirk guard: rpm's brp-ldconfig hardcodes /sbin/ldconfig, which some
# hosts (this dev box) lack; the script is a buildroot no-op for us anyway.
RPM_ARGS=()
[ -e /sbin/ldconfig ] || RPM_ARGS=(-D 'CPACK_RPM_SPEC_MORE_DEFINE=%define __brp_ldconfig %{nil}')
(cd "$BUILD" && "$CPACK" -G RPM "${RPM_ARGS[@]}" && "$CPACK" -G TGZ)

mkdir -p dist
cp -v "$BUILD"/xrd-ucache-*.rpm "$BUILD"/xrd-ucache-*.tar.gz dist/
echo "artifacts in dist/:"
ls -l dist/
