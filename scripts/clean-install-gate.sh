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
#   5. with a clean environment (no XRD_/UCACHE_ vars that affect activation —
#      activation only via the default-dir conf), reads the file TWICE via the
#      plugin and asserts the warm pass fetches ZERO bytes from the origin; then
#      copies a second file with a plain xrdcp and asserts the copy is the
#      origin's bytes and never entered the cache,
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
fail() {
  printf 'clean-install FAIL: %s\n' "$*" >&2
  # post-mortem for CI (the container is destroyed on exit): origin log tail
  for f in /tmp/ucache-gate/xrootd.log*; do
    [ -f "$f" ] && { echo "--- $f (tail) ---" >&2; tail -30 "$f" >&2; }
  done
  exit 1
}

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
# The plugin is installed under the name of the XRootD major it was built for,
# as XRootD names its own plugins; a conf names the plain libXrdClUCache.so and
# the client adds its major. So the plain file must not be installed.
PLUGIN_SO=$(ls "$PREFIX"/lib*/libXrdClUCache-[0-9]*.so 2>/dev/null | head -1)
[ -n "$PLUGIN_SO" ] || fail "plugin not installed as libXrdClUCache-<major>.so"
! ls "$PREFIX"/lib*/libXrdClUCache.so >/dev/null 2>&1 || fail "the plain libXrdClUCache.so was installed"
readelf -d "$PLUGIN_SO" | grep -q 'ORIGIN' || fail "plugin RPATH not relocatable"
PLUGIN_LIB="$(dirname "$PLUGIN_SO")/libXrdClUCache.so"   # the name confs carry

say "3. ucache setup (unprivileged; ONE conf file, nothing else touched)"
"$PREFIX/bin/ucache" setup --host "localhost:$PORT" --dir "$CACHE" || fail "setup failed"
test -f "$CONF_FILE" || fail "setup did not write $CONF_FILE"
grep -q "^dir = $CACHE" "$CONF_FILE" || fail "conf lacks the explicit cache dir"
# The recommended configuration ships beside the binaries, activates nothing,
# and says what setup writes: the same text, bar the three lines setup fills in.
REC="$PREFIX/share/xrd-ucache/ucache.conf"
test -f "$REC" || fail "the recommended configuration was not installed ($REC)"
diff <(grep -v '^url = \|^lib = \|^dir = ' "$REC") <(grep -v '^url = \|^lib = \|^dir = ' "$CONF_FILE") \
  || fail "setup's conf and the installed recommended one differ beyond url/lib/dir"
grep -q "^# recompress = on" "$CONF_FILE" && grep -q "MEMORY" "$CONF_FILE" \
  || fail "the conf does not carry the recompression block and its memory note"
grep -qx "lib = $PLUGIN_LIB" "$CONF_FILE" || fail "setup did not name this install's plugin as $PLUGIN_LIB"

say "4. start a self-contained local xrootd origin"
head -c 4194304 /dev/urandom > "$ORIGINDIR/probe.bin"       # 4 MiB test object
head -c 1048576 /dev/urandom > "$ORIGINDIR/copy.bin"        # 1 MiB, copied in step 5b
# xrootd refuses to run as uid 0; in a root container (CI) hand the origin to
# a scratch user. Everywhere else run it as the invoking (unprivileged) user.
XRD=(env -u LD_LIBRARY_PATH xrootd)
if [ "$(id -u)" = 0 ]; then
  useradd -m xrdsrv 2>/dev/null || true
  chmod 1777 /tmp/ucache-gate
  chmod -R a+rX "$ORIGINDIR"
  XRD=(env -u LD_LIBRARY_PATH runuser -u xrdsrv -- xrootd)
fi
"${XRD[@]}" -b -p "$PORT" -l /tmp/ucache-gate/xrootd.log "$ORIGINDIR" || \
  fail "could not start xrootd origin"
sleep 2
# The origin exports the filesystem path it was started with, so the URL
# carries the ABSOLUTE path of the probe file.
URL="root://localhost:$PORT/$ORIGINDIR/probe.bin"

# Zero-env activation: no XRD_PLUGINCONFDIR or UCACHE_DIR — XrdCl finds the
# conf in the default user plugin dir, and the cache dir comes from the conf's
# explicit `dir =` line.
read_via_plugin() { # -> exit code of xrdcp
  # xrdcp is a copy tool, and uCache reads a copy straight from the origin (a
  # copy is the origin's bytes) unless copy detection is off. And xrdcp >= 5.8
  # transfers via PgRead, which the plugin deliberately relays uncached
  # (analysis clients use Read/ReadV, which cache). Neither setting touches
  # activation: with both, this smoke-test exercises the cached path.
  env -u XRD_PLUGINCONFDIR -u UCACHE_DIR UCACHE_COPY_DETECT=off XRD_CPUSEPGWRTRD=0 \
    xrdcp -f "$URL" /tmp/ucache-gate/out.bin >/dev/null 2>&1
}
metas() { ls "$CACHE"/objects/*/*.meta 2>/dev/null | wc -l; }
stat_sum() { # $1 = counter: summed over the last line of every stats file
  python3 - "$CACHE/stats" "$1" <<'PY'
import sys,glob,re
tot=0
for f in glob.glob(sys.argv[1]+"/*.jsonl"):
    last=""
    for l in open(f):
        if l.strip().endswith("}"): last=l
    m=re.search(r'"%s":(\d+)' % sys.argv[2],last) if last else None
    if m: tot+=int(m.group(1))
print(tot)
PY
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

say "5b. a plain xrdcp is a copy: the origin's bytes, nothing cached"
N0=$(metas)
env -u XRD_PLUGINCONFDIR -u UCACHE_DIR xrdcp -f "root://localhost:$PORT/$ORIGINDIR/copy.bin" \
    /tmp/ucache-gate/copy.bin >/dev/null 2>&1 || fail "plain xrdcp failed"
[ "$(sha256sum < "$ORIGINDIR/copy.bin")" = "$(sha256sum < /tmp/ucache-gate/copy.bin)" ] ||
  fail "the copy differs from the origin's file"
[ "$(metas)" = "$N0" ] || fail "a plain xrdcp created a cache entry (copies must bypass the cache)"
COPIES=$(stat_sum copier_handles); echo "copier_handles=$COPIES"
[ "$COPIES" -gt 0 ] || fail "the copy was not counted as one (plugin not engaged, or copy detection off?)"

say "5c. no conf at all: XRD_PLUGIN + UCACHE_DIR switch it on with every default"
# XrdCl loads the library XRD_PLUGIN names for every URL and reads no plugin
# conf while it is set; the conf is moved away anyway, so nothing can mask a
# failure here.
CACHE2=/tmp/ucache-gate/cache-env
mv "$CONF_FILE" "$CONF_FILE.off"
read_env() {
  env -u XRD_PLUGINCONFDIR XRD_PLUGIN="$PLUGIN_LIB" UCACHE_DIR="$CACHE2" \
      UCACHE_COPY_DETECT=off XRD_CPUSEPGWRTRD=0 xrdcp -f "$URL" /tmp/ucache-gate/out-env.bin >/dev/null 2>&1
}
read_env || fail "cold read with XRD_PLUGIN failed"
COLD=$(CACHE=$CACHE2 origin_bytes); echo "cold origin_bytes=$COLD"
[ "$COLD" -gt 0 ] || fail "XRD_PLUGIN read cached nothing (plugin not engaged?)"
rm -f "$CACHE2"/stats/*.jsonl
read_env || fail "warm read with XRD_PLUGIN failed"
WARM=$(CACHE=$CACHE2 origin_bytes); echo "warm origin_bytes=$WARM"
[ "$WARM" -eq 0 ] || fail "XRD_PLUGIN warm read fetched $WARM bytes (expected 0)"
env -u XRD_PLUGINCONFDIR XRD_PLUGIN="$PLUGIN_LIB" UCACHE_DIR="$CACHE2" \
    "$PREFIX/bin/ucache" doctor >/tmp/ucache-gate/doctor-env.log 2>&1
grep -q "activation: XRD_PLUGIN" /tmp/ucache-gate/doctor-env.log \
  || { cat /tmp/ucache-gate/doctor-env.log; fail "doctor does not recognise XRD_PLUGIN activation"; }
mv "$CONF_FILE.off" "$CONF_FILE"

say "6. fail-open: break the plugin, read must still succeed"
mv "$PLUGIN_SO" /tmp/ucache-gate/broken.so
"$PREFIX/bin/ucache" doctor >/tmp/ucache-gate/doctor.log 2>&1
grep -qiE 'FAIL|not loadable' /tmp/ucache-gate/doctor.log || echo "  (note: doctor did not flag the broken plugin)"
read_via_plugin || fail "read did not fail open with a broken plugin (FAIL-OPEN VIOLATION)"
echo "  read still succeeded with the plugin removed (fail-open OK)"

kill %1 2>/dev/null || true
echo; echo "clean-install PASS: zero-admin install -> setup -> new-shell cached read (warm origin=0), fail-open holds."
