// Process-wide cache root: entry registry, validation policy, eviction,
// stats dump.
//
// One FileEntry per key per process (refcounted via shared_ptr; concurrent
// IMT streams share state). Eviction purges LRU-by-atime whole entries from
// HIGH_WATER down to LOW_WATER under the cache-wide LOCK flock, skipping
// pinned entries; entries unlinked while open keep serving via their fd.
//
// Budget: by DEFAULT the cache uses the disk and evicts to
// keep a statvfs free-disk FLOOR (minFreeBytes, auto-set from the disk) — an
// active session fills most of the disk and reclaims LRU under pressure. The
// floor uses statvfs directly (no per-startup scan) and is the accurate
// cross-process guard. An OPTIONAL hard byte cap (maxBytes, via UCACHE_MAX_BYTES)
// adds a second trigger driven by a per-process running usage total (approxUsage_:
// seeded once when the cap is set, adjusted on page-write, reconciled to an
// authoritative scan after every evictNow) — cheap but per-process, hence
// best-effort under concurrency. Eviction checks run on both open() and the
// page-write path, rate-limited by evictCheckSeconds.
//
// Thread-safety: fully thread-safe (registry mutex; Stats + approxUsage_
// atomics; eviction serialized cross-process by flock and in-process by a mutex).
#pragma once

#include "Config.h"
#include "CpuCounters.h"
#include "ReadFootprint.h"
#include "FileEntry.h"
#include "IOBackend.h"
#include "Stats.h"
#include "UrlKey.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace ucache {

class Tracer; // sampled IO tracer (Trace.h)

class CacheStore {
 public:
  // Creates cacheDir/objects + /stats (0700). `io` must outlive the store.
  CacheStore(IOBackend& io, Config cfg);
  ~CacheStore(); // best-effort stats dump

  CacheStore(const CacheStore&) = delete;
  CacheStore& operator=(const CacheStore&) = delete;

  const Config& config() const { return cfg_; }
  Stats& stats() { return stats_; }

  // The free-disk floor eviction actually enforces, for a Config that may not
  // have been through resolveBudget(). A store resolves the automatic floor into
  // its OWN copy of the Config, so a caller holding the pre-resolution Config
  // sees minFreeBytes == 0 and would conclude "no floor, nothing to protect" —
  // which silently disables anything gated on headroom. Ask here instead of
  // reading the field, and the answer is the same wherever it is asked from.
  // Returns 0 only when eviction is genuinely off. Pure: logs nothing.
  static uint64_t effectiveMinFree(const Config& cfg, IOBackend& io);

  // Opens (or shares) the entry for `key`, validating against the origin
  // metadata. nullptr => caller fails open to pass-through.
  // Triggers a rate-limited eviction check.
  // `declinedForSpace` (optional, out) distinguishes the two reasons for a null
  // return, which callers must not conflate: a genuine failure (fail-open, and
  // something is wrong) from a capacity DECISION — every resident entry is still
  // inside the protection window, so this NEW entry is not admitted. The read
  // still succeeds uncached either way, but only the first is a fail-open event.
  std::shared_ptr<FileEntry> open(const UrlKey& key, uint64_t originSize,
                                  uint64_t originMtime = 0,
                                  uint8_t cksumKind = MetaData::kCksumNone,
                                  uint32_t originCksum = 0,
                                  bool* declinedForSpace = nullptr);

  // True once an eviction pass has found the budget exceeded with NOTHING
  // eligible to remove: the cache is full of entries too recently read to touch,
  // so it has stopped growing. Latched by evictNow() and cleared there as soon as
  // a victim becomes eligible again. Reported by `status` and `doctor`.
  bool admissionBlocked() const { return admissionBlocked_.load(std::memory_order_relaxed); }

  // Rate-limited high-water check; runs eviction when above.
  void maybeEvict();
  // Unconditional eviction pass (CLI/tests). Returns entries removed, or
  // -1 when another process holds the eviction lock.
  int evictNow();
  // Sum of cached payload bytes across all sidecars (allocated-data view).
  // Authoritative but O(N): a full sidecar scan. Prefer approxUsageBytes() for
  // hot-path checks; use this for CLI `status` and reconciliation.
  uint64_t usageBytes();
  // Cheap running estimate of this process's view of cache usage.
  uint64_t approxUsageBytes() const { return approxUsage_.load(std::memory_order_relaxed); }

  // One cached object as seen on disk, for the CLI `ls`/`status` (cold path).
  struct EntryInfo {
    std::string key;         // full normalized key
    uint64_t fileSize = 0;   // origin size
    uint64_t cachedBytes = 0;
    uint64_t replicaBytes = 0; // .tdata overlay size; 0 = no replica
    uint64_t atime = 0;
    double coverage = 0.0;   // fraction of the file present in cache [0,1]
    bool pinned = false;
    bool complete = false;
  };
  // Snapshot of every cached entry (authoritative disk scan; torn sidecars
  // skipped). Cold path — use approxUsageBytes()/stats for hot checks.
  std::vector<EntryInfo> listEntries();

  // CRC scrub of one entry (CLI `verify`). The caller MUST pass the entry's OWN
  // validation metadata (from its sidecar) — size AND mtime/cksum — so the
  // re-open adopts under every validate mode; passing defaults would fail §7
  // validation and truncate/wipe the entry instead of scrubbing it.
  FileEntry::ScrubResult verify(const UrlKey& key, uint64_t originSize, uint64_t originMtime = 0,
                                uint8_t cksumKind = MetaData::kCksumNone, uint32_t originCksum = 0);

  // Pin/unpin the entry for `key` (CLI `pin`/`unpin`): mutates the live entry
  // if open, else rewrites the sidecar flag directly under the entry flock
  // (no origin metadata needed — never re-validates, so it cannot wipe data).
  // Returns false if the entry is not cached or the rewrite failed.
  bool setPinnedByKey(const UrlKey& key, bool pinned);

  // Remove a single entry by key (CLI `rm`): byte cache + any replica overlay.
  // Returns false if nothing was cached under that key. A handle still holding
  // the entry keeps serving via its fd (unlink-while-open guard).
  bool removeEntry(const UrlKey& key);

  // Criterion-based bulk removal for the CLI cleanup commands (`clear`, `evict
  // --older-than`, `evict --to-size`). `arg` depends on the mode:
  //   kAll       — remove every entry (arg ignored)
  //   kOlderThan — arg = seconds; remove entries whose last-access (atime) is
  //                older than now-arg
  //   kNewerThan — arg = seconds; remove entries whose last-access is WITHIN
  //                the window (undo a polluting run)
  //   kToSize    — arg = bytes; LRU-remove (oldest first) until the total cache
  //                size (cached + replica) is <= arg
  // keepPinned skips pinned entries. dryRun selects victims and totals bytes
  // WITHOUT unlinking anything (preview). Serialized cross-process by the same
  // eviction LOCK as evictNow(); report.locked is false (and victims empty) if
  // another process holds it.
  enum class CleanupMode { kAll, kOlderThan, kNewerThan, kToSize };
  struct CleanupVictim {
    std::string key;
    uint64_t bytes = 0; // cached + replica
    uint64_t atime = 0;
  };
  struct CleanupReport {
    bool locked = false; // false => another process holds the eviction lock
    uint64_t bytes = 0;  // total across victims
    std::vector<CleanupVictim> victims;
  };
  CleanupReport cleanup(CleanupMode mode, uint64_t arg, bool keepPinned, bool dryRun);

  // Drops the entry for `key`: registry detach + unlink of data+sidecar
  // (any write access to a cached URL invalidates its entry).
  // Handles still holding the entry keep serving via their fds; the
  // unlink-while-open guard prevents sidecar resurrection.
  void invalidate(const UrlKey& key);

  // Appends one JSON line to stats/<host>-<pid>-<start_ts>.jsonl. finalDump:
  // this is the process's LAST dump (atexit/dtor) — also emit the Layer-2
  // per-file records of entries still alive (their destructors may never run;
  // emit-once guard prevents doubles).
  void dumpStats(bool finalDump = false);
  // Per-file record for a handle the cache never served (pass-through:
  // UCACHE_DISABLE, write-opened, no store entry). Same companion file and
  // shape as FileEntry's lifetime records, with the bytes under `wire_bytes` —
  // it is what a cache-disabled BASELINE run leaves behind, and the gain
  // readout matches later cached runs against it by key.
  void recordRelayObs(const std::string& url, uint64_t bytes, const char* mode = "relay",
                      uint64_t spanUs = 0, uint64_t originSize = 0);

  // The read footprint of one file, shared by every handle on it for as long
  // as this process lives. Both routes need that lifetime: a relayed file has
  // no FileEntry to hold one, and a cached file's entry is destroyed when its
  // last handle closes, so an application that opens each file twice -- which
  // ROOT does -- recorded two half-footprints and kept only the larger.
  //
  // Keyed the way records are keyed, not by the raw url: the same file asked
  // for with different capitalisation, an explicit default port or a per-job
  // token in the query string is one file, and splitting it produced exactly
  // the half-footprints this exists to prevent.
  //
  // Returns nullptr once the table is full. A signature that stops growing
  // would be a confident wrong answer, so the file records none at all --
  // "unknown", which every reader already treats as no evidence. The entry
  // enforces that by poisoning its private footprint when it is handed a null
  // (see FileEntry::useSharedFootprint); for a while this header promised the
  // behaviour and the entry quietly filled its own footprint instead.
  //
  // The cap is a bound on memory, and not a generous one: at a bucket per
  // MiB, a table of 2 GiB files runs to tens of megabytes of bitmaps. It is
  // sized to stop unbounded growth, not because the memory is negligible.
  ReadFootprint* footprintFor(const std::string& url);
  ReadFootprint& relayFootprint(const std::string& url); // legacy name, relay path

  // The CLI opens a store only to read/mutate; suppress the destructor's stats
  // dump so `ucache` invocations don't litter stats/ with zero-counter lines.
  // Also drops the stats file the constructor reserved, while it is still
  // empty — reserving the NAME is what keeps two loaded copies of this library
  // apart, but a CLI call that will never write must not leave one behind.
  void disableStatsDump();

 private:
  // Sidecar-adjacent artifacts observed in the shard listing: removal
  // paths unlink only what the scan saw instead of
  // paying a blind ENOENT round trip per suffix per entry. An artifact
  // published AFTER the scan snapshot is missed here and reaped by the
  // age-guarded sweepReplicaOrphans instead.
  enum : uint8_t {
    kArtTdata = 1u << 0,
    kArtTmeta = 1u << 1,
    kArtTok = 1u << 2,
    kArtVal = 1u << 3,
    kArtCost = 1u << 4,
  };
  struct MetaScan {
    std::string dataPath, metaPath, hashHex, key;
    uint64_t fileSize = 0; // origin size (listEntries reporting)
    uint64_t atime = 0, cachedBytes = 0;
    uint64_t replicaBytes = 0; // .tdata size (0 = none) — evicted with the entry
    double coverage = 0.0;     // fraction of pages present [0,1]
    uint8_t artifacts = 0;     // kArt* bits present in the shard listing
    bool pinned = false;
    bool complete = false;
  };
  // Full-cache sidecar walk (eviction, usage, listEntries): reads
  // sidecar SUMMARIES (MetaFile::loadSummary — header+bitmap, not the page-crc
  // table) and fans out over shard dirs with up to 16 threads; results are in
  // sorted-shard order (deterministic given a quiescent cache). Requires the
  // IOBackend to be thread-safe (RealIO: stateless syscalls; FaultIO: internal
  // mutex). Threads are joined before return — none escape the call.
  std::vector<MetaScan> scanObjects();
  // One shard directory's entries, appended to `out` (worker body of the scan).
  void scanShard(const std::string& objRoot, const std::string& shard,
                 std::vector<MetaScan>& out);
  // Remove replica artifacts (.tdata/.tmeta/*.tmp) whose v1 .meta is gone —
  // debris of a crash mid-publish or of a v1.0.0 process evicting an entry
  // without knowing about replica files (D1 mixed-version caveat). Age-guarded
  // (1 h) so a concurrent publisher's in-flight files are never swept.
  // Caller must hold the cache-wide eviction LOCK.
  void sweepReplicaOrphans();
  // Called from the page-write path: add persisted bytes to the running total
  // and run a rate-limited eviction check.
  void notePersisted(uint64_t bytes, bool allowEvict = true);
  // Resolves budgetAuto -> concrete maxBytes/minFreeBytes from free disk, then
  // seeds approxUsage_. Called once from the constructor.
  void resolveBudget();

  IOBackend& io_;
  Config cfg_;
  Stats stats_;
  // Work done by this process while the cache was engaged (CpuCounters.h):
  // instruction totals are the fingerprint that says two runs are comparable.
  CpuCounters cpu_;
  std::mutex relayFpMu_;
  std::map<std::string, std::unique_ptr<ReadFootprint>> relayFp_;
  // One bitmap plus a key per distinct file this process touched. A
  // long-lived server process touches an unbounded number of them, so this
  // stops rather than grows for ever.
  //
  // The ceiling is far above any analysis job, and it is NOT free in memory --
  // an earlier note here saying it was negligible was wrong, and the note that
  // replaced it was right only at the one file size it named. The cost scales
  // with FILE SIZE, because the bitmap is one bit per MiB: a 2 GiB file is
  // 256 bytes, a 100 GB file is 12.5 KB, and the bitmap accepts indices up to
  // 16 TiB (2 MB). So a full table is ~100 MB of 2 GiB files but ~2.6 GB of
  // 100 GB ones. A count of entries is the wrong unit for a memory bound; a
  // byte budget would be the right one, and is not what this is.
  static constexpr size_t kMaxFootprints = 200000;
  std::mutex regMu_;
  std::map<std::string, std::weak_ptr<FileEntry>> registry_; // hash -> entry
  // Session summaries of closed entries for the per-entry coverage dump.
  std::mutex sumMu_;
  std::vector<std::string> entrySummaries_;
  std::mutex evictMu_;
  uint64_t lastEvictCheckS_ = 0;
  std::atomic<uint64_t> approxUsage_{0}; // running usage estimate
  // Entries removed by the last real eviction pass. 0 => the budget is over but
  // nothing is evictable (pinned/open set exceeds high-water); the over-budget
  // rate-limit bypass is then disabled so we don't scan-storm on every write.
  std::atomic<int> lastEvictEvicted_{1};
  // See admissionBlocked(). Latched by evictNow() so the hot open path reads one
  // relaxed atomic rather than re-deciding, and so the state survives between
  // the rate-limited eviction checks.
  std::atomic<bool> admissionBlocked_{false};
  std::atomic<bool> blockedWarned_{false}; // WARN once per process, not per entry
  std::string statsPath_;
  bool dumpStatsOnDtor_ = true;
  // Distinct keys opened this process (drives stats.filesOpened);
  // the Layer-2 record sink and the optional Layer-3 tracer. The sink is
  // shared with entries so a record can still land after store teardown.
  std::set<std::string> seenKeys_; // under regMu_
  std::shared_ptr<FileEntry::ObsSink> obsSink_;
  std::unique_ptr<Tracer> tracer_;
};

} // namespace ucache
