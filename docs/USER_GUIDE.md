# uCache — User Guide

`ucache` is a transparent, per-user read cache for remote ROOT/XRootD data. You
install it once; thereafter every `root://…` file your jobs read is cached on
local disk, so the **second and later** passes over the same data read from your
NVMe instead of the network. Nothing in your analysis code changes.

It is safe by construction: if anything goes wrong (bad cache dir, disk full,
version mismatch, corruption) it **fails open** — your job still runs correctly,
just without the speedup.

## 1. Install (no root — like everything else here)

Unpack the release tarball into your home:

```sh
mkdir -p ~/.local
tar -C ~/.local --strip-components=1 -xf xrd-ucache-<version>-el9-x86_64.tar.gz
ucache doctor      # expect FAILs for cache dir + activation — §2 fixes both;
                   # the `plugin loads` line is what confirms the install
```

That's the whole install: `bin/ucache`, `bin/ucache-netbench` and
`lib64/libXrdClUCache.so` under `~/.local`. On EL9 `~/.local/bin` is on `PATH` by default; if `ucache` is
not found, add `export PATH="$HOME/.local/bin:$PATH"` to your shell startup
file (`~/.bashrc`, `~/.zshrc`, …). Any other prefix works too — nothing
cares where the files live. Continue with §2 (activation — one config
file). Ask the maintainer for the current tarball until the project has a
public download page.

The one runtime dependency is the XRootD 5 client library — and any machine
that already reads `root://` URLs has it (CMSSW, LCG/CVMFS ROOT, or EPEL's
`xrootd-client`). The plugin never links ROOT; it binds to whatever XrdCl
**5.6–5.9** the process already uses, and where it can't load (CentOS 7-era
releases under apptainer, xrootd 6 stacks) it fails open — the job just runs
uncached.

### Build from source — AlmaLinux 9 / RHEL 9

Install the toolchain and the XRootD **client** headers. `xrootd*` comes from
EPEL, so enable it first. None of this needs to be the same machine that later
runs your jobs — it just needs the headers to build against.

```sh
sudo dnf -y install epel-release
sudo dnf -y install gcc-c++ cmake make \
     xrootd-client xrootd-client-devel \
     xz-devel zlib-devel libzstd-devel lz4-devel     # codec libs (see note)
```

- `xrootd-client-devel` provides `libXrdCl` + headers (must be ≥ 5.6; EPEL 9
  ships 5.6+). Its headers live under `/usr/include/xrootd`, which is why the
  build needs `-DUCACHE_XROOTD_ROOT=/usr` below.
- The four `*-devel` codec packages are only needed for the **recompression**
  feature (`ucache recompress`, see §Configuration). Omit them
  and everything else still builds — the page cache works fully; the replica
  tier is simply skipped (fail-open at build time). `lz4-devel` is optional even
  among those (it only lets the transposer *read* LZ4-compressed inputs).

Build against the **host** XRootD (point the version pin at `/usr`) and install
to a user prefix — no root needed for the build/install itself:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DUCACHE_BUILD_BENCH=OFF -DUCACHE_BUILD_TESTS=OFF \
      -DUCACHE_XROOTD_ROOT=/usr
cmake --build build -j"$(nproc)"
cmake --install build --prefix ~/.local        # or a system prefix (needs write access)
```

`configure` prints `ucache: XRootD client <version> (>= 5.6 required) — OK`; if
it instead can't find `XrdCl/XrdClPlugInInterface.hh`, `xrootd-client-devel`
isn't installed or `-DUCACHE_XROOTD_ROOT` doesn't point at its prefix. This
installs the plugin `lib64/libXrdClUCache.so` and the `ucache` CLI to `bin/`.
Put the prefix's `bin` on your `PATH` if it isn't already.

> **Note:** `-DUCACHE_XROOTD_ROOT=/usr` is required on a stock EL9 box — the
> default points at the CERN CVMFS/LCG build used for development, which a
> normal host does not have. Set it to wherever `xrootd-client-devel` landed
> (`$(dirname "$(dirname "$(command -v xrdcp)")")` resolves it).

### Other distributions

The only distro-specific part is the XRootD client package; the rest is the same
`cmake` invocation with `-DUCACHE_XROOTD_ROOT` pointed at its prefix (usually
`/usr`). On Debian/Ubuntu: `apt-get install g++ cmake make xrootd-dev
liblzma-dev zlib1g-dev libzstd-dev liblz4-dev` (verify `xrootd-dev` ≥ 5.6). Any
Linux with a C++17 compiler, CMake ≥ 3.20, and XRootD client ≥ 5.6 works.

### Build from source — macOS

There is no prebuilt macOS package, and there will not be one: a downloaded
dylib carries Gatekeeper quarantine, and quarantine blocks the `dlopen` the
XRootD client uses to load the plugin. Building it locally avoids that
entirely.

The requirements are the same as anywhere else — a C++17 compiler, CMake >= 3.20,
an XRootD client >= 5.6 — plus one rule that matters more here: **build against
the XRootD client your ROOT actually loads.** Check which that is first:

```sh
root-config --has-xrootd                                       # must say yes
otool -L "$(root-config --libdir)/libNetxNG.so" | grep -i XrdCl
```

MacPorts is the tested route (Apple silicon, macOS 14). Command Line Tools are
enough — Xcode is not required.

```sh
sudo port install xrootd root6 +xrootd
sudo port install xz zstd lz4          # codec libs — recompression only, optional
```

```sh
export TMPDIR=/tmp          # do this FIRST — see the note below
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DUCACHE_BUILD_BENCH=OFF -DUCACHE_BUILD_TESTS=OFF \
      -DUCACHE_XROOTD_ROOT=/opt/local -DCMAKE_PREFIX_PATH=/opt/local
cmake --build build -j"$(sysctl -n hw.ncpu)"
cmake --install build --prefix ~/.local
```

> **Set `TMPDIR` before building.** With it unset, the compiler is handed a
> per-session `/var/folders/...` path it may not own, and AppleClang then fails to
> compile anything at all with "unable to make temporary file". Any writable
> directory works.

> **If both MacPorts and Homebrew are present**, CMake searches `/opt/homebrew`
> first on Apple silicon. The two prefix flags above are what keep the build on
> the same XrdCl your ROOT loads — confirm with `otool -L` rather than assuming.

Everything after this is identical to Linux: activate with the same plugin
configuration file (§2), and point the cache at an ordinary directory on a
volume with room to spare.

### For site administrators (optional)

Each release also ships `xrd-ucache-<version>-1.el9.x86_64.rpm` for a system-wide
install (`dnf install`, lands in `/usr/lib64` + `/usr/bin`; EPEL provides the
xrootd dependency). Nothing about uCache *needs* this — it exists for admins
who want one shared copy; users still activate per-user (§2). Maintainers:
`scripts/package-el9.sh` builds both artifacts into `dist/` with the host
toolchain (never an LCG shell), so they run on stock EL9 and inside CMSSW/LCG
environments alike.

## 2. Activate (one file, no root)

uCache is activated **and** configured by a single file: an XRootD plugin
config that tells XrdCl to load the plugin and carries all ucache settings —
starting with where the cache lives. Write it yourself, or let `ucache setup`
write the same file; nothing else is touched either way.

### Write the file

XrdCl scans `~/.xrootd/client.plugins.d/` automatically in every `root://`
process — no environment variable, no shell startup file to edit, no shell
to reload, and batch jobs pick it up too:

```sh
mkdir -p ~/.xrootd/client.plugins.d
cat > ~/.xrootd/client.plugins.d/ucache.conf <<EOF
url = *
lib = $HOME/.local/lib64/libXrdClUCache.so
enable = true

# ucache settings (UCACHE_* env vars override — see §Configuration)
dir = /data/$USER/ucache-cache
EOF
ucache doctor      # verify install, filesystem, activation; exits 0 when good
```

- `dir` — where the cached data lives. **Required, on purpose**: there is no
  default location (a default would silently land in your home directory,
  which on CERN machines is AFS — caching network data onto a network
  filesystem defeats the purpose). Use a **local** disk. Until it is set,
  `doctor` FAILs and the plugin runs uncached (fail-open).
- `lib` — the absolute path to wherever §1 put the plugin:
  `$HOME/.local/lib64/…` for the no-root tarball/source install (as shown;
  the unquoted heredoc expands `$HOME` when writing — the conf itself needs
  a literal absolute path, no `~`), or `/usr/lib64/libXrdClUCache.so` if an
  admin installed the RPM. If unsure, `ucache doctor` prints the resolved
  path on its `plugin loads` line.
- `url = *` intercepts every `root://` URL. If a system config in
  `/etc/xrootd/client.plugins.d/` already claims the `*` slot (`doctor`
  warns), list your data hosts explicitly instead:
  `url = eospublic.cern.ch:1094;xrootd.example.org:1094` (your experiment's
  data servers or redirectors).
- XrdCl resolves the `~` in `~/.xrootd/client.plugins.d` from the passwd
  database, not `$HOME` — they differ on some batch systems.
- Prefer the file elsewhere? XrdCl also processes `$XRD_PLUGINCONFDIR`
  (after `/etc/xrootd/client.plugins.d` and the default dir — last wins for
  the same `url`), so you can keep the conf in any directory and set that
  variable in your shell startup file; that route only works in shells (and
  jobs) where the variable is set.

Deactivation is equally boring: delete the file (or set `enable = false`).

However you configured things — this file, `XRD_PLUGINCONFDIR`, environment
variables, `setup` — **`ucache doctor` is the one verification step**: it
finds the governing conf exactly as XrdCl would, dlopens the very library
that conf names, checks the cache filesystem, and exits nonzero if anything
would stop caching (no conf, `enable = false`, a shadowed `url = *`, a stale
`lib =` path, an unset or unsuitable cache dir).

### Or: `ucache setup` (writes the same file)

```sh
ucache setup --host eospublic.cern.ch:1094 --dir /data/$USER/ucache-cache
ucache doctor
```

`setup` writes exactly the file above — plugin path resolved, cache dir
explicit — to `~/.xrootd/client.plugins.d/ucache.conf`, and touches nothing
else: no shell startup files, no environment variables, no second config.
`--dir` is required unless `UCACHE_DIR` or an existing conf already provides
a location (there is deliberately no default). Activation is immediate for
every `root://` process, batch jobs included.

## 3. Verify it really works, then run your analysis

**The standard self-test** — point it at any file your conf intercepts
(pick one of your usual data files; the whole file is transferred, so not
your biggest):

```sh
ucache test root://<your-host>//path/to/file.root
```

It uses your setup exactly as configured — no changes, no environment — and
runs a cold pass then a warm pass through a real XRootD client, with
progress shown. It PASSes only if the warm pass is served entirely from
cache with **zero origin contact**, then removes the entry it created (a
file that was already cached is verified warm-only and kept). Exit 0 = the
whole chain works: conf → plugin loaded → interception → caching → warm
serving.

Then run your normal ROOT / RDataFrame / uproot job **twice**:

```sh
python my_analysis.py     # cold: fetches the working set from the origin
python my_analysis.py     # warm: served from local cache
ucache stats              # warm pass shows origin_bytes == 0
```

(Don't smoke-test with plain `xrdcp` — it transfers with PgRead, which the
cache deliberately passes through, so `stats` stays at zero. If you must:
`XRD_CPUSEPGWRTRD=0 xrdcp …` uses the cached read path.)

`ucache status` shows where the cache lives, the disk budget, how much is cached,
and aggregate counters.

## Monitoring — start with `ucache summary`

`ucache summary` answers the question you actually have: **is this cache worth
having, and what has it saved me?** It reads records the plugin already wrote —
nothing needs to be running, and there is no sampling daemon to start.

```text
$ ucache summary
overall    : 8 run(s) recorded — 720.8 GiB served from cache, 266.1 GiB from the origin
  saved    : 35m22s — 4 run(s) took 27m14s, and would have taken about 1h02m with
             no cache (2.3x, vs a measured baseline)
  health   : OK — no faults in any recorded run
cache      : /scratch/ucache — 1456 entries, 202.8 GiB on disk, 1.3 TiB headroom
next       : `ucache summary --detail` for the last run; `ucache history` for the trend
```

**The headline is time, and it is measured against a baseline — the same work
run once with the cache out of the loop.** Record one by running your job with
`UCACHE_DISABLE=1` set; the plugin passes everything through untouched and
writes the run down like any other. From then on, cached runs over the same
files are compared against it — and the comparison is symmetric: **a cache that
is slower than your origin shows up as a cost, not a gain**:

```text
overall    : 7 run(s) recorded — 429.4 GiB served from cache, 222.8 GiB from the origin
  COST     : the cache is costing you time on this workload — 4 run(s) took 15m26s
             where no cache would have taken about 11m40s (0.8x, vs a measured baseline)
```

That is a real result from a real workload (a fast LAN origin and cheaply
compressed data): the honest answer there is to not use a cache, and the tool
says so.

### Why a baseline, and not an estimate

Nothing recorded during cached operation can stand in for the origin alone —
this was measured, twice, before being accepted. The cold fill's wall is set
partly by the cache's own writes (on the workload above, using it as the
reference reported a 1.7x *gain* where the measured truth was a 0.70x *loss*),
and service-time histograms record blocked waits, which overlap compute and are
not wall time (same test: 2.8x, same truth). A measured baseline reproduces the
true ratio to about 1% either way. One extra run of your job is what
truthfulness costs.

Rules the comparison enforces, so a number is never printed where it would
mislead: the baseline must cover ≥70% of the same files in both directions
(matched by URL); a run that mostly *filled* the cache is never scored; runs and
baselines under 30 s or 64 MiB are refused (too short to time at one-second
resolution); a baseline that hit faults is not used; with several baselines the
nearest in time wins — and recording a baseline *after* you have been running
cached for a while works fine. When the answer cannot be sound, `summary` says
why and what to do instead of printing a number.

`ucache summary --detail` adds the last run underneath: tier split, delivered
rate, per-tier read counts and sizes, replica coverage, and that run's own
comparison with its baseline.

## Trend across runs — `ucache history`

One row per run, newest first. This is where you see whether the numbers hold up
over time, across versions, or after you change the cache disk.

```text
$ ucache history
8 run(s) recorded, newest first (showing 8)
WHEN                  DUR   FILES     SERVED     ORIGIN       RATE  BYTE/REPL/RELAY  FAULTS GAIN
ALL (8 runs)        1h24m    1456  853.9 GiB  266.1 GiB   238 MB/s  27%/58%/16%           0 2.30x
                 4 of 8 runs measured vs baseline: took 27m14s, no cache would have
                 taken about 1h02m — saved 35m22s
----
2026-08-18 09:42    3m51s    1456  164.5 GiB        0 B   764 MB/s  0%/100%/0%            0 4.06x
2026-08-18 09:20   15m41s    1456    1.8 GiB  133.1 GiB   154 MB/s  100%/0%/0%            0 fill
2026-08-18 09:02   15m39s    1456  133.1 GiB        0 B   145 MB/s  0%/0%/100%            0 base
```

`GAIN` reads `base` for a baseline run (cache disabled — the reference), `fill`
for a run that populated the cache, and `-` where the comparison was refused.
The `RATE` column is what the job consumed, from all sources combined — it is
paced by the application, not a cache capability (that is `ucache bench`).
`--top N` shows more rows; `--json` on either command emits the same figures
for scripting.

Records appear when a job that used the cache **exits**; CLI invocations do not
write them. `ucache stats --reset` starts a fresh counter window but keeps the
run history (moved under `stats/history/`, where both commands still read it) —
baselines survive a reset.

## Monitoring — reading `ucache stats`

`ucache stats` aggregates the counters every process wrote and prints four
blocks. Run it after a job to see what the cache actually did. An example from
a warm pass:

```text
stats across 3 process file(s):
  opens              1574
  hit_bytes          58720256000 (54.7 GiB)
  miss_bytes         0 (0 B)
  origin_bytes       0 (0 B)
  ...
  crc_failures       0
  failopen_events    0
  admissions_bypassed 0
workflow:
  files opened       787 distinct (2.0 opens/file)
  served by tier     direct 0 B | fill 0 B | ram 1.2 GiB | disk 53.5 GiB | replica 0 B
  hit disk reads     2380995 (mean 41.8 KiB, 88% sequential)
  re-read factor     1.8x (first touch 30.4 GiB of 54.7 GiB byte-tier serves)
  fill flushes       4210 runs (mean 7.9 MiB)
read size p50/p95/p99 (log2 buckets, floor):
  requested          4.0 KiB / 64.0 KiB / 256.0 KiB
  byte-tier pread    32.0 KiB / 128.0 KiB / 512.0 KiB
latency p50/p95/p99:
  hit read           48us / 210us / 1.4ms
  origin rt          -
```

The first block is raw totals. `workflow:` is the arithmetic you would
otherwise do by hand — which tier served the bytes, how often each file was
opened, how much of the reading was re-reading. The last two blocks describe
what the storage was asked for and how long it took; `-` means there were no
samples, so `origin rt -` above is the point of a warm pass, not a gap.

### The four questions worth asking

1. **Did the cache engage at all?** On a warm pass `origin_bytes` should be
   **0** — every byte came from local disk. If it is large, or `relay_bytes`
   dominates, the cache is being bypassed rather than used; `ucache doctor`
   and the activation section above are where to look.
2. **Is anything wrong?** `crc_failures`, `failopen_events` and
   `validations_failed` should **all be 0**. They are separate on purpose:
   a CRC failure quarantines a page, a fail-open event means the cache
   degraded to pass-through, and a validation failure means an entry was
   discarded as stale.
3. **Is the cache big enough?** A non-zero `admissions_bypassed` means files
   were left uncached because everything resident was still in use — reads
   succeeded, uncached. Together with a large `evicted_bytes` it says the
   working set does not fit; `ucache status` names the remedy.
4. **Is the cache disk keeping up?** `fill stalls` is time a job spent waiting
   on the cache disk, and the `hit read` latency percentiles show what reads
   cost. If those are slow, measure the disk itself with
   `ucache bench <dir>` — a cache on the wrong device can be slower than the
   origin.

### Other forms

```sh
ucache stats --files --top 20   # per-file records, costliest first
ucache stats --reset            # delete the window and start a fresh measurement
```

`--reset` warns if a job looks like it is still running, since its counters
would vanish with the files.

Every field, and the JSON schema tooling reads, is in [Metrics](STATS.md).

## How it works (briefly)

- **Page cache, not file cache.** uCache stores the 4 KiB pages your analysis
  actually reads (typically a small fraction of each file), each protected by a
  CRC32C. It never stores data it wasn't asked for beyond page rounding.
- **Validation.** A cached entry is trusted for a freshness window (default
  7 days): inside it, opens don't contact the origin at all — warm passes work
  even when the remote is down or flaky. When the window expires, the next
  open re-checks the origin file's size (and, with
  `UCACHE_VALIDATE=size+mtime`, its mtime); a changed file is re-cached. Set
  `revalidate_seconds = 0` to re-check on every open.
- **Fail-open.** Every failure path degrades to a normal uncached read.

## Configuration

Three levels, lowest to highest, each with its own job:

1. **Your defaults** — `key = value` lines in the same `ucache.conf` that
   activates the plugin (§2), next to `url`/`lib`/`enable`. Edited by hand,
   by you; no `ucache` command ever writes this file.
2. **Current values** — set from the CLI without touching your defaults:
   `ucache set <key> <value>` / `ucache unset <key>` (kept in a small state
   file inside the cache dir; applies to processes started from then on,
   persists until unset).
3. **Per-job override** — the `UCACHE_*` environment variable twins, for one
   run: `UCACHE_REVALIDATE_S=0 ./my_job`.

`ucache settings` prints every knob with its effective value **and where it
came from** (default | conf | state | env); `doctor` flags any CLI-set values
overriding your defaults. Common keys:

| ucache.conf         | env                     | meaning |
|---------------------|-------------------------|---------|
| `dir = …`           | `UCACHE_DIR`            | cache location — **required** (no default; use a local disk). Unset ⇒ doctor FAILs, plugin runs uncached |
| `max_bytes = 50g`   | `UCACHE_MAX_BYTES`      | hard cache-size cap; unset ⇒ no cap (use the disk, evict at the floor) |
| `min_free_bytes = …`| `UCACHE_MIN_FREE_BYTES` | keep this much disk free (the default limit) |
| `evict_protect_seconds = …`| `UCACHE_EVICT_PROTECT_S` | do not evict an entry read this recently (default 86400 = 1 day; 0 = plain LRU) |
| `validate = size`   | `UCACHE_VALIDATE`       | `none` / `size` / `size+mtime` / `cksum`. **Caveat:** the plugin has no origin-checksum source yet, so `cksum` currently degrades to size-only (weaker than `size+mtime`) — prefer `size+mtime` until a checksum query lands |
| `revalidate_seconds = 604800` | `UCACHE_REVALIDATE_S` | freshness window (TTL): an entry validated against the origin within this many seconds is served with **no remote contact at all**. Default 7 days — right for write-once physics data. `0` = re-check on every open; `ucache rm <url>` forces a re-check anytime |
| `open_retries = 0`  | `UCACHE_OPEN_RETRIES`   | retry a transient open failure this many times (0 = off); backoff via `open_retry_base_ms`/`open_retry_max_ms` |
| `recompress = off`  | `UCACHE_RECOMPRESS`     | `on` = the files your jobs read are transcoded to fast-to-decode replicas **automatically in the background** (default off — opt-in CPU/disk). Flip it with `ucache set recompress on` |
| `recompress_codecs = lzma,zlib` | `UCACHE_RECOMPRESS_CODECS` | which **source** codecs are worth recompressing (comma list); branches in other codecs are served as-is |
| `recompress_reclaim = superseded` | `UCACHE_RECOMPRESS_RECLAIM` | what to free from the byte cache once a file's replica exists: `superseded` (default) punches only the ranges the replica replaced; `full` drops the **entire** byte copy — replicas become the primary copy, uncovered reads refetch from origin (space-tight disks) |
| `trace = off` | `UCACHE_TRACE` | `io` = write a sampled per-operation JSON trace next to the process's stats file (deep-dive forensics; zero cost when off). Best set per job: `UCACHE_TRACE=io python3 my_analysis.py` |
| `trace_sample = 64` | `UCACHE_TRACE_SAMPLE` | record every Nth read-class trace op (`1` = everything; opens/flushes are always recorded) |
| `disable = true`    | `UCACHE_DISABLE`        | turn caching off (pure pass-through) |

Sizes accept `k`/`m`/`g`/`t` suffixes.

**Put the cache on local disk.** `dir` has **no default** — an unset cache
location is a loud condition (`doctor` FAILs, the plugin passes through
uncached), never a silent cache in your home directory, which on CERN
machines is AFS/NFS. Point it at a local filesystem (`/data/$USER/…`, local
scratch, `/tmp/$USER/…`); `doctor`'s sparse-files WARN flags network
filesystems that slipped through.

**Reliable reads under a flaky/loaded/WAN remote.** This is the default: once
an entry has been validated, reads within the 7-day freshness window are
served **entirely from local disk — the origin is never opened or statted**,
and is contacted only if a genuine cache miss needs new bytes. Repeated
analysis passes are independent of remote state (the origin can be down and
warm reads still complete). Right for write-once physics data; a file changed
in place at the origin isn't noticed until the window lapses — set
`revalidate_seconds = 0` (re-check every open) if that ever matters for yours.

**Reliable opens under a flaky/loaded remote.** Some servers intermittently fail
individual file *opens* under load or over the WAN (a transient error — the same
file opens on retry), and ROOT aborts the whole job on any single open failure, so
a many-file job's success probability collapses with scale. Set
`UCACHE_OPEN_RETRIES` (e.g. `3`) and the plugin retries a *transient* open failure
with exponential full-jitter backoff (`UCACHE_OPEN_RETRY_BASE_MS`=200,
`UCACHE_OPEN_RETRY_MAX_MS`=5000 cap) instead of letting it abort — genuine errors
(missing or forbidden files) still fail fast. This rescues the initial (cold)
fill; `UCACHE_REVALIDATE_S` then carries later warm passes with no remote contact.
Off by default; for a large job over a flaky remote, 2–3 retries makes a transient
per-open failure negligible.

**Eviction is on by default.** With no `max_bytes` set, the cache uses the disk
freely and evicts least-recently-used entries only to keep a free-space floor
(`min(50 GiB, 10% of the volume)` free) — so a heavy session fills most of the
disk and reclaims under pressure. Set `max_bytes` for a fixed-size cache instead.
Protect a hot dataset from eviction with `ucache pin <url>`. To reclaim space
yourself — by age, by size, per file, or all at once — see the cleanup commands
below and the dedicated guide in `docs/CACHE_MANAGEMENT.md`.

## CLI reference

| command            | what it does |
|--------------------|--------------|
| `ucache --version` (`-V`) | print the version and the build id, then exit. The build id identifies the revision this binary was built from (`v0.18.3`, or `v0.18.3-4-g1a2b3c-dirty` for a local build with edits); it also appears in every benchmark record, so a measurement can be traced back to a binary. A build id equal to the bare version means the build could not read its own revision |
| `ucache setup [--host H] [--dir PATH]` | write the single conf file (activation + settings, cache dir explicit) to `~/.xrootd/client.plugins.d` |
| `ucache doctor`    | check install, filesystem (sparse/flock), and activation |
| `ucache test <url>` | end-to-end self-test: cold + warm whole-file read via xrdcp against your setup as-is; warm must be origin-free; cleans up the entry it created (pre-existing entries kept) |
| `ucache enable` / `disable` | turn caching on/off (flips the conf) |
| `ucache summary [--detail] [--json]` | **overall performance across every recorded run, and the time the cache has saved (or cost) you, measured against a no-cache baseline** — record one by running your job once with `UCACHE_DISABLE=1`. Refused with a reason rather than qualified when the comparison would not be sound. `--detail` adds the last run: tier split, delivered rate, per-tier read counts and sizes, replica coverage |
| `ucache history [--top N] [--json]` | an **ALL** row aggregating every recorded run, then one row per run, newest first — whether the numbers are holding up across runs, versions and machines |
| `ucache status`    | cache location, budget, usage, aggregate stats |
| `ucache ls [--sort age\|size]` | list cached entries (size, cached, coverage, last-used age, replica, pinned) |
| `ucache stats`     | aggregate `stats/*.jsonl` across all processes, plus the derived **workflow picture**: opens per distinct file, bytes served per tier (RAM / disk / replica / origin / relay), disk-read count + mean size + sequential share, re-read factor, fill flush shape, and p50/p95/p99 latencies |
| `ucache stats --files [--top N]` | per-file records (one per file per process), costliest first: which files were re-opened, re-read, served from which tier |
| `ucache stats --reset` | delete the per-process stats files (counters, per-file records, traces): a fresh window for measuring **one** run against a specific cache state (run it between jobs — it warns if a file looks live). Counters only; the cache contents are untouched |
| `ucache bench --threads N [PATH …] [--size SZ] [--measurement-duration S] [--block KB] [--fill writers=N,block=SZ] [--sweep] [--cache-path [--cache-sample SZ]] [--log FILE\|--no-log]` | storage self-test of the cache dir (or of candidate dirs, to pick one). Three groups: **standard** measures at pinned block sizes and queue depths 1/16/32, comparable to a datasheet; **pattern** measures at your job's concurrency — each serving tier's read shape, and random reads under writeback; and, with `--cache-path`, the same storage **through uCache's own fill and read code** rather than an imitation of it. Plus fsync, create/unlink. `--threads` is **required and never guessed** — it is what your analyses run at, not the core count. `--measurement-duration` is the window of ONE measurement (the build stage gets 3×; the plan and estimated total print up front). Appends every run, with the machine, load and block device behind the path, to `./ucache-bench.txt`. **Full guide: `docs/BENCH.md`** |
| `ucache netbench <root://…> [--streams N,…] [--block KB] [--seconds S]` | origin random-read baseline at 1/16/64 streams: what the network side delivers, for a fair cache-vs-origin comparison |
| `ucache evict [--older-than DUR \| --newer-than DUR \| --to-size SIZE] [--dry-run]` | reclaim space: no flags = one pass to the configured budget; `--older-than 30d` drops entries unused that long; `--newer-than 1h` drops entries used within the window (undo a polluting run); `--to-size 20g` LRU-evicts down to a total size; `--dry-run` previews |
| `ucache rm <url> [url…]` | remove specific entries (byte cache + replica) |
| `ucache clear [--yes] [--keep-pinned]` | empty the whole cache (prompts unless `--yes`) |
| `ucache pin <url>` / `unpin <url>` | protect / unprotect an entry from eviction |
| `ucache verify <url>` | CRC-scrub an entry; quarantine (not wipe) bad pages |
| `ucache settings`  | every setting: effective value + where it comes from (default \| conf \| state \| env) |
| `ucache set <key> <value>` / `unset <key>` | change / drop a **current** value without touching your defaults in the conf |
| `ucache recompress [--jobs N] [--yes]` | transcode the cached files whose source codec is in `recompress_codecs`, in the foreground with live progress (`--jobs` default cores/2). Estimates disk growth first and asks for confirmation if the sweep would push the cache into eviction; `--yes` overrides (scripts) |
| `ucache branches <url>` | which branches your analysis read: fully-cached branches with bytes + source codec, and the summary share |
| `ucache untranspose <url>` | drop an entry's replica; the byte cache is kept |

**Recompression (decompress-once replicas).** Tightly compressed (LZMA)
ntuples spend most of a *warm* analysis just decompressing. uCache can rebuild
the branches you actually read into replicas re-encoded once as ZSTD-1 — an
order of magnitude cheaper to decode — and serve them transparently (same
results, bit-identical). One switch controls it:

```sh
ucache set recompress on   # or `recompress = on` in ucache.conf as your default
```

With it on, each file's replica is built by a detached, nice'd background
worker right after your job closes it, with no commands ever typed and nothing
printed to your terminal (log: `<cache-dir>/recompress.log`; totals:
`ucache status`). With it off (the default), nothing is ever built in the
background; `ucache recompress` runs one foreground sweep over what is already
cached, with live progress. In both cases only branches whose source codec is
in `recompress_codecs` (default `lzma,zlib`) are transcoded — recompressing
already-fast codecs would waste CPU and disk — and only branches your runs
actually read, fully cached, qualify.

**On a large dataset, finish the job with one explicit sweep.** Background
building is deliberately unobtrusive — nice'd, two jobs at a time — so it is
opportunistic, not a guarantee: measured over a 787-file dataset, only about a
quarter of the replicas existed by the time the analysis pass ended. That
matters because **partial coverage buys little or nothing**: the files that
still lack a replica are read from the byte cache and pace the whole loop, so a
half-recompressed dataset can run no faster than an unrecompressed one. The
reliable recipe is therefore both halves:

```sh
ucache set recompress on   # build opportunistically while jobs run
# … run your analysis once (fills the cache) …
ucache recompress          # complete coverage; prints what it did
```

The background pass is still worth having: it costs the running analysis
nothing measurable and does a useful fraction for free, which shortens the
sweep. Check where you stand with `ucache status` (the `recompressed:` line) or
per file with `ucache ls` (the `RECOMP` column). The sweep's own summary gives
each outcome its own words — `recompressed`, `declined` (with the codec it found
and the one-line fix), `already recompressed`, `incomplete`, `failed` — and
background passes add `deferred` when they decline for want of disk space.
`ucache doctor` will tell you why nothing is being built if that is what you are
seeing (see Troubleshooting).

Replicas coexist with the byte cache by default (only the ranges the replica
physically replaced are punched). If disk space is tight, make replicas the
primary copy instead:

```sh
ucache set recompress_reclaim full
ucache recompress            # also retroactively reclaims existing replicas
```

Every entry with a valid replica then gives back its whole byte-cache copy the
moment the replica is available; anything the replica does not cover refetches
from the origin on demand. See CACHE_MANAGEMENT §4 for details.

Set expectations honestly: the replica removes *decompression* time only. A
warm analysis that is 80% LZMA decode gets several× faster; one dominated by
its own compute may gain 10%. One instrumented run with ROOT's
`TTreePerfStats` tells you your ceiling before you spend the disk (measured
1.04–1.22× of the cached bytes on LZMA-9 sources, by container — see
`docs/CACHE_MANAGEMENT.md` §3; `ucache status` totals it, and superseded
original pages are hole-punched so the data is not stored twice). `ucache
branches <url>` shows exactly which branches you read and their codecs.
Escape hatches: `ucache set transpose off` stops serving replicas; `ucache
untranspose <url>` drops one (they are derived data, rebuildable from the
byte cache).

Stuck? See **`docs/TROUBLESHOOTING.md`**.

---

uCache is MIT-licensed (see `LICENSE`); copyright (c) 2026 Massachusetts
Institute of Technology.
