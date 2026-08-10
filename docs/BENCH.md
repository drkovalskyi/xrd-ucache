# `ucache bench` — what it measures, and how to read it

`ucache bench` is a storage self-test. It writes a test file into a directory
you name and measures the read and write patterns that matter for a cache,
then prints a table, a one-line JSON record, and the context needed to read
them again in six months.

It exists because cache performance is dominated by the storage under the
cache directory, and the spread between locations is enormous. A local SSD
serves cached reads in tens of microseconds; a network volume with an IOPS
quota can be *slower than reading from the origin*, at which point a cache
makes the job worse. Datasheets do not settle it — quotas, filesystem, mount
options, and the access pattern all move the answer by more than the device
model does.

**It reports numbers, not verdicts.** Nothing here decides whether a location
is good; it produces the evidence you decide with.

Contents: [running it](#running-it) · [what it does to the
disk](#what-a-run-does-to-the-disk) · [the two groups of
numbers](#the-two-groups-of-numbers) · [every measurement in
detail](#every-measurement-in-detail) · [the fill stage](#the-fill-stage-in-detail)
· [the run context](#the-run-context-block) · [reading a whole
record](#reading-a-whole-record) · [what reproduces](#what-reproduces-and-what-does-not)
· [pitfalls](#pitfalls) · [options](#options-reference) · [JSON
fields](#json-field-reference)

---

## Running it

The convention run, once per cache device:

```sh
ucache bench --size 64g --measurement-duration 60 --threads 32 /path/to/cache
```

That takes about 15 minutes. For a quick look, the defaults are much shorter:

```sh
ucache bench --threads 32                 # the configured cache dir, ~1 min
ucache bench --threads 32 /data1 /ssd2    # compare two candidate locations
```

### `--threads` is required and never guessed

It is the concurrency **your analyses run at** — not the machine's core count,
unless those happen to be equal. Without it the tool exits 2 rather than pick a
number for you:

```
bench: --threads N is required — it is the concurrency your analyses run at,
       and it is never guessed. It is NOT the core count unless they match.
```

The reason is that half the measurements are quoted as "what a job gets here",
and a concurrency nobody chose makes that claim false. Reading it off the core
count is wrong wherever a job does not use every core, and it is silently wrong
— which is worse than an error message.

### The run plan, printed before anything starts

`--measurement-duration` is how long **one** measurement runs, not how long the
tool runs. A run is `measurements × duration`, plus building the test file. The
tool prints that arithmetic up front so it never has to be guessed:

```
run plan: 60.0 s per measurement, 12 measurements  ->  ~900 s (15 min)
  test file: build until 64.0 GiB or 180 s, whichever comes first
  threads: 32

  Standard measurements
    sequential 4096 KiB read, QD1
    ...
```

12 measurements is the default shape (with `--threads`). `--sweep` raises it to
19. The build stage gets **three times** the measurement window as its time cap,
which is why the total is larger than a first guess.

Every measurement is time-boxed. That is what lets the tool finish on a volume
delivering a few dozen IOPS, and it is why `--size` is a **ceiling, not a
promise**: if the build hits its cap first, the test-file line says
`stopped at time cap` and the file is whatever got written.

### Where the results go

Each run prints to stdout **and appends** a full record to `./ucache-bench.txt`
— the plan, the table, the verbatim JSON line, and the context block. Redirect
with `--log FILE`, disable with `--no-log`.

The default is the **current working directory**, so run it from wherever you
keep that machine's records and the file maintains itself. Two consequences
worth knowing:

- an exploratory run started from a directory holding a curated record files
  itself into that record. Pass `--no-log` for anything you do not want kept.
- appended blocks are **newest last**.

Exit status is 0 when every path succeeded, 1 if any measurement failed, 2 for a
bad argument.

---

## What a run does to the disk

Everything happens inside one directory it creates and removes:

```
<path>/.ucache-bench.<pid>/
    testfile          # --size, or as much as the time cap allowed
    fill00-000000     # one at a time during the fill stage, unlinked as it goes
    m0000 … m0199     # 200 tiny files, during the create/unlink stage
```

**The test file never grows past `--size`.** It is written once, and every later
write stage cycles *in place* inside it rather than extending it — so however
long a window runs, that file stays the size it was built to.

Peak usage is a little above `--size`, during the fill stage: the test file is
still there while each writer holds one fill file. A fill file is
`min(--fill file, --size / 4 / writers)`, so the fill set adds at most a quarter
of `--size`, and with the defaults it adds 256 MiB per writer. In round numbers,
budget `--size` + 256 MiB for a normal run, or 1.25 × `--size` when `--size` is
under 1 GiB.

The tool's own pre-flight only checks for `--size` + 64 MiB, which is less than
that peak. If the disk is that tight the fill stage simply cannot create its
files and reports a very low rate rather than an error, so **leave real headroom
rather than the bare minimum** — and treat an implausibly low fill number on a
nearly full disk as suspect.

You need free space, not just a big disk — and on a **cache** directory,
remember that a 64 GiB test file is 64 GiB the cache cannot use while the run
lasts. If the cache is near its limit, that alone can trigger eviction.

Cleanup covers every way the measurement can end, including a failure partway
through. It does **not** cover signals: if you interrupt a run with Ctrl-C or
kill it, the directory stays behind with the whole test file in it. Remove it by
hand — `rm -rf <path>/.ucache-bench.*` — after checking no run is in progress.

### O_DIRECT, or buffered with eviction

The tool tries `O_DIRECT` first, so reads reach the device instead of RAM. The
mode it got is in the header line:

```
=== ucache bench: /path (xfs, 1787.6 GiB total, 306.4 GiB free, O_DIRECT) ===
```

Where `O_DIRECT` is unavailable — some network filesystems, `tmpfs` — it falls
back to buffered I/O and drops the file's pages (`fdatasync` +
`POSIX_FADV_DONTNEED`) before **each** read measurement. That is weaker than
`O_DIRECT`: eviction is advisory and the kernel may still read ahead. Treat
`buffered` numbers as an upper bound on the device, and do not compare them
against `O_DIRECT` numbers from another machine.

**On a virtual machine, `O_DIRECT` bypasses only the guest's cache.** The
hypervisor may still be caching underneath, and the block-layer counters cannot
see that — they attribute the traffic to a virtual device that answered it out of
someone else's RAM. The symptom is a rate above anything the hardware class can
deliver: one VM volume reported 12.8 GB/s on the replica-tier row with 100% of
the bytes attributed to the benchmark and the device idle beforehand. Nothing was
wrong with the measurement; the "device" simply was not the disk. If a number
exceeds the interconnect it would have to cross, it is a cache hit somewhere you
cannot see, and it does not predict what a cold job gets.

### The order stages run in

Build the file → `fdatasync` samples → random 4 KiB reads at QD1, 16, 32 →
sequential read → sequential write → *(pattern reads: sequential at job
concurrency, byte tier, replica tier)* → random 4 KiB write → reads under
writeback → fill buffered → fill O_DIRECT → create/unlink.

The order is fixed, and it matters: each stage inherits whatever device state
its predecessor left. The fills run late, right after a stage that deliberately
saturates the device with writes.

---

## The two groups of numbers

They answer different questions and should not be mixed.

**Standard measurements** use block sizes and queue depths pinned identically on
every machine — 4 KiB and `--block`, at QD1, 16 and 32. They are comparable: to
a datasheet, to an `fio` number someone quotes, to another machine. They are not
predictive of your job, because your job does not read like a datasheet.

**Pattern measurements** use the shapes a cache actually generates, at your job's
concurrency. They predict what a job gets on this storage. They are not
comparable to anything published, and only comparable across machines if
`--threads` matches.

QD1 appears in both roles: it is the latency reference and the denominator of
every scaling factor. A device that returns the same IOPS at QD32 as at QD1 is
capped — a quota, not a device.

---

## Every measurement in detail

### Test-file creation — `build_write_*`

**Mechanics.** Sequential `--block`-sized writes extending a new file, until
`--size` or the time cap, then `fdatasync`. The reported rate includes that
flush. If the window was long enough to split, the first and last quarters are
reported separately.

**How to read it: not as a device specification.** It pays allocation of fresh
extents, runs before anything is warm, and its window length depends on
`--size`. On one idle SATA SSD, five runs of a single command reported 268.5,
451.9, 371.9, 303.5 and 397.1 MB/s — a 68% spread — while the in-place
sequential write on the same device held to 6%.

What it *is* good for is its **shape**, which the tool always prints:

```
test-file creation  268.5 MB/s   (build 180 s: 436.9 -> 268.5 MB/s, FALLING
                                  — slowed through the window)
```

- `FALLING` — the window ran long enough to leave whatever was absorbing writes
  early on. The last quarter is a real sustained rate.
- `RISING` — it never got there, or allocation paced the start. The figure is a
  **ceiling**: a longer write would report less.
- `flat` — the two quarters agree within 10%.
- `unsplit` — the window was too short to divide, so there is nothing to say.

The shape is a fact about the **run**, not only the device. The same 64 GiB build
on one SSD reported `FALLING 437 → 268` straight after a heavy write job and
`RISING 342 → 452` after that device had been idle for forty minutes.

**A trap the window length sets.** An 8 GiB build finishing in 12 s can report
burst 448 and sustained 521 — *agreeing*, because both quarters are inside
whatever absorbed the writes — while a 64 GiB build on the same device shows a
2× split. Without `build_write_window_s` beside them, the short run is the more
misleading of the two. Always read the window length.

**For a write number you can quote, use `seq_write_mbps`** below.

### `fdatasync` p50

Five 4 KiB writes at scattered offsets, each followed by `fdatasync`, on a
buffered descriptor; the median is reported. This is the durability cost of a
small metadata update — the pattern a cache uses when flushing sidecar state.
Microseconds to a fraction of a millisecond is healthy; several milliseconds
means every state update pays for a device round trip.

### Sequential read, QD1 — `seq_read_mbps`

One stream reading `--block` (4 MiB by default) sequentially through the test
file, cycling back to its start rather than extending anything. The plain
single-stream bandwidth number, comparable to a datasheet.

### Sequential write, QD1, in place — `seq_write_mbps`

**The quotable write figure.** One stream writing `--block` blocks over the
*already allocated* test file, so no allocation is involved, and it reproduces
across runs to about 6% on a device whose specification it matches to 96–99%.
In buffered mode the stage `fdatasync`s before it stops, so writeback is billed
inside the window rather than left for the next stage to pay.

### Random 4 KiB read, QD1 / QD16 / QD32 — `randr<N>_*`

4 KiB reads at uniformly random 4 KiB-aligned offsets, one thread per queue
depth, each with its own descriptor; per-operation latency is sampled and
reported as p50/p95/p99.

The three depths are the point:

- **QD1** is the latency reference. Its p50 is the single most diagnostic number
  in the record: *microseconds* means a local disk; *milliseconds* means
  network-backed storage where warm reads may not beat the origin.
- **QD16** is where device grades are usually defined.
- **QD32** is what datasheets quote, and full NCQ depth for SATA.

Read the scaling factors the tool prints next to them. Healthy local storage
scales several-fold from QD1 to QD16 — 8.3 k → 94 k IOPS (11×) on one SATA SSD.
**Flat scaling exposes a quota:** a volume that returns the same IOPS at QD32 as
at QD1 is enforcing a limit, and no amount of concurrency will get past it.

Also watch the latency *shape*: on that same SSD, p50 doubles between QD16 and
QD32 (0.16 → 0.32 ms) while IOPS gains only 4%. Past 16, extra concurrency was
buying latency, not throughput.

### Random 4 KiB write, QD32 — `randw_iops`

The same pattern, writing. Scattered small writes are the worst case for
flash-based storage and for quota-priced volumes, and QD32 is the depth
specifications quote.

### Sequential read at job concurrency — `pat_seq_read_mbps`

`--threads` streams, each owning its own slice of the test file and reading
`--block` sequentially inside it. The printed `(N.NNx QD1)` factor is what
concurrency buys for streaming reads. On a device already saturated by one
stream this is ~1.0, and that is useful to know: it means adding analysis
threads will not extract more bandwidth from this disk.

Per-stream buffers are capped at 1 GiB in total. On very high `--threads` the
tool shrinks the block rather than the stream count, because the stream count is
what you asked to measure.

### Byte tier — random 48 KiB read at job concurrency — `pat_rand48k_read_*`

**Mechanics.** 48 KiB reads at random 48 KiB-aligned offsets, `--threads` deep.

**Why this shape.** A byte cache stores a file in its original layout, so a read
can only be as large as the client asked for, and the next thing the analysis
wants usually sits elsewhere in the file behind data it did not request. Measured
over a real 787-file dataset, that tier served reads averaging 42 KiB with no
useful locality between them — hence random, at 48 KiB.

**Why not interpolate from the standard points.** 48 KiB sits between 4 KiB and
4 MiB precisely where a device stops being operation-bound and becomes
bandwidth-bound. The `--sweep` ladder shows the knee directly; on one SATA SSD at
QD4: 4 KiB → 131 MB/s, 16 KiB → 306, 48 KiB → 487, 128 KiB → 561, and flat from
there. Nothing in that curve can be guessed from its endpoints.

### Replica tier — sequential 512 KiB read at job concurrency — `pat_replica_read_*`

**Mechanics.** `--threads` streams reading 512 KiB blocks **sequentially**, each
in its own region.

**Why sequential and not random.** A replica stores each branch's data
contiguously, so consecutive reads land next to each other and one operation can
cover several of them. The same dataset that produced 2.38 M reads averaging
42 KiB from the byte tier produced **170 k reads averaging 599 KiB** from the
replica tier — about **14× fewer operations for the same work**.

Comparing this row against the byte-tier row tells you how much that difference
is worth *on this device*. Where bandwidth is the constraint the two rows are
close. Where operations are priced — an IOPS-quota volume — the gap between them
is the whole reason to complete a recompression.

### Fill pattern, buffered and O_DIRECT — `fill_buffered_*`, `fill_direct_*`

The write pattern a cold fill generates. This is the most subtle measurement in
the tool and has [its own section below](#the-fill-stage-in-detail).

### Random 4 KiB reads under writeback, QD4 — `mixed_*`

**Mechanics.** One writer streams `--block` blocks through the test file
continuously while four readers do 4 KiB random reads. Reported: the readers'
IOPS and p50/p99, plus what the writer achieved.

**Why it is here.** This is the normal mixed mode in production — warm reads
being served while a fill writes — and it is where quota-priced storage falls
apart, because reads and writes compete for the same budget. Compare its read
IOPS against the QD1 random-read row: on one SATA SSD the readers dropped from
8.3 k IOPS to 755 with p99 at 14.9 ms. That is the honest cost of reading from a
disk that is simultaneously being filled.

### create / unlink (untimed)

200 tiny files created then removed, timed as two batches. These are the
metadata rates behind eviction and cleanup. On latency-bound filesystems unlink
rate is what makes a large cleanup take minutes rather than seconds.

---

## The fill stage, in detail

### What it models

A cold fill writes through **one stream** at roughly **48 KiB per write**,
regardless of how many threads the analysis runs — because cache entries are
keyed per file and drained per entry. The stage reproduces that: each writer
creates a file, extends it with `block`-sized writes, `fdatasync`s, closes,
unlinks, and starts the next one. File creation and turnover are *inside* the
measurement, which is what makes a fill different from writing to a file that
already exists.

Defaults, and why they are what they are:

| parameter | default | why |
|---|---|---|
| `writers` | 1 | measured: a fill drains one entry at a time, identical at 8 and 32 analysis threads |
| `block` | 48 KiB | measured mean write size of a real cold fill |
| `file` | 256 MiB | representative cached-file size; also bounded by `--size`/4 per writer |

Override with `--fill writers=2,block=64k,file=512m`. The mean write size the
filesystem actually issued is reported back (`48.0 KiB writes`) — if that
disagrees with what you asked for, the filesystem changed your writes, and *that*
is the finding.

**The `fdatasync` is inside the timed window on purpose.** Without it, a fill
smaller than the machine's dirty-page limit measures RAM, not the disk. The
context block prints that limit so you can see how much headroom there was.

### Why there are two modes

**Buffered is the real one** — it is what a cache does. **O_DIRECT is the
control**: it shows what the device does with 48 KiB writes when the kernel is
not allowed to merge them. Buffered faster than O_DIRECT means the kernel is
merging small writes into larger device writes, and by how much.

The comparison is machine-specific, and it inverts. On a host with a 37 GiB
dirty limit, buffering won clearly; on a host where `vm.dirty_bytes` was set to
about 107 MB, it did not. This is why the writeback configuration is in the
context block.

### The distribution, and the order statistics

A 60 s window completes roughly 100 whole file cycles, so there is a
distribution rather than a single number, and the tool reports it. Only
**complete** cycles count — the last file is usually cut off by the deadline, and
a partial slice is not a rate.

```
fill pattern, buffered        420.3 MB/s   1.6 files/s   48.0 KiB writes
  per file: p50 432.4   p10 370.9   p90 438.5 MB/s   (94 files, p90/p10 1.18x)
  in order: first quarter 398.0 -> last quarter 433.6 MB/s (+9.0%), lag-1 autocorr +0.74
            <- DRIFTS: mean depends on where the window fell, not sigma/sqrt(n)
```

Percentiles alone cannot tell random scatter from a trend, because sorting throws
away the one thing that distinguishes them. So the tool also reports two
**order-sensitive** statistics, computed before the sort:

- **first quarter → last quarter**, in order. A drift.
- **lag-1 autocorrelation** — how much each cycle resembles the one before it.
  Near 0 = independent samples. Positive = slow cycles cluster. Negative =
  alternating.

The verdict line spells out what they mean for the mean:

| verdict | condition | what it means |
|---|---|---|
| `DRIFTS` | quarters differ by >3% | The mean depends on where the window fell. Averaging more runs does **not** shrink the uncertainty — the effect is systematic, not noise. |
| `CLUSTERED` | lag-1 > +0.3 | Effective sample count is smaller than it looks; the mean is softer than `σ/√n`. |
| `ALTERNATES` | lag-1 < −0.3 | Deviations cancel; the mean is **better** determined than independent sampling would give. |
| independent | neither | `σ/√n` applies, and repeating runs genuinely helps. |

### Reading it in practice

Two worked observations, both from repeated runs on one SATA SSD.

**A large per-file spread is not necessarily instability.** A run showing
p10 371 vs p90 439 (1.18×) also showed the quarters rising +9.0% with lag-1
+0.74. That is a *warm-up ramp*, not scatter: p10 is the early files and p90 is
steady state. The same command's mean reproduced across runs to 1.1%.

**Which figure is stable can differ from the headline.** Across runs at two
different `--size` values, the buffered *means* differed by 3.4% (420 vs 435)
while `p50` and the last quarter agreed to ~1% (432 vs 436, 434 vs 433). The
mean differed only because a larger test file leaves more residual writeback to
drain, so more warm-up sat inside the window. **When the shape says `DRIFTS`,
p50 is the more transferable number.**

**And the two modes do not have the same reliability.** On that device the
buffered mean reproduced to about 1% at fixed settings, while the O_DIRECT mean
ranged over 9.5% across nine runs with no accompanying drift, load, or device
counter to explain it. Treat the O_DIRECT fill as ±5% and do not read a
350-vs-380 difference between two machines as real.

---

## The run-context block

Without it, a number in a log file is unreadable later: you cannot tell which
device answered or what else was happening. Every record carries this, including
failed runs.

```
--- run context ---
  when      2026-08-10 13:14:45 +0200   (ucache 0.18.3, build v0.18.3, 852.5 s wall)
  command   ucache bench --size 64g --measurement-duration 60 --threads 32 /path
  machine   host   5.14.0-503.el9 x86_64   64 cpu   187.1 GiB RAM
  cpu       Intel(R) Xeon(R) Silver 4216 CPU @ 2.10GHz
  writeback dirty_ratio 20% of RAM / background 10%  -> buffered writes are free up to ~37.4 GiB
  load      1.24 9.02 8.14 at start -> 3.08 12.84 11.60 at end   (cpu 1.2% busy, 12.1% iowait)
  target    /path
  mount     /path   type xfs   source /dev/sdb1   dev 8:17
  mountopts rw,nosuid,nodev,relatime / rw,seclabel,attr2,inode64,logbufs=8,logbsize=32k,noquota
  device    sdb (partition sdb1)   SAMSUNG MZ7LH1T9   rotational=0   sched=mq-deadline   1788.5 GiB
  device BEFORE the run (2.0 s sample): read 0.0 MB/s, wrote 0.1 MB/s, busy 0%   (idle)
  device IO during the run (this benchmark + everything else): read 179.5 GiB in 12879418 ops,
    wrote 190.3 GiB in 5306587 ops, 88% util
    of which this benchmark issued: read 179.5 GiB, wrote 190.2 GiB
    unattributed remainder:         read 0 MB (0.0%), wrote 92 MB (0.0%)
```

Field by field:

- **when / command / build id.** The build id identifies the binary, since the
  version is constant between releases. A record you cannot tie to a binary is
  hard to defend.
- **machine, CPU, RAM.** Needed to interpret the writeback line, and to know
  whether `--threads` was a large or small fraction of the machine.
- **writeback.** The dirty-page limit, in GiB, is how much buffered writing this
  machine can absorb before a writer blocks — the single most important number
  for interpreting the buffered fill. The tool names *which* knob governs,
  because the kernel reports `dirty_ratio` as 0 when `vm.dirty_bytes` is set, and
  printing both flat reads as a contradiction.
- **mount / device.** Resolved by matching the path's device number against the
  mount table, then reading the block device's model, scheduler, rotational flag
  and size. This is what stops "the fast disk" from being ambiguous a month
  later. A path with no block device behind it (network filesystem, `tmpfs`)
  says so.
- **load before → after.** Context, with a caveat: load average counts tasks in
  uninterruptible sleep as well as runnable ones, it decays over minutes, and it
  covers the whole machine. A high load can be caused *by* the benchmark. Do not
  read it as a measure of interference.

### Proving nothing else was using the disk

This is what the last three lines are for, and it is worth being precise about
what does and does not work.

**Device utilisation does not prove it.** `88% util` is derived from the time the
device had at least one request in flight. On a device that queues, that number
saturates well before the device does, and it cannot separate our requests from
anyone else's. It is a weak occupancy signal, nothing more.

**Byte counters do.** The tool records exactly how many bytes each stage issued
and compares that against the block device's own counters over the same
interval:

```
device IO during the run: read 179.5 GiB, wrote 190.3 GiB
of which this benchmark issued: read 179.5 GiB, wrote 190.2 GiB
unattributed remainder:    read 0 MB (0.0%), wrote 92 MB (0.0%)
```

A remainder near zero means nothing else touched this disk during the run. A
large one means something did — and then the measurements are contaminated and
you should rerun.

The remainder is a **residual, not a measurement**, and it is an upper bound on
interference: our own filesystem metadata (journal, extent maps, the 200
create/unlink pairs) lands in it too, as does traffic on *other partitions of
the same physical disk*. It is shown in MB with a percentage because at
GiB-with-one-decimal the metadata worth noticing rounds away.

**And the pre-run sample answers the other half.** The counters above cover the
run itself; they cannot say whether the disk was already busy when you started.
So the tool samples the device for 2 s before doing anything:

```
device BEFORE the run (2.0 s sample): read 0.0 MB/s, wrote 0.1 MB/s, busy 0%   (idle)
```

Anything above 5% busy prints `NOT IDLE: another workload is on this device`.
Starting a benchmark onto a disk that is already working produces numbers about
neither workload.

---

## Reading a whole record

A worked pass over one real run — 64 GiB test file, 60 s measurements,
`--threads 32`, on a SATA SSD (specified at 520 MB/s sustained write):

```
  test file               65536 MiB
  test-file creation         528.2 MB/s   (build 132 s: 502.7 -> 528.2, flat)
  fdatasync                    0.05 ms (p50)

  Standard measurements
    sequential 4096 KiB read (QD1)                554.9 MB/s
    sequential 4096 KiB write (QD1, in place)     526.5 MB/s
    random 4 KiB read (QD1)                        8319 IOPS  p50 0.12 ms
    random 4 KiB read (QD16)                      93631 IOPS  p50 0.16 ms  (11.3x QD1)
    random 4 KiB read (QD32)                      97500 IOPS  p50 0.32 ms  (11.7x QD1)
    random 4 KiB write (QD32)                     76280 IOPS  p50 0.38 ms

  Pattern measurements (job concurrency 32 threads)
    sequential 4096 KiB read (QD32)               567.7 MB/s  (1.02x QD1)
    random 48 KiB read (QD32)  [byte tier]        550.9 MB/s   11209 IOPS  p99 2.94 ms
    sequential 512 KiB read (QD32) [replica tier] 567.8 MB/s    1083 IOPS
    fill pattern, buffered                        420.3 MB/s
    fill pattern, O_DIRECT                        352.2 MB/s
    random 4 KiB read under writeback (QD4)         755 IOPS  p50 6.45 ms  p99 14.87 ms
```

1. **Is it a healthy local disk?** QD1 random read p50 is 0.12 **ms** —
   microseconds-scale, so yes. `fdatasync` at 0.05 ms confirms it.
2. **Is anything capped?** QD1 → QD16 scales 11.3×. No quota. Note QD32 adds
   only 4% IOPS while doubling p50: 16 streams is this device's useful depth.
3. **Which write number is real?** `seq_write_mbps` = 526.5, which is 101% of
   the drive's specification, so it is credible. The build figure happens to
   agree here and is labelled `flat`, but that agreement is luck — the same
   figure has come out anywhere from 268 to 452 on this device.
4. **What will a job get?** Both serving tiers land at ~550–568 MB/s, i.e. this
   device is bandwidth-limited, not operation-limited, and the two tiers are
   nearly equivalent *on this disk*. On an IOPS-priced volume the same pair
   would diverge sharply — that is where the 14× operation difference bites.
5. **Concurrency?** Sequential read at 32 threads is 1.02× the single-stream
   figure. One stream already saturates this disk; more analysis threads will
   not extract more bandwidth from it.
6. **What does filling cost?** Reads drop from 8319 IOPS to 755 with p99 at
   14.9 ms while a write stream runs. That is the real cost of serving warm
   reads during a fill, and it is far larger than any of the clean numbers
   suggest.
7. **Is the record trustworthy?** The context block reported the device idle
   beforehand and 0.0% of reads / 0.0% of writes unattributed. Nothing else was
   on the disk.

### The other outcome: a capped volume

The same command on a VM volume, for contrast. This is what you are looking for,
and it is unmistakable once you know the shape:

```
    random 4 KiB read (QD1)                          1017 IOPS   p50 0.86 ms
    random 4 KiB read (QD16)                         1000 IOPS   (1.0x QD1)
    random 4 KiB read (QD32)                         1000 IOPS   (1.0x QD1)
    sequential 4096 KiB read (QD1)                  157.6 MB/s
    random 48 KiB read (QD32)  [byte tier]           39.2 MB/s
    sequential 512 KiB read (QD32) [replica tier]   139.2 MB/s
    random 4 KiB read under writeback (QD4)           328 IOPS
```

- **1.0× scaling at every depth, landing on a round 1000 IOPS.** Devices do not
  do this; quotas do. No amount of concurrency will get more, so adding analysis
  threads cannot help.
- **p50 0.86 ms** — milliseconds, not microseconds. Comparable to fetching from
  a nearby origin, which is the point at which a cache stops being obviously
  worth it.
- **The two tiers differ by 3.6×** (39 vs 139 MB/s), where on the local SSD above
  they were within 3%. That gap *is* the operation price: the byte tier needs
  many more operations for the same bytes, and here operations are the scarce
  resource. On storage like this, completing a recompression is the difference
  between the two rows.
- **Reads collapse to 328 IOPS under writeback** — a third of an already small
  budget, because reads and writes come out of the same one.
- And the run itself reported `stopped at time cap`: the 64 GiB test file only
  reached 28.7 GiB in 180 s. The build stage having to give up is itself a
  measurement.

The record was clean — device idle beforehand, 0 MB unattributed — so none of
this is an artefact. It is what that volume does.

---

## What reproduces, and what does not

Measured by repeating one command on one idle SATA SSD. Use this to judge
whether a difference between two machines is real.

| measurement | reproducibility | notes |
|---|---|---|
| sequential read, QD1 | ~1% | |
| sequential write in place, QD1 | ~6% | the quotable write figure |
| random 4 KiB read, all depths | ~1–2% | the most stable numbers in the tool |
| byte-tier and replica-tier reads | ~1% | |
| fill, buffered (mean) | ~1% at fixed `--size` | but see below |
| fill, buffered (p50) | ~1% across `--size` values | more transferable than the mean |
| fill, O_DIRECT (mean) | **~9.5%** | unexplained; treat as ±5% |
| test-file creation | **up to 68%** | not a device measurement |
| reads under writeback | not characterised | |

Two rules follow:

- **Compare like with like.** Same `--size`, same `--measurement-duration`, same
  `--threads`, same mode (`O_DIRECT` vs `buffered`). The fill couples to
  `--size`, and short windows bias the buffered fill low by around 8% because
  the warm-up is a larger share of a shorter window.
- **A single measurement of a stateful device measures its state.** If a number
  matters, take it twice. Repeating helps only where the shape line says the
  samples are independent; where it says `DRIFTS`, more runs will not tighten
  anything.

---

## Pitfalls

- **`--measurement-duration` is per measurement.** 60 does not mean a 60-second
  run; it means about 15 minutes. Read the plan the tool prints.
- **`--size` is a ceiling.** Check for `stopped at time cap` before comparing a
  file-size-dependent number against another machine.
- **`--size` silently bounds the fill file.** The fill file is capped at a
  quarter of `--size` per writer, so `--size 32m` gives 8 MiB fill files no
  matter what `--fill file=` says. Small `--size` values do not just shorten the
  run, they change what the fill measures.
- **The default log is the working directory.** Running from a directory that
  holds curated records will append to them.
- **Do not quote `build_write_mbps`.** Use `seq_write_mbps`.
- **Do not compare `buffered`-mode numbers with `O_DIRECT` ones.**
- **Interrupting a run strands the test file.** Remove
  `<path>/.ucache-bench.*` by hand.
- **Running it on a live cache directory consumes the space.** The test file is
  real, and on a near-full cache it can trigger eviction.
- **A nearly full disk degrades the fill silently.** The pre-flight reserves
  `--size` + 64 MiB, but the peak is `--size` plus the fill files. With too
  little headroom the fill stage reports a low rate instead of failing.
- **`--threads` is not the core count.** It is what your jobs use.

---

## Options reference

| option | default | meaning |
|---|---|---|
| `PATH ...` | the configured cache dir | one or more directories; several print a comparison table |
| `--threads N` | **required** | job concurrency for the pattern measurements, 1–1024. Never guessed |
| `--size SZ` | `1g` | test-file ceiling (floor 16 MiB); also caps the fill file at `SZ/4` per writer |
| `--measurement-duration S` | `5` | seconds per measurement, 0–300. Aliases: `--phase-seconds`, `--seconds` |
| `--block KB` | `4096` | block size for the sequential measurements; 4–65536 KiB, multiple of 4 |
| `--fill k=v,...` | `writers=1,block=48k,file=256m` | fill-pattern parameters |
| `--sweep` | off | replace the single byte-tier point with a block ladder (4, 8, 16, 32, 48, 128, 512, 4096 KiB) to locate the operation-bound/bandwidth knee. 19 measurements instead of 12 |
| `--log FILE` | `./ucache-bench.txt` | append the record here |
| `--no-log` | | print only, write nothing |

---

## JSON field reference

One `ucache-bench-json:` line per path per run, `"schema":1`. The schema is
**additive only** — fields are added, never repurposed — so a parser written
against an older record keeps working. One historical exception is documented
below.

### Identification and configuration

| field | meaning |
|---|---|
| `schema` | always 1 |
| `host`, `time`, `wall_s` | hostname, ISO start time, total run seconds |
| `version`, `build_id` | uCache version, and the revision the binary was built from |
| `cmd` | the full command line |
| `path`, `fs`, `mode` | target, filesystem name, `O_DIRECT` or `buffered` |
| `total_gb`, `free_gb` | filesystem capacity and free space at start |
| `file_mb`, `size_capped` | test-file size actually reached, and whether the time cap stopped it |
| `measurement_s`, `block_kb`, `threads` | the three governing parameters |
| `error` | empty on success; the failure reason otherwise |

### Test-file creation

| field | meaning |
|---|---|
| `build_write_mbps` | last quarter of the build window (or the whole thing if unsplit). **Not a device figure** |
| `build_write_burst_mbps` | first quarter |
| `build_write_window_s` | how long the build ran — required to interpret the pair |
| `build_write_shape` | `flat`, `FALLING`, `RISING`, or `unsplit` |

### Standard measurements

| field | meaning |
|---|---|
| `seq_read_mbps` | sequential read, QD1, at `block_kb` |
| `seq_write_mbps` | sequential write in place, QD1 — **the quotable write figure** |
| `std_block_kib`, `std_qd`, `std_qds` | the pinned block size, the write queue depth, and the read depths |
| `randr<N>_iops`, `randr<N>_us_p50/p95/p99` | random 4 KiB read at queue depth `<N>`, one set per depth in `std_qds` |
| `randw_iops`, `randw_us_p50/p99` | random 4 KiB write at `std_qd` |
| `fsync_p50_ms` | median `fdatasync` |
| `create_ps`, `unlink_ps` | metadata rates |

`seq_write_mbps` is the historical exception: it kept its name when its meaning
moved from the build stage to the in-place measurement, because the build figure
turned out not to be reproducible. Records older than that carry the build
number under this key.

### Pattern measurements

Present only when `--threads` was given.

| field | meaning |
|---|---|
| `pat_seq_read_mbps` | sequential read at `threads` streams |
| `pat_rand48k_read_mbps`, `_iops`, `_us_p99` | byte tier: random 48 KiB. With `--sweep`, one set per ladder size as `pat_rand<N>k_read_*` |
| `pat_replica_read_mbps`, `pat_replica_read_iops` | replica tier: sequential 512 KiB |
| `mixed_read_iops`, `mixed_us_p50/p99`, `mixed_write_mbps` | random 4 KiB reads under writeback, and what the competing writer got |

### Fill pattern

`fill_buffered_*` and `fill_direct_*`, with identical shapes:

| field | meaning |
|---|---|
| `fill_writers`, `fill_block_kib`, `fill_file_mib` | the parameters used (note `--size` may have lowered the file size) |
| `fill_*_mbps` | mean rate over the window |
| `fill_*_files_ps`, `fill_*_write_kib` | file turnover, and the write size the filesystem actually issued |
| `fill_*_p10/p50/p90_mbps`, `fill_*_samples` | the per-file distribution over complete cycles |
| `fill_*_q1_mbps`, `fill_*_q4_mbps` | mean of the first and last quarter, **in order** |
| `fill_*_lag1` | lag-1 autocorrelation of the ordered series |

### Run context

| field | meaning |
|---|---|
| `kernel`, `arch`, `ncpu`, `mem_gb`, `cpu_model` | machine |
| `dirty_ratio`, `dirty_background_ratio`, `dirty_limit_gb`, `dirty_absolute` | writeback configuration; `dirty_absolute` true means `vm.dirty_bytes` governs and the ratios are meaningless |
| `mount`, `mount_fstype`, `mount_source`, `mount_opts`, `mount_super_opts`, `dev` | resolved mount |
| `dev_name`, `dev_model`, `dev_rotational`, `dev_sched`, `dev_size_gb` | the block device (absent if there is none) |
| `pre_read_mbps`, `pre_write_mbps`, `pre_busy_pct`, `pre_sample_s` | device state *before* the run |
| `load1_start`, `load1_end`, `cpu_busy_pct`, `cpu_iowait_pct` | machine load |
| `own_read_mb`, `own_write_mb` | what this benchmark issued — the basis for attribution |
| `dev_read_mb`, `dev_write_mb`, `dev_read_ops`, `dev_write_ops` | what the block device saw |
| `dev_util_pct` | occupancy; **cannot** attribute load, use the byte counters for that |

---

## See also

- `docs/CACHE_MANAGEMENT.md` — choosing and sizing a cache location, reference
  device grades, and `ucache-netbench` for measuring the origin you would be
  caching *from*. A cache location is worth having only if these numbers beat
  the origin's.
- `docs/STATS.md` — what the plugin measures about real jobs, as opposed to what
  this tool measures about the disk.
