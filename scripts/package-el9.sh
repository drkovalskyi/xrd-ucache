#!/usr/bin/env bash
# Build the prebuilt el9 artifacts: xrd-ucache-<v>-1.el9.x86_64.rpm + tarball.
#
# Deliberately does NOT use an LCG toolchain: packages must be built with the
# HOST toolchain (system gcc/cmake) so the binaries need only stock-EL9
# libstdc++/glibc. XRootD headers: host xrootd-client-devel when present
# (canonical, matches USER_GUIDE §1), else the pinned LCG install — headers
# only; the ABI floor (>= 5.6) is enforced at configure time by
# cmake/XRootDPin.cmake.
#
# The plugin is built TWICE, once per XRootD major, and both builds ship:
# libXrdClUCache-5.so (against 5.6) and libXrdClUCache-6.so (against 6.0). A
# conf naming libXrdClUCache.so makes each client open the file for its own
# major. The package requires no XRootD at all (cmake/Packaging.cmake).
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
# The same rule for the XRootD 6 build: 6.0, the first 6.x. The handshake
# compares major and minor only, so any 6.0.x patch release is the same floor.
XRD6_FLOOR=/cvmfs/cms.cern.ch/el9_amd64_gcc13/external/xrootd/6.0.2-16d7210c72f2d44b713bbd3a73c3f91b
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
if [ -n "${UCACHE_PKG_XROOTD6_ROOT:-}" ]; then
  XRD6=$UCACHE_PKG_XROOTD6_ROOT
elif [ -e "$XRD6_FLOOR/include/xrootd/XrdCl/XrdClPlugInInterface.hh" ]; then
  XRD6=$XRD6_FLOOR
else
  echo "FATAL: no XRootD 6.0 client to build the second plugin against (no CVMFS?) —"
  echo "       set UCACHE_PKG_XROOTD6_ROOT to a 6.0.x install (headers + libXrdCl.so.6)."
  exit 1
fi
XRDROOT_ARGS+=(-DUCACHE_XROOTD_EXTRA_ROOT="$XRD6")

env -u LD_LIBRARY_PATH -u CC -u CXX -u CMAKE_PREFIX_PATH -u CMAKE_MODULE_PATH \
  "$CMAKE" -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DUCACHE_BUILD_BENCH=OFF -DUCACHE_BUILD_TESTS=OFF \
  "${XRDROOT_ARGS[@]}"
env -u LD_LIBRARY_PATH "$CMAKE" --build "$BUILD" -j"$(nproc)"
rm -f "$BUILD"/xrd-ucache-*.rpm "$BUILD"/xrd-ucache-*.tar.gz  # stale versions would be re-copied below

# Hard gate: each shipped plugin must carry its major's floor handshake
# version (5.6.x, 6.0.x), or older clients of that major (CMSSW externals)
# refuse it — exactly the bug this pin fixes. And each must link its own
# major's client library: a build that picked up the wrong headers would
# compile just as happily and be refused at run time.
plugver() {
  strings "$1" | grep -oE "@V:XrdClUCache v[0-9.]+" | head -1 | grep -oE "v[0-9.]+" || true
}
for want in "5 v5.6. libXrdCl.so.3 ${UCACHE_PKG_XROOTD_ROOT:+custom}" \
            "6 v6.0. libXrdCl.so.6 ${UCACHE_PKG_XROOTD6_ROOT:+custom}"; do
  read -r major floor soname custom <<<"$want"
  so="$BUILD/src/plugin/libXrdClUCache-$major.so"
  [ -f "$so" ] || { echo "FATAL: $so was not built"; exit 1; }
  v=$(plugver "$so")
  needed=$(objdump -p "$so" | grep NEEDED || true) # not `| grep -q`: SIGPIPE under pipefail
  case "$needed" in
    *"$soname"*) ;;
    *) echo "FATAL: $so does not link $soname; it needs:"; echo "$needed"; exit 1 ;;
  esac
  if [ -n "$custom" ]; then
    echo "NOTE: custom XRootD $major root — libXrdClUCache-$major.so handshake version: $v"
  else
    case "$v" in
      "$floor"*) echo "libXrdClUCache-$major.so: handshake $v, links $soname (floor — accepted by every $major.x client)" ;;
      *) echo "FATAL: libXrdClUCache-$major.so handshake version '$v' is not the ${floor%.} floor"; exit 1 ;;
    esac
  fi
done

# Hard gate: the installed docs must be CLOSED under their own pointers. A
# shipped guide that says "see docs/X.md" for a file the package omits is a
# dead end precisely where the reader cannot fetch it — v0.18.4 shipped two
# such pointers, and nothing noticed until a kit was unpacked by hand.
# Stage a FRESH install rather than probing the build tree: a leftover doc dir
# from an earlier run is stale by construction, and an empty or absent one makes
# the loop below pass with nothing to check — which is how the first version of
# this gate went green on a package that was already broken.
DOCSTAGE=$(mktemp -d)
env -u LD_LIBRARY_PATH "$CMAKE" --install "$BUILD" --prefix "$DOCSTAGE/usr" >/dev/null \
  || { echo "FATAL: staging install for the doc gate failed"; exit 1; }
DOCDIR=$(find "$DOCSTAGE" -type d -name xrd-ucache -path '*/doc/*' | head -1)
[ -n "$DOCDIR" ] && [ -e "$DOCDIR/USER_GUIDE.md" ] \
  || { echo "FATAL: doc gate found no installed docs (staged in $DOCSTAGE)"; exit 1; }
MISSINGDOC=""
for ref in $(grep -oh 'docs/[A-Za-z_]*\.md' "$DOCDIR"/*.md 2>/dev/null | sort -u); do
  [ -e "$DOCDIR/$(basename "$ref")" ] || MISSINGDOC="$MISSINGDOC $ref"
done
if [ -n "$MISSINGDOC" ]; then
  echo "FATAL: installed docs point at files the package does not ship:$MISSINGDOC"
  echo "       add them to install(FILES ...) in cmake/Packaging.cmake, or stop citing them"
  exit 1
fi
echo "installed docs are closed under their own pointers ($(ls "$DOCDIR"/*.md | wc -l) docs)"
# Hard gate: the recommended configuration the package ships names the plugin
# where the RPM puts it. The staged install above is laid out as the RPM (its
# prefix is /usr). The conf names the plain libXrdClUCache.so, which a client
# turns into libXrdClUCache-<its major>.so, so both of those must exist — and
# the plain file must NOT: it is what a client of any other major falls back
# to, and it would refuse it rather than find nothing.
REC="$DOCSTAGE/usr/share/xrd-ucache/ucache.conf"
RECLIB=$(sed -n 's/^lib = //p' "$REC" 2>/dev/null)
[ -n "$RECLIB" ] || { echo "FATAL: the shipped ucache.conf names no lib ="; exit 1; }
for major in 5 6; do
  [ -e "$DOCSTAGE${RECLIB%.so}-$major.so" ] \
    || { echo "FATAL: the shipped ucache.conf names lib = '$RECLIB', but the package does not install ${RECLIB%.so}-$major.so"; exit 1; }
done
[ ! -e "$DOCSTAGE$RECLIB" ] || { echo "FATAL: the package installs the plain $RECLIB"; exit 1; }
echo "the shipped ucache.conf names the packaged plugins ($RECLIB -> -5.so, -6.so)"
rm -rf "$DOCSTAGE"

(cd "$BUILD" && "$CPACK" -G RPM && "$CPACK" -G TGZ)

# Hard gate: the RPM requires no XRootD (cmake/Packaging.cmake says why) and
# carries both plugin builds and no plain-named one.
RPMFILE=$(ls "$BUILD"/xrd-ucache-*.rpm)
if rpm -qp --requires "$RPMFILE" 2>/dev/null | grep -i xrdcl; then
  echo "FATAL: the RPM requires an XRootD client library (above) — it must not"; exit 1
fi
RPMLIST=$(rpm -qpl "$RPMFILE" 2>/dev/null)
for f in libXrdClUCache-5.so libXrdClUCache-6.so; do
  grep -q "/$f\$" <<<"$RPMLIST" || { echo "FATAL: the RPM does not carry $f"; exit 1; }
done
! grep -q '/libXrdClUCache[.]so$' <<<"$RPMLIST" || { echo "FATAL: the RPM carries a plain libXrdClUCache.so"; exit 1; }
echo "the RPM requires no XRootD and carries libXrdClUCache-5.so and libXrdClUCache-6.so"

mkdir -p dist
cp -v "$BUILD"/xrd-ucache-*.rpm "$BUILD"/xrd-ucache-*.tar.gz dist/
echo "artifacts in dist/:"
ls -l dist/
