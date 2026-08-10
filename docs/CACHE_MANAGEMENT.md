# Using uCache: status, space, and cleanup

A practical guide to running `ucache` day to day, checking what it has cached,
and keeping its disk usage under control. For first-time install and
activation see `docs/USER_GUIDE.md`; this document picks up from there and
focuses on the **run → check → clean up** loop.

The one thing to know up front: **uCache will not fill your disk.** By default
it keeps a free-space floor and evicts least-recently-used data under pressure,
so old files you have stopped reading are exactly what it drops first. The rest
of this guide shows how to see that happening and how to reclaim space on
demand when you want to.

---

## 1. Everyday use

Once activated, you do nothing special — every `root://…` file your jobs read
is cached transparently. Run your analysis normally:

```sh
python my_analysis.py     # cold: fetches the working set from the origin
python my_analysis.py     # warm: served from local disk, no origin traffic
```

Your code does not change, and if anything ever goes wrong (disk full, bad
cache dir, corruption) uCache **fails open** — the job still runs correctly,
just uncached. That safety property is what makes every cleanup action below
safe: the worst case of removing something is a slower next read.

---

## 2. Check status and space usage

### `ucache status` — the one-glance view

```
$ ucache status
cache dir : /data/you/ucache-cache
budget    : keep 50.0 GiB free (no byte cap; uses the disk)
headroom  : 371.2 GiB until eviction starts (disk free 421.2 GiB, floor 50.0 GiB)
freshness : 7d — entries validated within this window are served with no origin contact (revalidate_seconds=604800)
entries   : 1 (0 pinned)
original  : 2.5 GiB (total size of the cached files at the origin)
cached    : 450.3 MiB — 17.6% of the original bytes (median file: 17.6%)
disk used : 450.3 MiB (450.3 MiB cached bytes + 0 B recompressed)
recompressed: none — `ucache set recompress on` for automatic background builds, …
stats across 1 process file(s):
  opens              1
  hit_bytes          45632 (44.6 KiB)
  miss_bytes         444385280 (423.8 MiB)
  origin_bytes       472283136 (450.4 MiB)
  served_bytes       444430912 (423.9 MiB)
  ...
  evicted_entries    0
  evicted_bytes      0 (0 B)
  ...
```

- **budget** — how growth is bounded (see §3). "keep N free" is the default;
  "N hard cap" appears if you set one; "eviction disabled" only if you turned
  both limits off.
- **headroom** — how far the disk is from the eviction floor. `NONE` means
  every new fill evicts something (a revolving door — make the cache smaller
  or the disk bigger).
- **freshness** — the revalidation window (`revalidate_seconds`, see
  `docs/USER_GUIDE.md`).
- **entries / original / cached / disk used** — how many files, their total
  size at the origin, the bytes actually cached (with the coverage share),
  and the physical footprint including recompressed replicas.
- **stats** — cumulative counters aggregated across every process that used
  this cache. `origin_bytes` near 0 on a warm run means you are being served
  from disk; `evicted_*` shows what reclamation has done.

### `ucache ls` — per-file, largest first

```
$ ucache ls
SIZE      CACHED    COV%   LAST   RECOMP    PIN   KEY
2.5 GiB   450.3 MiB  17.6% 3d     -               root://127.0.0.1:10940/…/data1.root
(1 entries)
```

- **SIZE** — the origin file size; **CACHED** — how much of it is actually on
  your disk (uCache stores only the pages your jobs read, not whole files);
  **COV%** — that fraction.
- **LAST** — how long ago the entry was last *read* (`3d`, `5h`, `12m`). This is
  exactly what eviction ranks by, so it tells you what will roll off first.
- **RECOMP** — size of a decompress-once (recompressed) replica if one
  exists (see `docs/USER_GUIDE.md`), else `-`.
- **PIN** — `yes` if protected from eviction (§4).

By default `ls` is sorted largest-first. To hunt for stale data, sort by age:

```sh
ucache ls --sort age      # oldest-used entries first — what to clean up
```

### Raw disk footprint

`CACHED` is the logical byte count from the sidecars. To see the actual blocks
on disk (sparse files, replicas, and all), use `du` on the cache directory:

```sh
du -sh "$(ucache status | awk '/cache dir/{print $NF}')"
```

### Measuring one run against a specific cache state

The stats block in `ucache status` aggregates every process that ever ran
against this cache — useful for totals, useless for "what did *that* run
cost". For a clean window, reset the counters between jobs:

```sh
# put the cache in the state you want (warm it, reclaim it, …), then:
ucache stats --reset      # deletes stats/*.jsonl; warns if a job looks live
<run your analysis once>
ucache stats              # exactly that run: hits, misses, origin bytes,
                          # latency histograms
```

Only the diagnostic counters are removed — the cached data is untouched. Run
the reset while nothing is reading through the cache: a live process keeps
writing to its (now deleted) file and those numbers are lost.

### Comparing two workflows (why is THIS analysis slow?)

`ucache stats` ends with a derived `workflow:` block designed so that two
runs can be compared without guesswork:

```
workflow:
  files opened       1310 distinct (50.2 opens/file)      <- reopen loop!
  served by tier     ram 0 B | disk 47.4 GiB | replica 0 B | origin 0 B | relay 0 B
  hit disk reads     12400000 (mean 4.1 KiB, 12% sequential)
  re-read factor     4.8x (first touch 9.9 GiB of 47.4 GiB byte-tier serves)
latency p50/p95/p99:
  hit read           96us / 12.3ms / 201ms
```

How to read it: `opens/file` far above 1 means the analysis re-opens its
files (e.g. a new RDataFrame per trigger path) — every pass re-reads and
re-decompresses everything. The tier line shows who actually served the
bytes (replica `0 B` on a recompressed cache means the replicas never
engaged — worth investigating). `mean size` and `% sequential` of the disk
reads tell you whether the cache disk sees a streaming or a random-IOPS
workload — the difference between fine and unusable on quota-limited
volumes. `re-read factor` counts bytes served more than once within one
open of a file.

Drill down per file with `ucache stats --files --top 20`, and — for deep
forensics — rerun one job with a full trace:

```sh
UCACHE_TRACE=io UCACHE_TRACE_SAMPLE=1 python3 my_analysis.py
# -> <cache>/stats/<host>-<pid>-...trace.jsonl: one JSON line per operation
#    (op = open|hit|ram|replica|wire|readv|flush|meta, offset, length, µs)
```

---

## 3. How space is controlled (so it does not fill up)

Eviction is **on by default** and runs automatically during normal use — you do
not have to schedule anything. There are two ways to bound growth:

| Mode | Set via | Behavior |
|------|---------|----------|
| **Free-space floor** (default) | `UCACHE_MIN_FREE_BYTES`, or automatic | Use the disk freely; evict LRU whole entries whenever free space drops below the floor. Auto floor = `min(50 GiB, 10% of the volume)`, clamped to at most half of the free space at startup. |
| **Hard size cap** (opt-in) | `UCACHE_MAX_BYTES` | Cap the cache at a fixed size; when it reaches 90% of the cap, evict down to 75%. |

Eviction always removes **least-recently-used entries first** — ranked by when
each file was last *read*, not when it was cached. That is the key point for
your concern: **stale data you have stopped using is the first to go**, and hot
data you keep reading stays. Nothing is removed while it is still open; pinned
entries (§4) are never removed.

To make the cache smaller and steadier, pick one:

```sh
# Fixed-size cache (eviction starts at ~9 GiB — 90% of the cap — drains to ~7.5 GiB):
export UCACHE_MAX_BYTES=10g

# Or keep more of the disk free (evicts sooner, caches less):
export UCACHE_MIN_FREE_BYTES=200g
```

Make it permanent with `ucache set max_bytes 10g` (or `min_free_bytes
200g`), or put the same line in `ucache.conf` to make it your default
(USER_GUIDE §Configuration). Sizes accept `k`/`m`/`g`/`t`.

---

## 4. Cleaning up — reclaim space on demand

Automatic eviction handles the disk-full case for you. These commands are for
when you want to reclaim space *now*, or shrink the cache proactively.

### Reclaim to the configured limit, now

```sh
ucache evict
```

Runs one eviction pass immediately instead of waiting for the next read. It
respects your budget — with the default floor it removes only enough to restore
the free-space floor (often nothing, if you are not near it). If another
process is mid-eviction it reports the lock is held; just try again.

### Shrink to a specific size, now

```sh
ucache evict --to-size 20g            # LRU-evict down to a 20 GiB total
```

Removes least-recently-used entries (oldest first) until the whole cache is at
or below the target. Sizes take `k`/`m`/`g`/`t`.

### Remove data you haven't used in a while

```sh
ucache evict --older-than 30d         # drop entries not read in 30 days
```

Durations take `s`/`m`/`h`/`d`/`w`. This is the direct answer to "clear out old
unused data" — it targets staleness rather than total size.

### Undo a polluting run

```sh
ucache evict --newer-than 1h          # drop entries read within the last hour
```

The mirror of `--older-than`: a one-off job just walked a big dataset and
displaced your hot working set — drop what it touched, keep the rest.

Preview either one before committing with `--dry-run`, which lists exactly what
would go (and its age) and removes nothing:

```sh
ucache evict --older-than 30d --dry-run
  would remove 450.3 MiB 47d    root://…/old_dataset.root
  would remove 1.2 GiB   31d    root://…/another.root
dry run: 2 entries (1.6 GiB) would be removed
```

Both respect pins (§4) — a pinned entry is never removed by age or size.

### Remove specific files

```sh
ucache rm root://host//path/to/file.root [more URLs…]
```

Drops those entries entirely — page cache **and** any replica overlay. Use this
when you know exactly which datasets you are done with. It reports `not cached`
(and exits non-zero) for a URL that has nothing to remove.

### Protect data you do not want cleaned up

```sh
ucache pin   root://host//path/to/hot.root     # never evict this
ucache unpin root://host//path/to/hot.root     # allow eviction again
```

Pin your active dataset before an aggressive cleanup so shrinking the cache
never touches it.

### Wipe everything

```sh
ucache clear              # prompts for confirmation, then empties the cache
ucache clear --yes        # no prompt (for scripts/cron)
ucache clear --keep-pinned   # wipe everything except pinned entries
```

`clear` shows how many entries and how many bytes it will remove and asks before
doing anything; pass `--yes` to skip the prompt. On a non-interactive stdin
(a script or cron job) it refuses unless `--yes` is given, so it can never
surprise you. Wiping is always safe — uCache fails open and re-fetches on the
next read.

### Drop a file's replica (keep the byte cache)

```sh
ucache untranspose root://host//path/to/file.root
```

Removes a decompress-once replica overlay while keeping the ordinary page
cache. Useful if replicas are the bulk of your footprint (check the RECOMP
column in `ucache ls`).

### The opposite: let replicas replace the byte cache (space-tight disks)

If you use recompressed replicas and space is tight, the byte-cache copy of a
replicated file is mostly dead weight: the replica already serves the branches
your analysis reads, and what's left in the byte cache is prefetch margin and
odds and ends. Reclaim it:

```sh
ucache set recompress_reclaim full     # make replicas the primary copy
ucache recompress                      # builds what's missing + reclaims,
                                       # file by file, space freed as it goes
```

Space first: replicas cost ~1.4× of the bytes they replace, and a sweep that
does not fit triggers LRU eviction *mid-sweep* — which can evict the very
data your next run needs and turn a warm loop into an origin-refetch storm.
`ucache recompress` therefore estimates its net growth against the headroom
to your eviction floor before building anything, prints the arithmetic, and
asks for confirmation when it would not fit (`--yes` to override in scripts;
on a non-tty it refuses without it). Check where you stand any time with the
`headroom` line in `ucache status`.

**Background recompression never evicts.** With `recompress = on` the builds
run in a detached worker while your job is reading, so there is nobody to ask:
that worker checks each file against the same headroom and simply declines the
ones that would not fit, leaving them queued for a later pass — one that has
room because you freed some, or because `recompress_reclaim = full` handed a
byte copy back. It records what it deferred, and why, in
`<cache dir>/recompress.log`. So `recompress = on` cannot be the reason a
volume crosses its free-disk floor; if you want the coverage that a tight volume
will not give you, free space first and then run `ucache recompress` explicitly.

One exception, stated plainly: the check is against the **free-disk floor**. If
you cap the cache with `max_bytes` and set no `min_free_bytes`, eviction runs on
the byte cap instead, which this check cannot see — background builds are then
ungoverned, and the worker says so once in its log. Set `min_free_bytes` as well
if you want background recompression bounded on such a cache.

With `recompress_reclaim = full`, every pass that builds or finds a valid
replica drops that entry's **entire** byte-cache copy (holes are punched, so
the space is back immediately — watch the `cached` line in `ucache status`
shrink). The first sweep also reclaims retroactively: entries recompressed
earlier give their byte copy back too. Anything the replica does not cover
(rarely-read header bytes, branches that were only partially cached) is simply
refetched from the origin on demand and re-cached — reads always stay correct.

The default (`recompress_reclaim = superseded`) keeps today's behavior:
only the ranges the replica physically replaced are punched.

---

## 5. A "set it and forget it" recommendation

For most users, the defaults are already correct: leave eviction on, let the
free-space floor protect the disk, and never think about it. If you share a
volume or want a predictable footprint, one line does it —
`ucache set max_bytes 20g`, or as your default in `ucache.conf`:

```
max_bytes = 20g          # a fixed 20 GiB cache; oldest-unused data rolls off
```

Then the only housekeeping you ever need is an occasional `ucache status` to
confirm it is behaving, and `ucache pin` on a dataset you are actively
reprocessing.

If you want a periodic purge (e.g. weekly, dropping anything untouched for a
month), a cron line does it with no config change and no root:

```cron
0 6 * * 1  /path/to/ucache evict --older-than 30d
```

---

## 6. Choosing a cleanup approach

The commands in §4 span from surgical to nuclear — pick by how much you know
about what you want gone:

| You want to… | Use |
|--------------|-----|
| Get rid of a specific dataset you're done with | `ucache rm <url>` |
| Clear out anything you haven't touched lately | `ucache evict --older-than 30d` |
| Get back under a size budget when low on disk | `ucache evict --to-size 20g` |
| See what would go before committing | add `--dry-run` |
| Start completely fresh | `ucache clear` |

Between them, `ucache ls --sort age` is how you *decide*: it shows the stalest
entries first, so you can spot what to `rm` or how far back to set
`--older-than`. Pinned datasets (`ucache pin`) are skipped by every automatic
and criterion-based removal, so protect anything you're actively using before a
big cleanup.

One remaining sharp edge worth knowing: age is tracked at read time with a
~1-minute granularity, so `--older-than` is meant for hour/day windows, not
second-level precision.

---

## 7. Quick reference

| Task | Command |
|------|---------|
| Where is the cache / how full? | `ucache status` |
| What is cached (largest first)? | `ucache ls` |
| What is stalest (oldest-used first)? | `ucache ls --sort age` |
| Actual disk blocks used | `du -sh <cache dir>` |
| Reclaim to the configured limit now | `ucache evict` |
| Shrink to a target size now | `ucache evict --to-size 20g` |
| Drop anything unused for N days | `ucache evict --older-than 30d` |
| Undo a polluting run | `ucache evict --newer-than 1h` |
| Preview a cleanup without removing | add `--dry-run` |
| Remove specific files | `ucache rm <url> [url…]` |
| Empty the whole cache | `ucache clear` (`--yes` to skip the prompt) |
| Set a permanent size cap | `ucache set max_bytes 20g` (or `max_bytes = 20g` in ucache.conf) |
| Protect / unprotect a dataset | `ucache pin` / `ucache unpin <url>` |
| Drop one file's replica overlay | `ucache untranspose <url>` |
| Turn caching off entirely | `ucache disable` (or `UCACHE_DISABLE=1`) |

## 8. Is my cache disk fast enough? — `ucache bench`

Cache performance depends heavily on the storage under the cache dir: a
local SSD serves cached reads in microseconds, while a network/VM volume
with an IOPS quota can be *slower than reading from the origin* (seen in
production on a CERN OpenStack io1 volume). `ucache bench` measures the
raw storage behavior of the configured cache dir — or of any candidate
directory — using the exact access patterns uCache generates:

```sh
ucache bench                      # test the configured cache dir (~1 min)
ucache bench /data1 /ssd/scratch  # compare candidate locations
ucache bench /tmp --size 64g --measurement-duration 60 --threads 32
```

`--measurement-duration` (`--phase-seconds` and `--seconds` are still
accepted) is how long ONE measurement runs, not the runtime of the tool:
the run is `measurements × S` plus building the test file. The tool prints
the plan and its estimated total before it starts, so the arithmetic never
has to be guessed. Every measurement is time-boxed — that is what lets the
tool finish on a volume delivering a few dozen IOPS — so `--size` is a
ceiling the file may not reach; when it does not, the test-file line says
`stopped at time cap`.

`--threads` sets the concurrency of the **pattern** measurements, and is
**never guessed**: without it, the three concurrency-dependent ones are
skipped and the output says so. Pass the thread count your analyses actually
use — not the core count, unless they are the same. The **standard**
measurements are pinned at queue depths 1, 16 and 32 on every machine, which
is what makes them comparable.

The numbers come in two groups, because they answer different questions.
**`docs/BENCH.md` documents every measurement, how to read it, and how well each
one reproduces**; what follows is the summary.

**Standard measures** — fixed block sizes and queue depths, so they can be read
against a drive's datasheet, an `fio` number someone quotes, or another machine:

| metric | config |
|---|---|
| sequential read MB/s | QD1, large block, O_DIRECT |
| sequential write MB/s | QD1, large block, O_DIRECT, in place |
| random 4 KiB read IOPS + latency | QD1 (the latency reference), QD16 and QD32 |
| random 4 KiB write IOPS | QD32 — the depth datasheets quote |

**Pattern measures** — the shapes uCache actually generates, at your job's
concurrency, so they predict what a job will get here:

| metric | what it corresponds to in uCache |
|---|---|
| fill pattern MB/s, buffered and O_DIRECT | the cold fill: one writer, ~48 KiB writes, new files created and extended. The pair shows whether the kernel merges those small writes into larger device writes |
| random 48 KiB read at job concurrency | the byte cache, which serves at ~42 KiB with no locality between reads |
| sequential 512 KiB read at job concurrency | a replica, which is branch-major, so consecutive reads land adjacent and reach ~599 KiB per operation |
| sequential large-block read at job concurrency | streaming bandwidth under concurrency; **flat scaling vs QD1 means one stream already saturates the disk** |
| read-under-writeback latency | warm reads while a fill is running — the common mixed mode |
| fdatasync / create / unlink rates | sidecar flushes, eviction, `clear` |
| test-file creation MB/s | NOT a device spec — see below |

Both tier sizes sit between the standard 4 KiB and 4 MiB points, in the range
where a device stops being operation-bound and turns bandwidth-bound, so neither
can be interpolated from the standard measures. `--sweep` measures that curve
directly.

The write measurements cycle in place inside the one test file, so however long
a window runs the file never grows past `--size`. Peak disk usage is a little
above it — the fill stage holds its own file alongside — so budget
`--size` + 256 MiB.

**Which write number to trust.** Building the test file is not a device
measurement: it pays allocation, runs before anything has warmed up, and its
window length depends on `--size`. Three runs of one command on one idle SSD
reported 268.5, 451.9 and 371.9 MB/s for it, while the in-place sequential
write reproduced to ~6%. So **quote `seq_write_mbps`** (in place, QD1) for the
device, and read the test-file line for its SHAPE, which the tool always
states:

    test-file creation 268.5 MB/s   (build 180 s: 436.9 -> 268.5 MB/s, FALLING
                                     — the last quarter is the sustained rate)

`FALLING` is the useful case: the window got far enough to fall out of the
cache, so `seq_write_mbps` (the last quarter) is a real sustained rate.
`RISING` means it never did — the figure is a ceiling, not a floor, and a
longer write would report less. `flat` means neither was visible.

Whether you get one or the other depends on **how much the device has been
written to recently**, not only on the device. The same 64 GiB build on one
SATA SSD reported `FALLING 437 -> 268 MB/s` straight after a heavy write
job and `RISING 342 -> 452` after the same device had been idle for forty
minutes. Measure with a large `--size` and a long window
(`--size 64g --phase-seconds 60`), and if you need the number to be
defensible, run it twice.

So for a device's write capability, use `seq_write_mbps`: it writes to
already-allocated space partway through the run and reproduces to about 6%,
where the build-stage figure — first write to fresh extents, with whatever cache
state you started with — moved 68% across five runs of one command on an
otherwise idle machine.

Each run also prints a `ucache-bench-json:` line and appends a full record —
plan, numbers, JSON line, and the context needed to read them later — to
`./ucache-bench.txt` (`--log FILE` to redirect, `--no-log` to suppress).
The context is the machine, kernel and CPU count, the mount and block
device behind the path with its model and scheduler, the load average
before and after, and the CPU and device activity during the run. Keep one
such file per machine: it is what turns a number into evidence, and it is
what to attach when reporting a performance problem.

Rules of thumb from the machines measured so far:
random-read latency in *microseconds* = healthy local disk; *milliseconds*
= network-backed storage where warm reads may not beat the origin — on
such storage `recompress on` (sequential replica reads) matters far more,
and a genuinely slow cache dir can be worse than no cache at all. What such
storage charges for is read COUNT, not bytes, so both tiers serve a contiguous
run of cached pages with a single read rather than one read per page. A fully
cached request is usually one operation whatever its page count; it splits only
where the data does — at a page that is not cached, at a page still being
written, and at a 1 MiB ceiling.

That leaves a difference the two tiers cannot share. The byte cache stores the
file in its original layout, so a read can only be as large as the client's
request — and the next thing that analysis wants usually sits elsewhere in the
file, behind data it did not ask for. A replica stores each branch's data
contiguously, so consecutive reads land next to each other and one operation can
cover several of them. Measured on the same analysis over the same 787-file
dataset, serving it warm from each tier: the byte cache issued **2.38 M reads
averaging 42 KiB**, the replica tier **170 k averaging 599 KiB** — about **14×
more operations for the same work**, which is exactly the cost an IOPS-priced
volume charges for. Where operations are the scarce resource, that is the reason
to complete the recompression — and a partly-replicated dataset still reads the
remainder out of the original layout.

### Choosing a location: reference grades (from the 2026-07 fleet survey)

Anchor: a read from the origin costs ~1–8 ms (CERN-proximate), uncapped.
Sort candidates by `random 4KiB read (1)` p50 — lowest wins; prefer a small
fast disk over a big slow one (the cache holds ~1–3% of the data it serves;
cap with `max_bytes`).

Grade for a machine that runs analysis on ALL its cores (the normal case) —
the fair comparison is the cache's AGGREGATE small-read rate vs the origin's
at the same concurrency (~3,000 reads/s at 16 streams from a CERN-proximate
client; the origin scales, quota-capped volumes do not):

| grade | test | action |
|---|---|---|
| GOOD | rand read (1 thr) p50 ≤ 0.5 ms | everything works at any core count; `recompress on` optional (CPU win) |
| REPLICA-ONLY | 16-thr IOPS below ~3,000 BUT sequential read ≥ 300 MB/s | byte-cache hits LOSE to direct reads at full concurrency; usable only with `recompress on` **and full coverage** (replicas serve sequentially and dodge the IOPS quota — but any file left unreplicated is still read at the quota-capped rate and paces the job, so finish with an explicit `ucache recompress`) |
| BAD | 16-thr IOPS below ~3,000 AND sequential read < 300 MB/s | do not cache here — pick another disk or run uncached |

Sanity checks at any grade: read-under-writeback p99 > 50 ms means fills
will starve warm reads (> 200 ms: unusable); fsync p50 > 5 ms = sluggish
metadata (slow `clear`/eviction). Fleet calibration: every local disk =
GOOD; Ceph io2 = REPLICA-ONLY (1.2k IOPS cap, 347 MB/s seq); Ceph io1 =
BAD (680 cap at 13.5 ms/read, 50 MB/s seq).

Rules of thumb: any physical local disk (even consumer SATA) and VM-local/
ephemeral disks grade GOOD; network-attached volumes (Ceph RBD io1/io2/
standard, NFS, AFS, EOS FUSE) must be measured — the tier name does not
predict the grade (a measured io2 graded REPLICA-ONLY; a measured io1 BAD —
was slower than the origin). For trans-Atlantic origins (~30–100 ms/read)
the thresholds stretch proportionally; "local disk = GOOD" never changes.

### Measuring YOUR origin: `ucache-netbench`

The grades above compare against a reference origin (~1–3k reads/s
aggregate at CERN). Your origin may differ — measure it with the companion
tool (`ucache netbench` execs the ucache-netbench helper shipped next
to the plugin; needs an XrdCl installation):

```sh
ucache netbench root://<your-origin>//path/to/big_file.root
ucache netbench root://... --streams 1,16,64 --seconds 5   # 4 KiB blocks, same as `ucache bench`
```

It disables the uCache plugin for its own connections (it measures the
origin, not the cache; `--through-cache` for end-to-end runs) and prints
the same table + JSON discipline as `ucache bench`. A cache location is
worth having iff its `ucache bench` numbers beat your origin's
`ucache-netbench` numbers at your job's concurrency. Measured EOS
behaviors worth knowing: p95 latency is 20–40× the median (FST queueing);
a single file's server saturates near ~16 concurrent readers — more
streams on one file can make it slower; and the ORIGIN HAS ITS OWN CACHE —
repeated runs on the same file warm the FST's RAM (measured: 187 IOPS @
1.1 ms cold → ~2,000 IOPS @ 0.02 ms after several repeats; small files can
end fully server-RAM-resident at 40k+ IOPS), and cold latency depends on
which EOS pool holds the file (HDD pools: ~6 ms medians). For grading a
cache location, use the FIRST run on a file nobody read recently (that is
what an analysis faces); repeated runs measure the origin's best-case
ceiling instead.
