# STATS.md — stats dump schema

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
 "failopen_events": 0, "open_retries": 0, "open_retries_exhausted": 0,
 "disabled_handles": 0,
 "replica_opens": 0, "replica_published": 0, "replica_invalid": 0,
 "replica_crc_failures": 0, "replica_punched_bytes": 0, "replica_orphans_swept": 0,
 "files_opened": 0, "ram_hit_bytes": 0, "first_touch_bytes": 0,
 "hit_disk_reads": 0, "hit_disk_bytes": 0, "hit_disk_seq": 0,
 "replica_bytes_served": 0, "replica_reads": 0, "relay_bytes": 0,
 "readv_chunks": 0, "readv_mixed": 0,
 "flush_runs": 0, "flush_run_bytes": 0,
 "buffer_stalls": 0, "buffer_stall_us": 0,
 "hist_hit_read_us": [..], "hist_miss_read_us": [..], "hist_origin_rt_us": [..],
 "hist_open_us": [..], "hist_replica_read_us": [..],
 "hist_flush_write_us": [..], "hist_meta_flush_us": [..],
 "entries": [{"key": "root://host:1094/path", "file_size": 0,
              "cached_bytes": 0, "page_size": 4096}]}
```

## Field notes

- `origin_bytes` — bytes requested from the origin **after page rounding**;
  the read-amplification numerator. `miss_bytes`/`served_bytes`/`origin_reads`/
  `origin_readvs` are incremented by the plugin layer; the core
  owns `opens`, `hit_bytes`, `page_writes`, `crc_failures`, `meta_corrupt`,
  `validations_failed`, `evicted_*`, `failopen_events`.
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
- `replica_*` (docs/FORMAT.md replica section) — `replica_opens`:
  stitched views adopted after the open-time full verify;
  `replica_published`: successful publishes; `replica_invalid`: replicas
  quarantined at open (torn/stale/mismatched — each also implies a drop);
  `replica_crc_failures`: overlay page CRC mismatches (open or serve);
  `replica_punched_bytes`: v1 bytes reclaimed by punch-and-clear;
  `replica_orphans_swept`: crash/mixed-version debris removed by eviction.
- Workflow counters (`ucache stats` derives its `workflow:` block from
  these): `files_opened` — distinct keys opened by this process
  (`opens/files_opened` ≫ 1 = a reopen loop); `ram_hit_bytes` — hit bytes
  served from the fill buffer's staged RAM (subset of `hit_bytes`);
  `first_touch_bytes` — bytes served for the first time in the entry's
  in-process lifetime (the rest are re-reads); `hit_disk_reads` /
  `hit_disk_bytes` / `hit_disk_seq` — byte-tier disk reads, their bytes, and
  how many continued sequentially from the previous read;
  `replica_bytes_served` / `replica_reads` — bytes served from replica
  overlays and the physical `.tdata` preads that delivered them (one per
  overlay page touched, so a page read for two different user reads counts
  twice — the replica-tier analogue of `hit_disk_reads`; divide bytes by reads
  for the mean read size, and reads by wall time for the op rate the storage
  device actually sees); `relay_bytes`
  — pure pass-through, the cache never touched them; `readv_chunks` /
  `readv_mixed` — vector-read chunks seen / chunks in mixed hit+miss
  vectors; `flush_runs` / `flush_run_bytes` — coalesced fill-buffer pwrite
  runs and their bytes; `buffer_stalls` / `buffer_stall_us` — fills that had
  to drain synchronously at the buffer cap, and the wall time lost;
  `fetches_joined` — misses that joined an identical in-flight fetch instead
  of fetching again.
- Histograms are log2 buckets of microseconds: bucket *i* counts samples
  with `floor(log2(us)) == i` (bucket 0 = ≤1 µs); arrays are trimmed of
  trailing zeros, 40 buckets max.
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
