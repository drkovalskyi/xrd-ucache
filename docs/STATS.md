# Metrics

The numbers uCache records about your jobs: what each one means, and the format
of the JSON files it writes.

**To simply see how your cache is doing, run `ucache stats`** — it prints a
human-readable summary of these same numbers. This file is for reading the raw
JSON, or building tooling on it. The user guide's monitoring section explains
how to read the printed summary.

## Which number answers which question

| question | look at |
|---|---|
| Is the cache being used at all? | `opens`, `files_opened`, `hit_bytes` |
| Did a warm pass avoid the network? | `origin_bytes` — 0 means everything came from local disk |
| Is anything wrong? | `crc_failures`, `failopen_events`, `validations_failed` — **all three should be 0** |
| Is the cache big enough? | `admissions_bypassed`, `evicted_bytes`, `evicted_entries` |
| Where did the bytes come from? | `ram_hit_bytes`, `hit_bytes`, `replica_bytes_served`, `miss_bytes`, `relay_bytes` |
| Is the job re-reading the same data? | `opens` / `files_opened`, `first_touch_bytes` against `hit_bytes` |
| What is the cache disk being asked to do? | `hit_disk_reads`, `hit_disk_bytes`, `replica_reads`, `replica_read_bytes`, `hist_*_read_bytes` |
| Is the cache disk keeping up? | `buffer_stalls`, `buffer_stall_us`, `hist_hit_read_us`, `hist_flush_write_us` |
| Did the replica tier get used? | `replica_opens`, `replica_published`, `replica_bytes_served` |

## Where the numbers are written

These records are also what `ucache summary` and `ucache history` read: the
file name carries the store's start time and the last line carries its end, so
one process's work is bracketed without any periodic sampling, and the
`.files.jsonl` companion says which entries the bytes belonged to. Consumers
should take the file name's `<start_ts>` as the start of the run.

One JSON line is appended to `$UCACHE_DIR/stats/<host>-<pid>-<start_ts>-<seq>.jsonl`
at every `CacheStore::dumpStats()` (explicit calls, store destruction; the
plugin adds close/atexit). External tooling consumes these — the
schema is a correctness surface; changes require updating this file and the
consumers together.

## Line schema

```json
{"ts": 1783300000, "pid": 12345,
 "opens": 0, "validations_failed": 0,
 "hit_bytes": 0, "miss_bytes": 0, "origin_bytes": 0, "served_bytes": 0,
 "origin_reads": 0, "fetches_joined": 0, "origin_readvs": 0, "page_writes": 0,
 "crc_failures": 0, "meta_corrupt": 0,
 "evicted_entries": 0, "evicted_bytes": 0,
 "failopen_events": 0, "admissions_bypassed": 0,
 "open_retries": 0, "open_retries_exhausted": 0,
 "disabled_handles": 0,
 "replica_opens": 0, "replica_published": 0, "replica_invalid": 0,
 "replica_crc_failures": 0, "replica_punched_bytes": 0, "replica_orphans_swept": 0,
 "files_opened": 0, "ram_hit_bytes": 0, "first_touch_bytes": 0,
 "hit_disk_reads": 0, "hit_disk_bytes": 0, "hit_disk_seq": 0,
 "replica_bytes_served": 0, "replica_reads": 0, "replica_read_bytes": 0,
 "relay_bytes": 0,
 "readv_chunks": 0, "readv_calls": 0, "readv_mixed": 0,
 "flush_runs": 0, "flush_run_bytes": 0,
 "buffer_stalls": 0, "buffer_stall_us": 0,
 "hist_hit_read_us": [..], "hist_miss_read_us": [..], "hist_origin_rt_us": [..],
 "hist_open_us": [..], "hist_replica_read_us": [..],
 "hist_flush_write_us": [..], "hist_meta_flush_us": [..],
 "hist_req_read_bytes": [..], "hist_hit_read_bytes": [..],
 "hist_replica_read_bytes": [..],
 "entries": [{"key": "root://host:1094/path", "file_size": 0,
              "cached_bytes": 0, "page_size": 4096}]}
```

## Field notes

What each counter means, and the reasoning behind the ones that are easy to
misread. Three words recur. The **byte tier** is the page-granular copy of the
original file — the ordinary cache. The **replica tier** is a recompressed
overlay built by `ucache recompress`, which serves the same data with less
decompression work; a cache with recompression off has none. A **sidecar** is
the small file beside each cached entry holding its page bitmap and checksums.
[How it works](USER_GUIDE.md#how-it-works-briefly) covers the tiers, and
[FORMAT.md](FORMAT.md) the files.

### Bytes and where they came from

Every read is served from somewhere, and the counters name the source. Bytes
served locally are `hit_bytes` (byte tier) and `replica_bytes_served` (replica
tier); bytes that had to be fetched are `miss_bytes`; bytes the cache never
touched at all are `relay_bytes`. On a warm pass the fetched figure should be
zero.

- `origin_bytes` — bytes requested from the origin **after page rounding**, so
  it is the numerator of read amplification: divide by the bytes the job asked
  for to see how much extra the cache pulled to satisfy them.
- Which layer maintains which counter, when tracing an unexpected value back to
  its source: `miss_bytes`/`served_bytes`/`origin_reads`/`origin_readvs` are
  incremented by the plugin layer; the core owns `opens`, `hit_bytes`,
  `page_writes`, `crc_failures`, `meta_corrupt`, `validations_failed`,
  `evicted_*`, `failopen_events`.

### Capacity, and things going wrong

These are the counters to read first when something looks off. `crc_failures`,
`failopen_events` and `validations_failed` should all be zero. By contrast
`admissions_bypassed` is not a fault at all — it records a deliberate decision
not to cache, and says the cache is under pressure rather than broken.

- `admissions_bypassed` — files NOT cached because every resident entry was
  still inside the eviction protection window (`evict_protect_seconds`), so the
  cache had stopped growing rather than evict data a running job still needs.
  These reads SUCCEED, uncached. Deliberately separate from `failopen_events`:
  that counter means something went wrong, while this is a capacity decision. A
  non-zero value here means the cache is too small for the working set — see
  `ucache status`, which names the remedy.
- `validations_failed` — cached entries discarded because they no longer
  matched the file at the origin: a different size, mtime or checksum, or a
  changed page size. The entry is refetched from scratch, so a read still
  succeeds; a steadily rising count means the data is being rewritten
  underneath the cache.
- `crc_failures` — pages that failed CRC on read and were quarantined
  (refetched later). `meta_corrupt` — sidecars that failed to load (torn or
  damaged) and were rebuilt.
- `failopen_events` — cache-side faults that degraded to pass-through
  (failed page writes, failed sidecar persists). `disabled_handles` —
  handles that tripped `UCACHE_MAX_ERRORS` (plugin layer).
- `open_retries` / `open_retries_exhausted` (plugin layer) —
  transient inner-open failures re-attempted (`UCACHE_OPEN_RETRIES`), and opens
  that ultimately gave up after retrying. Uncounted when caching is off (retry
  still works, just unrecorded — no store to hold the counters).

### Replica tier

Only meaningful once `ucache recompress` has built overlays; all zero
otherwise. They track the life of an overlay: built, adopted for serving,
rejected, or cleaned up. See the replica section of [FORMAT.md](FORMAT.md).

- `replica_opens` — overlays adopted for serving, after the full verification
  done when the entry is opened.
- `replica_published` — overlays successfully built and made available.
- `replica_invalid` — overlays rejected at open as torn, stale or mismatched;
  each rejection also drops the overlay.
- `replica_crc_failures` — overlay pages whose checksum did not match, at open
  or while serving.
- `replica_punched_bytes` — original bytes reclaimed once an overlay covered
  them, freeing the space the byte-tier copy held.
- `replica_orphans_swept` — leftover overlay files removed by eviction, after a
  crash or a version change.

### What the printed summary is derived from

`ucache stats` does the divisions in its `workflow:` block from the counters
below — how often each file was opened, which tier served what, how much of the
reading was re-reading, and what the cache disk was asked to do.

- Workflow counters: `files_opened` — distinct keys opened by this process
  (`opens/files_opened` ≫ 1 = a reopen loop); `ram_hit_bytes` — hit bytes
  served from the fill buffer's staged RAM (subset of `hit_bytes`);
  `first_touch_bytes` — bytes served for the first time in the entry's
  in-process lifetime (the rest are re-reads).
- Byte-tier disk reads: `hit_disk_reads` /
  `hit_disk_bytes` / `hit_disk_seq` — byte-tier disk reads, their bytes, and
  how many continued sequentially from the previous read. **One
  `hit_disk_reads` is one pread covering a whole contiguous run of cached
  pages, not one per page** — a request spanning ten adjacent pages costs one
  read op — so `hit_disk_bytes / hit_disk_reads` is the mean size the device
  sees and `hit_disk_reads` divided by wall time is the op rate it is charged
  for (the currency on IOPS-quota storage). Bytes are counted at whole-page
  granularity, since each page is still checksummed in full. `hit_disk_bytes`
  is directly comparable to versions before reads were coalesced for every
  request that SUCCEEDS (verified byte-identical on the same workload). It
  differs only for a request that fails part way: the older per-page loop had
  already read some pages before discovering an absent one, whereas the planning
  pass now finds that page first and reads nothing — so a run with a non-zero
  acceptance triple can show slightly fewer bytes than an older one.

- Replica-tier reads: `replica_bytes_served` / `replica_reads` /
  `replica_read_bytes` — bytes served from replica overlays, the physical `.tdata` preads that delivered
  them, and the bytes those preads moved (the replica-tier analogue of the
  byte-tier trio, coalesced the same way; `replica_read_bytes` ≥
  `replica_bytes_served` because whole overlay pages are read and verified,
  and a page read for two different user reads counts twice).
- Pass-through and vector reads: `relay_bytes` — pure pass-through, the cache
  never touched them; `readv_chunks` /
  `readv_calls` / `readv_mixed` — vector-read chunks seen, vector reads the
  cache handled (`readv_chunks / readv_calls` = mean batch width), and
  vectors mixing hits with misses.
- Fill path: `flush_runs` / `flush_run_bytes` — coalesced fill-buffer pwrite
  runs and their bytes; `buffer_stalls` / `buffer_stall_us` — fills that had
  to drain synchronously at the buffer cap, and the wall time lost;
  `fetches_joined` — misses that joined an identical in-flight fetch instead
  of fetching again.
- Histograms are log2 buckets: bucket *i* counts samples with
  `floor(log2(v)) == i` (bucket 0 = ≤1); arrays are trimmed of trailing
  zeros, 40 buckets max. The `_us` ones bucket microseconds; the
  `_bytes` ones bucket bytes (bucket 12 = 4 KiB, 16 = 64 KiB).
- Read-shape histograms answer "what is the storage actually asked for":
  `hist_req_read_bytes` — sizes the client requested (one sample per read
  and per vector-read chunk, before the cache decides how to serve them);
  `hist_hit_read_bytes` / `hist_replica_read_bytes` — sizes of the preads
  each tier issued. Issued sizes *smaller* than requested are normal on a
  partially cached entry, because a read stops at any page that is absent or
  still staged in RAM, and at the coalescing cap; issued sizes *larger* than
  requested come from rounding out to whole pages. What the pair is for is the
  shape of the tail: many issued reads far smaller than the requests they serve
  means the entry is fragmented, and on op-priced storage that is what costs.
- `entries` — per-entry page coverage of entries live in the process at dump
  time (`cached_bytes / file_size` = the coverage signal gating replica
  materialization). Closed entries' coverage persists in their
  sidecars and is aggregated by `ucache stats` from there.
- Counters are process-lifetime monotonic; successive lines from one process
  supersede earlier ones (consumers take the last line per file).

## Companion files (same stem)

- `<stem>.files.jsonl` — one record per entry per process lifetime, emitted
  once (at last release, or at the final dump). Consumed by
  `ucache stats --files`:

  ```json
  {"ts": 0, "key": "root://…", "opens": 0, "served_bytes": 0, "ram_bytes": 0,
   "replica_bytes": 0, "disk_reads": 0, "disk_seq": 0, "disk_bytes": 0,
   "first_touch_bytes": 0, "wire_bytes": 0}
  ```

- `<stem>.trace.jsonl` — sampled per-operation IO trace, written only with
  `trace = io` (every `trace_sample`-th operation per class). Records are
  `{"t": <wall µs>, "op": "<operation>", "k": "<16-hex key hash>", "off": 0,
  "len": 0, "us": 0}`; the first record for each key is a legend line
  `{"op": "key", "k": "<hash>", "url": "<full url>"}` mapping the hash back
  to the URL.
