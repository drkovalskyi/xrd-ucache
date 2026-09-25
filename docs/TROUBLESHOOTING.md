# uCache — Troubleshooting

Start with `ucache doctor` — it checks the install, the cache filesystem, and
activation (whichever way you configured it), and prints `[ OK ]` / `[WARN]` /
`[FAIL]` per item. Then `ucache test <url>` on one of your data files proves
the whole chain dynamically (cold + warm pass; warm must be origin-free).
Most issues below map to one of their lines.

## The cache doesn't seem to be used (warm pass still hits the network)

`ucache stats` after two identical runs should show `origin_bytes == 0` on the
second. If not:

1. **No conf where XrdCl looks.** `doctor` shows `[FAIL] no plugin conf`.
   Write `~/.xrootd/client.plugins.d/ucache.conf` (USER_GUIDE §2) or run
   `ucache setup`. Note XrdCl resolves that `~` from the passwd database, not
   `$HOME` — they can differ on batch systems.
2. **Using the `XRD_PLUGINCONFDIR` route in a shell or job that doesn't set
   it.** The default-directory conf needs no environment at all; but if you
   keep the conf elsewhere via `XRD_PLUGINCONFDIR`, every shell and batch job
   must have that variable set (the job still runs — fail-open — just
   uncached). Easiest fix: move the conf to `~/.xrootd/client.plugins.d/`.
3. **The `*` slot is taken.** If a system plugin conf under
   `/etc/xrootd/client.plugins.d/` already binds `url = *`, your `url = *` user
   conf is shadowed and skipped (`doctor` and `setup` warn about this). Re-run
   `ucache setup --host <host:port>` (e.g. `eospublic.cern.ch:1094`) to bind your
   data host explicitly.
4. **Host mismatch.** The cache key includes host:port. If you read via a
   federation redirector on the cold pass and a direct site URL on the warm pass
   (or vice versa), they are different keys. Read via a consistent URL.
5. **Caching disabled.** `UCACHE_DISABLE=1`, `disable = true` in the conf, or
   `ucache disable` was run. Re-enable with `ucache enable`.
6. **You tested with `xrdcp`.** A copy never uses the cache, by design: `xrdcp`,
   `xrdfs`, `xrdadler32`, XRootD's copy engine from any program (Python's
   `CopyProcess`, `gfal-copy`, `rucio download`), ROOT's `TFile::Cp`, `hadd`
   and ROOT's command-line tools (`rootcp`, `rootls`, ...) are
   recognised and read straight from the origin, because a copy must be the
   origin's bytes and uCache may show readers a file in a layout of its own.
   `stats` then shows `copier_handles` and direct (relayed) bytes, not hits —
   which also proves the plugin is loaded. Analysis reads (ROOT, RDataFrame,
   uproot) cache normally. To smoke-test, use `ucache test <url>`; to push one
   copy through the byte cache, `UCACHE_COPY_DETECT=off
   UCACHE_MAX_READ_FRACTION=100 XRD_CPUSEPGWRTRD=0 xrdcp …` (a copy reads all
   of a file, see item 8; recent `xrdcp` otherwise transfers with PgRead,
   which the plugin relays without caching).
7. **The job's XrdCl refuses the plugin (version or glibc).** Two variants,
   both **silently fail-open** (job correct, nothing cached); diagnose either
   with `XRD_LOGLEVEL=Debug <your job> … 2>&1 | grep -i plug` in the same
   environment:
   - *Version handshake:* XrdCl accepts a plugin only when the plugin's build
     version ≤ the client's. Release artifacts are therefore built against
     the **5.6 ABI floor** (`@V:XrdClUCache v5.6.x`) and load on every 5.6+
     client, including CMSSW externals (verified: xrootd 5.6.4/5.8.4/5.9.6).
     If you build from source against newer headers, older clients (e.g.
     CMSSW's) log `Plugin version client v5.6.4 is incompatible …` and run
     uncached — rebuild against the oldest headers you need to serve.
     **`ucache doctor` now checks this for you**: it compares the plugin's
     declared version against the `xrootd` client on your PATH and FAILs with
     a clear message when the client is too old (silent-refusal) or is a 4.x
     client (a different ABI — `libXrdCl.so.3` is absent, so a 5.x plugin
     cannot load at all).
   - *Which framework release do I need?* Find your client version with
     `xrdcp --version` (or, in a CMSSW area, `scram tool info xrootd`). The
     prebuilt plugin needs xrootd **≥ 5.6**. CMSSW ships modern xrootd in the
     latest patch release of **actively-maintained** cycles — e.g.
     `CMSSW_10_6_50` (2026) carries xrootd 5.7.2, though early patches of the
     same cycle (`10_6_26`) shipped 4.8.5 — so *upgrading to the newest patch
     in your cycle* usually resolves it. **Frozen** cycles never got the bump
     (`CMSSW_10_2_29` stays 4.8.3, `CMSSW_11_3_4` stays 4.12.3) and cannot run
     the plugin at any patch level; move to a maintained cycle.
   - *Older containers* (`cmssw-el8`/`cmssw-el7`): the el9-built `.so` needs
     GLIBC_2.34 and cannot load in an el8/el7 apptainer (glibc ≤ 2.28). el9
     CMSSW releases work as-is; an el8-flavor artifact is future work.
8. **The job reads most of each file.** A job whose reads of a ROOT file (a
   TTree or RNTuple of 64 MiB or more) cover branches holding more than
   `max_read_fraction` of the file's data (default 25%) reads that file
   straight from the origin and does not cache it: one `WARN` per job names
   the first such file and the share it read, and `ucache stats` counts them in
   `direct_read_files`. That is deliberate — such a job would fill the cache
   with the whole dataset. To cache it anyway, `UCACHE_MAX_READ_FRACTION=100
   <your job>`, or `ucache set max_read_fraction 100` for every job.

## `[Error][File] Plug-in factory failed to produce a plug-in … continuing without one`

Harmless despite the Error tag. uCache deliberately provides no *FileSystem*
plugin (locate/stat/query are pure pass-through), and XrdCl logs this line
whenever a client — CMSSW's XrdAdaptor and `edmFileUtil` do — creates an
`XrdCl::FileSystem` for a bound host. Your *file* reads still engage the
cache: check `ucache stats` (opens/hit/miss moving), not this message.

## cmsRun warm runs: `failed to determine data server name` / `server (unknown)`

Harmless warnings, and actually good news: a warm open inside the freshness
window is served **entirely from the local cache — no server is contacted**,
so CMSSW's XrdAdaptor has no data-server name to report and says "(unknown)".
The job runs normally, reads come from disk (`ucache stats`: warm
`origin_bytes ≈ 0`). Two more lines in the same family:
`[Error][PostMaster] Unable to get transport handler for file protocol` and
`[Error][XRootD] [localhost] Unable to send the message kXR_query …` —
XrdAdaptor probing the cache's local-redirect URL for readv limits (it lacks
the guard ROOT and uproot have); the probe fails fast and sane defaults
apply.

## A site's monitoring lists my jobs as `ucache`, not as ROOT or cmsRun

Expected, and it is one setting. uCache names itself as the application in the
login of every session it caches, so that sites can tell traffic that comes
through a cache from traffic that does not; your program's name goes in the
information string beside it (`ucache/1.0.0 (root.exe)`). If a dashboard,
accounting rule or site policy needs your program in the application field
instead, either turn the announcement off:

```sh
ucache set announce off      # or `announce = off` in ucache.conf
```

or set the name yourself, which uCache never overrides:

```sh
export XRD_APPNAME=my-analysis
```

Nothing else changes with it either way — the announcement is two strings in
the login request and is not involved in reading or caching a single byte.

## The file changed at the origin but uCache serves the old bytes

Expected inside the freshness window (default 7 days, `revalidate_seconds`):
a validated entry is served without contacting the origin, so a file
*replaced under the same name* keeps serving the cached version until the
window expires. Physics data is normally write-once, which is why the default
trusts the cache. When you know a file changed:

- `ucache rm <url>` — drop that entry; the next open re-fetches.
- `UCACHE_REVALIDATE_S=0 <your job>` — force origin re-checks for one run.
- `revalidate_seconds = 0` in your `ucache.conf` — always re-check (the
  pre-0.9.1 behavior; adds a remote open+stat to every open).

## A copy of a file does not match the origin (size or checksum)

A copy that uCache recognises is read straight from the origin and is the
origin's bytes: `xrdcp`, `xrdfs`, `xrdadler32`, `edmCopyUtil`, XRootD's copy
engine from any program (Python's `CopyProcess`, `gfal-copy`, `rucio
download`), ROOT's `TFile::Cp`, `hadd`, ROOT's command-line tools (`rootcp`,
`rootmv`, `rooteventselector`, `rootslimtree`, `rootls`, `rootprint`, ...) and
ROOT's file merger (`TFileMerger`, given the file's name). `ucache stats`
counts each such handle in `copier_handles`.

A copy made any other way reads through the cache like an analysis job, and a
file with a replica is then shown in the replica's layout — the same data to
ROOT, but a larger file with different bytes. That is the case for reading a
file handle in a loop (fsspec's `get` or `open().read()`, a hand-written
loop), fast cloning inside a program of your own (`CloneTree(-1, "fast")` on a
tree it opened), and copies through an XRootD proxy or a FUSE mount with uCache
inside it. Make those copies with the cache switched off, and inspect a file's
real sizes and codecs the same way (`TTree::Print`, `TFile::Map`, the
`RNTupleInspector` report what they are shown):

```sh
UCACHE_DISABLE=1 python3 my_copy.py
```

A copy tool that uses XRootD's copy engine and still is not counted in
`copier_handles` is worth reporting, with its name and the client version.

## `doctor` reports a problem

- **`[FAIL] plugin not loadable`** — the `.so` can't be `dlopen`ed. Usually a
  missing/incompatible `libXrdCl` (wrong XRootD version) or a moved install.
  Reinstall against your host's `xrootd-client-devel` (≥ 5.6); the build fails
  the version gate if it's too old.
- **`[WARN] sparse files`** — the cache filesystem doesn't report sparse
  allocation (some network filesystems). uCache still works but uses more space
  than the logical cached bytes; prefer a local ext4/xfs/btrfs `UCACHE_DIR`.
- **`[WARN] advisory locks (flock)`** — cross-process eviction coordination needs
  `flock`. On filesystems without coherent locking (some NFS setups) eviction may
  be less precise; use a local `UCACHE_DIR`.

## Fail-open events

`ucache stats` shows `failopen_events`. A non-zero count means some reads
degraded to uncached pass-through (e.g. transient ENOSPC, an I/O error, a torn
sidecar). **Your results are unaffected** — fail-open never serves wrong bytes;
it just skips caching for the affected reads. A steadily climbing count points at
a sick cache filesystem (full, read-only, failing disk).

## The cache is filling my disk

By default uCache uses the disk and evicts LRU to keep a free-space floor
(`min(50 GiB, 10% of the volume)` free). If that's too aggressive for your disk:

- Set a hard cap: `UCACHE_MAX_BYTES=50g` (or `max_bytes = 50g` in the conf).
- Raise the floor: `UCACHE_MIN_FREE_BYTES=100g`.
- Check current usage/budget: `ucache status`; force a pass: `ucache evict`.

Note the byte cap is best-effort under extreme concurrent write rates; the
free-disk floor is the hard guard against actually filling the volume.

## Jobs are killed for memory with `recompress = on`

With recompression on, a job holds more memory while it reads a recompressed
file: ROOT sizes its read buffers from the layout the file is shown in and does
not shrink them to fit, and uCache keeps a table for each such file while it is
open (about 20 MB for a large NanoAOD file). On the analyses measured, warm
passes needed about 1.9x (TTree) and 2.3x (RNTuple) the memory of the same job
reading a replica made by `ucache recompress`, which needs about what the job
needs without the cache. A batch system or the kernel then stops the job
without a word from uCache (`ucache doctor` says so while recompression is
on).

- Turn recompression off for these jobs (`UCACHE_RECOMPRESS=off`, or
  `ucache set recompress off`) and build replicas with `ucache recompress`
  instead. The files the killed jobs already read keep their recompressed
  layout whatever the setting, and `ucache recompress` counts them as done:
  remove those first, while no job is reading them (a job still reading one
  would fail) — `ucache untranspose <url>` for one file, `ucache clear` for
  all — read them again (what was converted is fetched again), then run
  `ucache recompress`. Until then `UCACHE_TRANSPOSE=0` serves every file as
  stored, fetching again what was converted.
- For TTree files, a smaller `recompress_slot_factor` (default 3; 2 or 2.5,
  say) shrinks ROOT's share, not uCache's table, and leaves more baskets in
  their original codec; near 1 almost nothing is converted. It applies to files
  that get their replica after the change: a file that has one keeps its own
  until it is removed.

## `recompress = on` but no replicas ever appear

With `recompress = on`, a file your job reads gets its replica while it is read
(`ucache ls` shows its size under RECOMP as soon as records are committed:
every few seconds, and when the job closes the file). If a file stays at 0,
the INFO log names why its layout was declined (`UCACHE_LOG=info`), and
`ucache status` counts declined files on its `declined` line. A file declined
for its codec is decided again at the next read once `recompress_codecs`
lists that codec; data cached before recompression was switched on and not
read since gets a replica only from `ucache recompress`. The rest of this
section covers both.

**Ask the tools first — they diagnose this for you:**

```sh
ucache doctor      # names the cause, and exits nonzero on it
ucache status      # the reason, cheaply
```

`doctor` reports `recompress = on but no replicas exist` together with the
specific cause, when it can identify one; a cache whose data simply predates the
switch has no replica yet and nothing wrong with it. The causes, in order of
likelihood:

1. **The source codec is not in `recompress_codecs`.** The default is
   `lzma,zlib`, the two codecs that are expensive to decode. Data already
   stored in ZSTD or LZ4 is declined in full, because transcoding it to ZSTD-1
   would spend disk to buy nothing. A sweep says so in as many words, naming
   the codec it actually found and the fix:

   ```
   recompress: 0 recompressed, 9 declined (source codec zstd, not in recompress_codecs = lzma,zlib), 0 nothing to do (nothing cached), 0 failed
     nothing was recompressed: this cache holds zstd content, but recompress_codecs = lzma,zlib.
     to transcode it:  ucache set recompress_codecs zstd
   ```

   `ucache branches <url>` shows the codec per branch if you want to look first.

2. **`transpose = off`.** A replica is created on the first pass only where
   replicas are served, so with the replica tier switched off a job's first pass
   neither creates nor serves one. `ucache set transpose on` (or
   `ucache unset transpose`) lets recompression work again.

3. **There is no headroom above the eviction floor.** Converted records that
   still do not fit above the floor after an eviction pass are not kept
   (`ucache stats` counts them as `cold_replica_declined`), and a sweep defers
   each file that would not fit rather than pushing your cache into eviction,
   saying so in its summary. Free some space, or set
   `recompress_reclaim = full` so replicas replace byte copies instead of adding
   to them. See "The cache is filling my disk".

4. **Nothing qualifies yet.** Only branches your jobs actually read, fully
   cached, are eligible. A first pass that was interrupted, or a file whose
   needed baskets were never completely fetched, is reported as `incomplete` and
   retried on a later pass.

If replicas exist but you see no speedup, coverage is probably partial — see
the recompression section of the User Guide: unreplicated files pace the loop,
so finish with an explicit `ucache recompress`.

**Files an older uCache left in the cache directory.** `recompress.pending`,
`recompress.working` and `recompress.log` belonged to the background worker that
uCache 1.2.0 and earlier ran; nothing reads them now. `ucache recompress` removes
the first two (it scans the whole cache, so it covers every file they listed);
`recompress.log` is safe to delete.

## `failed` vs `incomplete` vs `deferred` in `ucache recompress` output

Only `failed` means something is wrong. The others are ordinary states with
their own words, because conflating them made healthy runs look broken:

- **`incomplete (bytes not cached)`** — the builder wanted basket bytes the cache
  could not vouch for: not fetched yet, or reclaimed while the build was running.
  It retries for free on the next pass and needs no action. The message reads
  `not built yet, will retry`.
- **`deferred (no space)`** — no headroom above the eviction floor; the file is
  left for a later `ucache recompress` once there is room. See cause 3 above.
- **`declined (source codec …)`** — working as configured; see cause 1 above.
- **`failed`** — a real build failure: a malformed file, a cache entry whose
  bytes no longer match their checksum (the message then points at
  `ucache verify`), or an entry that has gone missing. The message names the
  branch when the failure is specific to one. A failed *build* cannot produce wrong data: a replica
  is only ever served after it has been verified. Confirm serving is clean with
  `ucache stats` — `crc_failures 0`, `failopen_events 0`, `validations_failed 0`.
  If failures persist for the same file and coverage never completes, report it
  with the branch name from the sweep's output.

## Corruption / CRC

Every cached page carries a CRC32C, verified on read; a mismatch quarantines that
page (it's re-fetched next time) and increments `crc_failures` — wrong bytes are
never served. To scrub an entry on demand: `ucache verify <url>` (it reports
`checked`/`bad`; bad pages are quarantined, not the whole entry). To drop and
re-fetch an entry entirely, remove it and let the next read repopulate.

## `replica ... dropped: torn/corrupt sidecar` right after an upgrade

If this appears for many entries at once, and the entries are ones a *newer*
uCache recompressed, the sidecars are almost certainly fine. An older version
reading a cache directory it shares with a newer one does not recognise the
newer sidecar layout, and an unreadable sidecar is indistinguishable from a
damaged one, so it reports corruption, counts `replica_invalid`, and removes
the replica. The next recompression rebuilds it in the older layout, and the
two versions can then take turns undoing each other's work.

Nothing is lost and no wrong bytes are served — reads fall back to the byte
cache or the origin — but the transcoding is paid for repeatedly, and with
`recompress_reclaim = full` the byte copy was already released, so those reads
go back to the origin.

Point one version at a cache directory at a time. A cache directory is version
coupled state, not a shared scratch area; separate directories cost only disk.
Genuine sidecar damage looks different: it turns up on a few entries rather
than all of them, and it does not correlate with a version change.

## Pinned data got evicted / a pin didn't take

`ucache pin <url>` sets a flag in the entry's sidecar; `ucache ls` shows a `yes`
in the PIN column. If a URL won't pin, it isn't cached yet (pin after a first
read). Pinned entries are skipped by eviction; if the pinned set alone exceeds
the budget, eviction can't shrink below it (that's intended — unpin something).

## Clean slate

`ucache clear` is the supported wipe: it prompts, removes cached data and
settings state, and keeps your conf (`--keep-pinned` spares pinned entries).
Everything uCache stores lives under the configured cache dir (`dir =` /
`UCACHE_DIR`), so removing that directory by hand is equally safe — the next
run repopulates. The only file elsewhere is the plugin conf itself
(`~/.xrootd/client.plugins.d/ucache.conf`, or under `~/.config/ucache/plugins/`
if you use the `XRD_PLUGINCONFDIR` route). `ucache disable` turns caching off
without removing anything; deleting the conf removes all trace.
