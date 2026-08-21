Changelog — what each release changed, newest first. High level only: the
things that affect someone using uCache; the commit history has the detail.
Heading size is how much the release matters: `#` a landmark, `##` new
capability, `###` fixes and refinements.

Versions are `0.y.z` while the project is pre-production: `y` adds capability,
`z` fixes and refines. Prebuilt EL9 packages are attached to each release.

**Across every release so far:** the on-disk cache format has not changed, so a
newer release reads a cache written by an older one. Recompression stays off
unless you turn it on. Any XRootD 5.6 or newer 5.x client works; 6.x from
v0.21.0.

## v0.21.0 — 2026-08-21
- XRootD 6 clients are supported — the plugin builds against and loads into 6 as
  well as 5.
- A running job no longer evicts its own working set: an entry is not an eviction
  candidate while it was read within the last day.
- When the cache is full of such entries it stops caching new files rather than
  evicting data the job still needs, and `ucache status` says so. Reads keep
  working, uncached. `evict_protect_seconds = 0` restores the old behaviour.
- Recompression accepts ZLIB sources by default as well as LZMA, so it no longer
  declines a ZLIB dataset without being told to.

## v0.20.0 — 2026-08-16
- The plugin, the CLI and the tests build and run on macOS (Apple silicon,
  MacPorts), and uCache serves `root://` reads there.
- `ucache bench` measures macOS storage too, and reports how that measurement
  differs from the Linux one — the two platforms cannot bypass the page cache
  the same way.
- Faster checksums on 64-bit Arm.
- Building against XRootD 6 stops at configure with an explanation, rather than
  failing deep inside the compiler.

### v0.19.1 — 2026-08-13
- A warm pass over a recompressed RNTuple file no longer re-reads anything from
  the origin.

## v0.19.0 — 2026-08-13
- RNTuple files can be recompressed into the replica tier — same switch, same
  behaviour as TTree files.
- Large reads are cached again: their edge pages were being dropped, and an
  oversized vector-read element was refused outright.

### v0.18.4 — 2026-08-12
- `ucache bench` measures a cold fill through uCache's own code rather than an
  imitation of it.
- A reference describing every measurement `ucache bench` makes, and how to
  read it.

### v0.18.3 — 2026-08-10
- `ucache bench` records the device it measured and attributes the run's IO to
  it, so a number can be traced to the hardware that produced it.

### v0.18.2 — 2026-08-10
- `ucache bench` measures at several stream counts, split into
  datasheet-comparable and workload-shaped groups, with every run appended to a
  log.
- `--threads` is required rather than guessed: it is the concurrency your jobs
  actually run at, which no tool can infer.

### v0.18.1 — 2026-07-27
- A failed recompression build is reported and exits non-zero; `--strict` for
  callers who want it enforced.
- The number of background recompression jobs is set by the caller.

## v0.18.0 — 2026-07-27
- Reads of cached data issue one request per contiguous run of pages instead of
  one per page, on both cache tiers.

### v0.17.5 — 2026-07-26
- Background recompression respects the eviction floor, so it can no longer
  fill the cache and start evicting the data your job is reading.
- When it builds nothing, it says which reason applies.
- Pages are verified against their checksums before being recompressed.

### v0.17.4 — 2026-07-25
- ROOT files that grew past 2 GB after their key list was written can be
  recompressed; earlier releases refused them.

### v0.17.3 — 2026-07-22
- Prebuilt EL9 packages (RPM and relocatable tarball) attached to each release,
  built against the oldest supported client so they load under any newer one.
- `ucache doctor` detects an XRootD client too old for the plugin; previously
  it simply did not load, and nothing said why.

# v0.17.2 — 2026-07-22
- First public release: a transparent per-user read cache for `root://` data —
  an XrdCl plugin that caches the pages your jobs read onto local disk, and a
  `ucache` command to set it up, inspect it and clean it.
- Fail-open by construction: any cache problem degrades to a normal uncached
  read.
- Optional recompression rewrites cached files into a form that is faster to
  read back.
