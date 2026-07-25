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
6. **You tested with `xrdcp`.** Recent `xrdcp` (5.8+) transfers with PgRead by
   default, which the plugin relays to the origin without caching — `stats`
   stays at zero even though the plugin is loaded (opens count). Analysis reads
   (ROOT, RDataFrame, uproot) use Read/ReadV and cache normally. To smoke-test
   with `xrdcp`, disable its PgRead path: `XRD_CPUSEPGWRTRD=0 xrdcp …`.
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

## `recompress = on` but no replicas ever appear

**Ask the tools first — they diagnose this for you:**

```sh
ucache doctor      # names the cause, and exits nonzero on it
ucache status      # queue depth + the reason, cheaply
```

`doctor` reports `recompress = on but no replicas exist (N queued)` together with
the specific cause. Full detail always lands in `<cache-dir>/recompress.log`, but
you should no longer need to read it to find out *why* nothing happened. The
causes, in order of likelihood:

1. **The source codec is not in `recompress_codecs`.** The default is `lzma`
   only, so a ZLIB- or ZSTD-compressed dataset is declined in full. A sweep says
   so in as many words, naming the codec it actually found and the fix:

   ```
   recompress: 0 recompressed, 9 declined (source codec zlib, not in recompress_codecs = lzma), 0 failed
     nothing was recompressed: this cache holds zlib content, but recompress_codecs = lzma.
     to transcode it:  ucache set recompress_codecs zlib
   ```

   `ucache branches <url>` shows the codec per branch if you want to look first.

2. **The `ucache` CLI is not reachable.** The plugin builds replicas by spawning
   `ucache` itself, resolved from `PATH`. When it cannot be found you get one
   warning — `recompress is on but the ucache helper is not executable` — and
   nothing is queued, since nothing could drain it. Either make the CLI reachable
   by the process running your analysis (`which ucache` from the same shell), or
   point at it explicitly with `UCACHE_RECOMPRESS_HELPER=/path/to/ucache`. An
   explicit `ucache recompress` needs no helper and works regardless.

3. **There is no headroom above the eviction floor.** Background builds are
   declined rather than pushing your cache into eviction, and the log records
   what was deferred and by how much. Free some space, or set
   `recompress_reclaim = full` so replicas replace byte copies instead of adding
   to them. See "The cache is filling my disk".

4. **Nothing qualifies yet.** Only branches your jobs actually read, fully
   cached, are eligible. A first pass that was interrupted, or a file whose
   needed baskets were never completely fetched, is reported as `incomplete` and
   retried on a later pass.

If replicas exist but you see no speedup, coverage is probably partial — see
the recompression section of the User Guide: unreplicated files pace the loop,
so finish with an explicit `ucache recompress`.

## `failed` vs `incomplete` vs `deferred` in `recompress.log`

Only `failed` means something is wrong. The others are ordinary states with
their own words, because conflating them made healthy runs look broken:

- **`incomplete (bytes not cached)`** — the builder wanted basket bytes the cache
  could not vouch for: not fetched yet, or reclaimed while the build was running.
  It retries for free on the next pass and needs no action. The message reads
  `not built yet, will retry`.
- **`deferred (no space)`** — no headroom above the eviction floor; the entry
  stays queued for a pass that has room. See cause 3 above.
- **`declined (source codec …)`** — working as configured; see cause 1 above.
- **`failed`** — a real build failure, e.g. a malformed or unusual file. The
  message names the branch. A failed *build* cannot produce wrong data: a replica
  is only ever served after it has been verified. Confirm serving is clean with
  `ucache stats` — `crc_failures 0`, `failopen_events 0`, `validations_failed 0`.
  If failures persist for the same file and coverage never completes, report it
  with the branch name from the log.

## Corruption / CRC

Every cached page carries a CRC32C, verified on read; a mismatch quarantines that
page (it's re-fetched next time) and increments `crc_failures` — wrong bytes are
never served. To scrub an entry on demand: `ucache verify <url>` (it reports
`checked`/`bad`; bad pages are quarantined, not the whole entry). To drop and
re-fetch an entry entirely, remove it and let the next read repopulate.

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
