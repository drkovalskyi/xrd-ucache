# FORMAT.md — on-disk cache format (entry sidecars v1, replica sidecars v2)

Bump `MetaData::kFormatVersion` on ANY layout change; readers reject other
versions (entry treated as absent and rebuilt).

```
$UCACHE_DIR/                         (0700)
  objects/<hh>/<sha256(key)>.data    # sparse data file, logical size = origin size
  objects/<hh>/<sha256(key)>.meta    # sidecar, layout below
  objects/<hh>/<sha256(key)>.meta.tmp  # transient during atomic rewrite
  objects/<hh>/<sha256(key)>.cost    # CPU-span evidence sidecar (recompression;
                                     #  tmp+rename at close, best-effort)
  stats/<host>-<pid>-<start_ts>-<seq>.jsonl  # per-process stats dumps (docs/STATS.md)
  stats/<stem>.files.jsonl           # per-file lifetime records (docs/STATS.md)
  stats/<stem>.trace.jsonl           # sampled IO trace, `trace = io` (docs/STATS.md)
  LOCK                               # cache-wide flock for eviction
```

`<hh>` = first two hex chars of the sha256. The key is the normalized URL;
CGI/opaque parameters are stripped before hashing so tokens never
reach disk, in keys or file names.

## .meta layout (all integers little-endian; no implicit struct packing)

| offset | size | field | notes |
|---|---|---|---|
| 0 | 4 | magic | `"UCAC"` |
| 4 | 4 | format_version u32 | = 1; mismatch → treat as absent |
| 8 | 4 | page_size u32 | power of two, 4 KiB–1 MiB; fixed at entry creation |
| 12 | 4 | flags u32 | bit0 = pinned, bit1 = complete |
| 16 | 8 | file_size u64 | origin size validator (always checked) |
| 24 | 8 | atime u64 | unix seconds, coarse (≤1 update/min) — eviction LRU key |
| 32 | 8 | origin_mtime u64 | 0 unless `UCACHE_VALIDATE=size+mtime` |
| 40 | 1 | cksum_kind u8 | 0 none, 1 adler32, 2 crc32c (`UCACHE_VALIDATE=cksum`) |
| 41 | 3 | reserved | zero |
| 44 | 4 | origin_cksum u32 | valid when cksum_kind ≠ 0 |
| 48 | 4 | key_len u32 | ≤ 64 KiB |
| 52 | 4 | meta_crc u32 | crc32c of the whole image with this field zeroed |
| 56 | key_len | key | UTF-8, not NUL-terminated |
| — | pad | | zero pad to 8-byte alignment |
| — | ⌈npages/8⌉ | bitmap | LSB-first within each byte; bit i = page i present |
| — | npages×4 | crc32c[u32] | per-page CRC of the page's real bytes; 0 when absent |

`npages = ceil(file_size / page_size)`. The tail page covers only
`file_size − (npages−1)·page_size` bytes; its CRC covers exactly those bytes.

## Integrity rules

- A page is served only if its bitmap bit is set **and** its stored CRC
  matches the bytes read. Any mismatch → page treated as absent (bit
  cleared, CRC zeroed), refetched from origin, `crc_failures` counted.
- Write ordering: page data is written (and optionally fdatasync'd per
  `UCACHE_FSYNC`) **before** its bit is set. The bitmap+CRCs are flushed on
  close and every `UCACHE_META_FLUSH_S` (default 30 s).
- Sidecar rewrites are atomic: serialize → `<path>.meta.tmp` → rename, under
  `flock(LOCK_EX)` on the entry's `.data` fd (stable inode; the sidecar's
  inode changes on every rewrite). Readers take `LOCK_SH` on `.data` for
  full-meta loads at open.
- The whole-image `meta_crc` makes torn sidecar writes detectable: a corrupt
  or truncated sidecar simply fails to load and the entry restarts empty
  (`meta_corrupt` counted). A crash can therefore only lose cached pages —
  never serve wrong bytes.
- Page writes are idempotent (same origin bytes at the same offsets), so
  concurrent populators — including other processes — are benign.
- An entry unlinked while open (eviction) keeps serving through its open fd;
  the owner detects `st_nlink == 0` before sidecar flushes and skips them so
  the entry is not resurrected.

## Replica sidecars: `<hash>.tdata` + `<hash>.tmeta`, format_version 2

A transposed replica adds two files next to the v1
pair — the v1 `.meta` layout above is UNCHANGED (coexistence: a pre-replica
process never reads or writes replica files, and both generations share one
cache dir safely).

`.tdata` — overlay bytes, dense (not sparse): the patched header/directory
windows plus the extension region (re-encoded hot baskets, relocated tree
metadata) that, stitched over the origin bytes, form the virtual view.

> Metadata-at-rest: the relocated tree-metadata key MAY be a ROOT-compressed
> record ('ZS' framing, ZSTD; `fObjlen > fNbytes − fKeylen`) rather than raw —
> it compresses ~5–7× and dominates the overlay for tiny-hot-set files. This is
> a `.tdata`-content change only: **readers are agnostic** — `.tdata` is opaque
> CRC'd bytes, ROOT inflates the key on open exactly as it does the original's
> LZMA key, the `.tmeta` version is untouched by it, and older (raw-metadata)
> replicas keep serving unchanged in the same cache (no migration).

`.tmeta` layout (little-endian, fixed 80-byte header + arrays, whole-image
crc32c at offset 68 computed with that field zeroed). Version 1 wrote a
72-byte header and no origin map; it is still read, with the map empty:

| off | field | notes |
|---|---|---|
| 0 | magic `UCTR` | |
| 4 | format_version u32 | 2 today; 1 still accepted. Anything else is treated as absent — see the version note below |
| 8 | flags u32 | reserved |
| 12 | encoding u8 | 1=ZSTD-1, 2=LZ4, 3=raw (current builders write ZSTD-1) |
| 13 | cksum_kind u8 | origin validator kind |
| 16 | origin_size u64 | validator: must equal the origin's size |
| 24 | origin_mtime u64 | validator per UCACHE_VALIDATE |
| 32 | origin_cksum u32 | validator per UCACHE_VALIDATE |
| 36 | encoder_version u32 | builder codec version pin (repair never mixes encoder builds) |
| 40 | virtual_size u64 | stitched fEND′; the Stat size ROOT sees |
| 48 | tdata_bytes u64 | exact `.tdata` size |
| 56 | key_len u32 · 60 n_extents u32 · 64 n_superseded u32 | |
| 68 | tmeta_crc u32 | whole-image crc32c |
| 72 | n_orig_map u32 (v2 only) | 76..79 padding; a v1 image ends the header at 72 |
| 80 | key, extents (virt_off,len,tdata_off u64×3), superseded (off,len u64×2), orig_map (virt_off,len,orig_off,orig_len u64×4), crc32c u32 × n_overlay_pages | extents sorted, non-overlapping; overlay page = 64 KiB. The map is sorted by virt_off; v1 images have none and the array is absent |

The origin map records, for each piece the builder relocated, the range of the
ORIGINAL file whose content it carries. No SERVED BYTE depends on it —
`.tdata` stays opaque bytes and which bytes come back is exactly what it was
without the map — but the read path does consult it, to record which part of
the original file a read touched. (An earlier wording here said serving never
consults it, which is not the same claim and is not true.) It is
what lets a read served from a replica be reported in the original file's own
coordinates, and so compared with the same read served any other way. It is not
a linear mapping: a recompressed piece has a different length than the piece it
replaced, so an offset inside a mapped range has no meaningful original offset
and the RANGE is the unit. Fields 0..71 are unchanged from version 1, which is
why the crc keeps its place and a v1 image still parses.

**Version skew is a hazard worth knowing about.** A reader that predates a
version simply cannot tell an unfamiliar sidecar from a damaged one, so it
reports corruption and removes the replica; the newer writer then rebuilds it,
and two versions sharing a cache directory can take turns undoing each other's
work. Nothing is lost and no wrong bytes are served, but the transcoding is
paid for repeatedly. Point one version at a cache directory at a time.

Rules:

- **Publish protocol**: `.tdata.tmp` → write → fdatasync → rename `.tdata`,
  THEN `.tmeta` (tmp+fsync+rename). Readers adopt only via a valid `.tmeta`,
  so a visible sidecar implies a complete, durable overlay; a torn publish
  is unadoptable and its debris is swept by eviction (age-guarded 1 h).
- **Open-time full verify**: every overlay page CRC-checked before a view is
  served; any failure (or validator mismatch) quarantines the replica —
  fail-open to the plain v1 view, never wrong bytes. After open, reads
  re-verify touched pages.
- **Punch-and-clear**: when the overlay supersedes original ranges, the v1
  bitmap bits of fully-covered pages are cleared and flushed FIRST, then the
  bytes are hole-punched (`FALLOC_FL_PUNCH_HOLE`) — a crash between costs
  cached pages only; v1 readers refetch punched ranges from origin.
- Replica files are part of the entry's eviction unit: counted in usage,
  unlinked with the entry, dropped on invalidation. Orphans (e.g. an older
  process evicted the entry without knowing about replica files) are swept.
  The `.cost` sidecar is likewise part of the eviction unit and is unlinked
  with its entry.
- **Verify-once marker `<hash>.tok`** (28 bytes, self-checksummed: magic
  `UCTV` + `.tdata` size + mtime(ns) + crc32c of the page-CRC array): ADVISORY — written by
  publish and by the first successful full verify; a matching marker skips
  the open-time full overlay scan (a 32-process batch scans once, not 32×).
  Stale/torn/absent markers just re-run the scan; the per-read page CRC is
  never skipped, so served bytes are always verified regardless.

## Cache-freshness marker (optional): `<hash>.val`

An empty per-entry marker whose mtime records the last time the entry was
validated against the origin (a real remote stat). Written only when
`UCACHE_REVALIDATE_S > 0`; evicted / invalidated / orphan-swept with the
entry like the other sidecars. At open, if the marker is younger than
`UCACHE_REVALIDATE_S` and a `.meta` is present, the plugin trusts the local
entry and skips the remote open+stat entirely — serving from the
byte cache / replica, touching the origin only on a genuine miss (fail-open).
Advisory: a missing/stale marker just triggers a normal revalidation; the
per-page CRC guarantees still hold regardless.
