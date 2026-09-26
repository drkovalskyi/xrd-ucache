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
  objects/<hh>/<sha256(key)>.slots   # slot store: a replica built as the file is read
  objects/<hh>/<sha256(key)>.slots.tmp.<pid>.<n>  # transient during its creation
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
  close and every `UCACHE_META_FLUSH_S` (default 30 s) — by time, not only
  when a write arrives, so a process that stops writing and exits without
  running destructors leaves at most one interval of fill behind.
- Sidecar rewrites are atomic: serialize → `<path>.meta.tmp` → rename, under
  `flock(LOCK_EX)` on the entry's `.data` fd (stable inode; the sidecar's
  inode changes on every rewrite). Readers take `LOCK_SH` on `.data` for
  full-meta loads at open.
- A rewrite is a **commit, not a replacement.** Several processes may hold
  one entry open and fill different ranges of it (one job per chunk, or
  several jobs on one node). Under the lock the writer re-reads the sidecar
  and applies only the pages **it** set or cleared since its last store, plus
  a `pinned` flag it changed; every other bit and CRC is taken from the image
  on disk. So a sibling's pages survive, and a page one process cleared
  before punching is not put back by another process's stale view. The writer
  then adopts the committed image, so it serves its siblings' pages as well.
- A fresh entry's empty sidecar is stored at open, under the same lock, so a
  second opener adopts it instead of truncating `.data` under a fill that has
  not published its pages yet.
- The whole-image `meta_crc` makes torn sidecar writes detectable: a corrupt
  or truncated sidecar simply fails to load and the entry restarts empty
  (`meta_corrupt` counted). A crash can therefore only lose cached pages —
  never serve wrong bytes.
- Page writes are idempotent (same origin bytes at the same offsets), so
  concurrent populators — including other processes — never conflict on
  `.data`; their sidecar changes are merged by the commit rule above.
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

## Slot store: `<hash>.slots`, format_version 2

The replica of a file recompressed as it is read (`recompress = on`). The file
is served to readers in its SLOT layout: every convertible basket (TTree) or
page (RNTuple) of the original sits in a slot past the original end, sized so
its converted record fits, followed by zeros. An RNTuple slot is the page
DECODED, followed by 8 bytes: the XXH3-64 of the decoded page, little-endian,
the page's checksum in the format's own convention (the page list flags every
such page as checksummed), so a reader verifies each page it is served. That layout is fixed when the
store is created and never changes, so a reader may reopen the file at any
time and find its offsets unchanged. All integers little-endian.

```
[0, 4096)            header (written once)
[4096, 4096+blob)    layout blob (written once): the layout, as its creator computed it
then, each at a 4096-aligned offset: commit blocks
```

Header:

| offset | size | field | notes |
|---|---|---|---|
| 0 | 8 | magic | `"UCSLOTS1"` |
| 8 | 4 | format_version u32 | = 2. Below 2: not used, and replaced when a new store is made. Above 2 (a newer uCache's store): left in place — never replaced, unlinked or served; the file is served as stored |
| 12 | 4 | layout_version u32 | the layout algorithm. Lower than this build's: store replaced; higher: left in place, as a newer format is |
| 16 | 1 | container u8 | 0 = TTree, 1 = RNTuple |
| 18 | 1 | declined u8 | 1 = the file is not served this way (nothing else follows) |
| 20 | 4 | n_slots u32 | |
| 24 | 8 | origin_size u64 | validators, compared like a replica's (`validate`) |
| 32 | 8 | origin_mtime u64 | |
| 40 | 1 | cksum_kind u8 | |
| 42 | 2 | slot_factor u16 | in hundredths: a TTree slot is ⌊stored basket length × slot_factor / 100⌋ bytes. 300 (3×), fixed; a store an earlier build made with another factor is served with the one it records |
| 44 | 4 | origin_cksum u32 | |
| 48 | 8 | virtual_size u64 | the file size readers are shown |
| 56 | 8 | layout_hash u64 | XXH3-64 of the layout |
| 64 | 8 | store_id u64 | random, chosen at creation |
| 72 | 8 | blob_len u64 | |
| 80 | 4 | blob_crc u32 | CRC32C of the blob |
| 84 | 2 | codecs_len u16 | then the codecs list the store converts (ASCII, ≤ 256) |
| 4092 | 4 | header_crc u32 | CRC32C of bytes [0, 4092) |

The layout blob is `raw_len u64` followed by the raw layout compressed as
ROOT-style ZSTD chunks — each a 9-byte header (`'Z' 'S' 1`, compressed size
u24, uncompressed size u24, little-endian) then one zstd frame, the raw bytes
cut into chunks under 16 MiB — or followed by the raw bytes themselves, which
is recognised by `raw_len` equalling what follows. (A plain zstd decoder does
not read the chunked form.) All integers are little-endian. The raw layout:

| field | encoding |
|---|---|
| magic | `"UCLAYT02"` |
| container | u8 (0 TTree, 1 RNTuple) |
| origin size, virtual size, metadata seek, slots begin | u64 each |
| windows | u32 count, then per window: offset u64, bytes (u64 length + bytes) |
| relocated metadata record | u64 length + bytes |
| read-footprint ranges | u32 count, then per range: offset u64, length u64 |
| relocated branches or column ranges | u32 count, then u32 each |
| slot table | u32 count, then per slot the six varints below |
| RNTuple pages | per slot: stored size u32, checksum flag u8 |

Per slot, in this order, against the slot before (the first against zeros
and against `slots begin`): the branch index delta and the basket index delta
minus one, as zigzag varints; the original seek delta, a zigzag varint; the
original length, a plain varint; the slot length, a varint that is 0 for the
plain factor × original length (TTree only) and length + 1 otherwise; and the
slot's virtual seek minus the previous slot's END, a zigzag varint (0 when
slots follow one another). The layout is decoded and checked, never trusted:
the slots must lie inside the virtual size, in order and without overlap.

Commit block, at a 4096-aligned offset:

| offset | size | field |
|---|---|---|
| 0 | 8 | magic `"USBLOCK1"` |
| 8 | 8 | store_id u64 (must match the header) |
| 16 | 8 | block_len u64 (header + entries + records) |
| 24 | 4 | n_entries u32 |
| 28 | 4 | entries_crc u32 (CRC32C of the entries) |
| 60 | 4 | block_header_crc u32 (CRC32C of bytes [0, 60)) |
| 64 | 24 × n | entries: slot u32, kind u8 (1 ZSTD, 2 raw, 3 kept as stored), 3 pad, record offset u64, record length u32, record crc u32 |
| … | | the records |

A record's CRC32C is seeded with (store_id u64, slot u32, length u32), so it
validates only as the record it was written as, in the store it was written
to. A kept-as-stored entry has no record: the slot is served from the byte
cache's copy of the original basket, with its `fSeekKey` pointing at the slot.
The last valid entry for a slot wins.

- **Releases sharing a cache:** every format keeps the magic at offset 0 and
  format_version at offset 8, so any build can tell a newer store from debris.
  A newer store still counts as the file's store: no compact replica is made
  beside it.
- **Creation:** header and blob are written to `.slots.tmp.<pid>.<n>`, then
  `link()`ed into place. That fails if the store already exists, so a store is
  never visible without its layout.
- **Commit:** under an exclusive `flock` on the file:
  1. read the blocks written since;
  2. skip slots already committed;
  3. write one block at the 4096-aligned offset past the file's end, every
     block read, and the furthest end any block header CLAIMS — valid or not.
     A block cut short by a crash still owns its claimed extent: once the file
     grows past it, its header passes its checks and a reader steps over
     everything inside it, so nothing may be written there.

  A commit whose open file is no longer the one at the path (the store was
  dropped or replaced) writes nothing.
- **Reading new blocks** takes the shared lock without waiting. A block that
  fails its checks is debris of a commit that died, and is stepped over.

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
