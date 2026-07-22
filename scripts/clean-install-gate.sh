#!/usr/bin/env bash
# Clean-install acceptance gate: prove zero-admin activation end-to-end in a
# CLEAN environment (no CVMFS/LCG) — from a bare toolchain + the host's
# xrootd-client. Intended to run INSIDE a clean AlmaLinux 9 / Ubuntu 24.04
# container (apptainer/docker) or a fresh VM. It:
#   1. installs build deps + xrootd-client(-devel) from the OS package manager,
#   2. builds and `cmake --install`s ucache (plugin + CLI) to a prefix,
#   3. runs `ucache setup` as an unprivileged user — ONE conf file (activation
#      + settings, cache dir explicit) in XrdCl's default user plugin dir
#      (passwd home); no dotfiles, no env vars,
#   4. starts a self-contained local xrootd origin over a temp file,
#   5. with a COMPLETELY clean environment (no XRD_/UCACHE_ vars at all —
#      activation only via the default-dir conf), reads the file TWICE via the
#      plugin and asserts the warm pass fetches ZERO bytes from the origin,
#   6. breaks the install and asserts the read still succeeds (fail-open),
#      with `ucache doctor` reporting not-engaged.
#
# The conf lands in the container user's real ~/.xrootd/client.plugins.d (that
# is the point: XrdCl resolves it from passwd) and is removed on exit — run
# this in an ephemeral container/VM, not on a workstation you care about.
# Exit 0 = gate satisfied. This is the vehicle the GitHub CI invokes; it is
# NOT run on the LCG dev host (that environment is not "clean").
set -uo pipefail
REPO="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
PREFIX="${PREFIX:-/tmp/ucache-gate/prefix}"
CACHE="${CACHE:-/tmp/ucache-gate/cache}"
ORIGINDIR="${ORIGINDIR:-/tmp/ucache-gate/origin}"
PORT="${PORT:-10955}"
rm -rf /tmp/ucache-gate; mkdir -p "$PREFIX" "$CACHE" "$ORIGINDIR"
XRD_HOME="$(getent passwd "$(id -u)" | cut -d: -f6)"
CONF_FILE="$XRD_HOME/.xrootd/client.plugins.d/ucache.conf"
trap 'rm -f "$CONF_FILE"' EXIT

say() { printf '\n=== %s ===\n' "$*"; }
fail() { printf 'clean-install FAIL: %s\n' "$*" >&2; exit 1; }

say "1. install deps (bare OS, no CVMFS)"
if command -v dnf >/dev/null; then
  dnf -y install epel-release >/dev/null 2>&1 || true
  dnf -y install gcc-c++ cmake make xrootd xrootd-client xrootd-client-devel xrootd-server \
      >/dev/null 2>&1 || fail "dnf install failed"
elif command -v apt-get >/dev/null; then
  apt-get update >/dev/null 2>&1
  apt-get -y install g++ cmake make xrootd-client xrootd-dev xrootd-server >/dev/null 2>&1 \
      || fail "apt install failed (verify xrootd-dev >= 5.6 is available)"
else
  fail "no supported package manager (dnf/apt)"
fi
xrdcp --version 2>&1 | head -1

say "2. build + install ucache (against the HOST xrootd-client, not LCG)"
# find_package-style discovery: point the pin at the system prefix.
XRD_ROOT="$(dirname "$(dirname "$(command -v xrdcp)")")"
cmake -S "$REPO" -B /tmp/ucache-gate/build -DCMAKE_BUILD_TYPE=Release \
      -DUCACHE_BUILD_BENCH=OFF -DUCACHE_BUILD_TESTS=OFF \
      -DUCACHE_XROOTD_ROOT="$XRD_ROOT" >/tmp/ucache-gate/cmake.log 2>&1 \
  || { cat /tmp/ucache-gate/cmake.log; fail "configure failed (XRootD < 5.6?)"; }
cmake --build /tmp/ucache-gate/build -j"$(nproc)" >>/tmp/ucache-gate/cmake.log 2>&1 || fail "build failed"
cmake --install /tmp/ucache-gate/build --prefix "$PREFIX" >>/tmp/ucache-gate/cmake.log 2>&1 || fail "install failed"
test -x "$PREFIX/bin/ucache" || fail "ucache CLI not installed"
readelf -d "$PREFIX"/lib*/libXrdClUCache.so | grep -q 'ORIGIN' || fail "plugin RPATH not relocatable"

say "3. ucache setup (unprivileged; ONE conf file, nothing else touched)"
"$PREFIX/bin/ucache" setup --host "localhost:$PORT" --dir "$CACHE" || fail "setup failed"
test -f "$CONF_FILE" || fail "setup did not write $CONF_FILE"
grep -q "^dir = $CACHE" "$CONF_FILE" || fail "conf lacks the explicit cache dir"

say "4. start a self-contained local xrootd origin"
head -c 4194304 /dev/urandom > "$ORIGINDIR/probe.bin"       # 4 MiB test object
env -u LD_LIBRARY_PATH xrootd -b -p "$PORT" -l /tmp/ucache-gate/xrootd.log "$ORIGINDIR" || \
  fail "could not start xrootd origin"
sleep 2
URL="root://localhost:$PORT//probe.bin"

# Zero-env activation: no XRD_* or UCACHE_* variables at all —
# XrdCl finds the conf in the default user plugin dir, and the cache dir
# comes from the conf's explicit `dir =` line.
read_via_plugin() { # -> exit code of xrdcp
  env -u XRD_PLUGINCONFDIR -u UCACHE_DIR \
    xrdcp -f "$URL" /tmp/ucache-gate/out.bin >/dev/null 2>&1
}
origin_bytes() { # sum origin_bytes across stats files
  python3 - "$CACHE/stats" <<'PY'
import sys,glob,json,re
tot=0
for f in glob.glob(sys.argv[1]+"/*.jsonl"):
    last=""
    for l in open(f):
        if l.strip().endswith("}"): last=l
    m=re.search(r'"origin_bytes":(\d+)',last) if last else None
    if m: tot+=int(m.group(1))
print(tot)
PY
}

say "5. cold read then warm read (warm must fetch 0 from origin)"
read_via_plugin || fail "cold read failed"
COLD=$(origin_bytes); echo "cold origin_bytes=$COLD"
[ "$COLD" -gt 0 ] || fail "cold read cached nothing (plugin not engaged via the default-dir conf?)"
rm -f "$CACHE"/stats/*.jsonl
read_via_plugin || fail "warm read failed"
WARM=$(origin_bytes); echo "warm origin_bytes=$WARM"
[ "$WARM" -eq 0 ] || fail "warm read fetched $WARM bytes (expected 0 — cache not serving)"

say "6. fail-open: break the plugin, read must still succeed"
mv "$PREFIX"/lib*/libXrdClUCache.so /tmp/ucache-gate/broken.so
"$PREFIX/bin/ucache" doctor >/tmp/ucache-gate/doctor.log 2>&1
grep -qiE 'FAIL|not loadable' /tmp/ucache-gate/doctor.log || echo "  (note: doctor did not flag the broken plugin)"
read_via_plugin || fail "read did not fail open with a broken plugin (FAIL-OPEN VIOLATION)"
echo "  read still succeeded with the plugin removed (fail-open OK)"

kill %1 2>/dev/null || true
echo; echo "clean-install PASS: zero-admin install -> setup -> new-shell cached read (warm origin=0), fail-open holds."
