#include "CacheStore.h"

#include "Log.h"
#include "Trace.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

namespace ucache {
namespace {
uint64_t nowS() { return static_cast<uint64_t>(::time(nullptr)); }
// Starting point for the stats-file suffix, so multiple CacheStore instances
// (same host+pid+second) get distinct files — else they share one, dumpStats
// appends both, and aggregateStats' last-line-per-file rule silently drops the
// earlier source.
//
// This counter alone is NOT enough, because it is per-IMAGE, not per-process.
// A client can load two copies of this library at once: XrdCl reads
// /etc/xrootd/client.plugins.d, then the passwd home's client.plugins.d, then
// XRD_PLUGINCONFDIR, and loads every library they name. When two of those name
// DIFFERENT paths (a user's own install plus a self-contained one, which is
// exactly what the benchmark kit ships), each copy has its own globals, each
// starts this counter at 0, and both pick the same filename. The copy that
// served nothing then appends its all-zero record last and becomes the one
// every reader believes. So the name is claimed from the filesystem below
// rather than assumed to be ours.
std::atomic<uint64_t> g_statsSeq{0};

std::string jsonEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    if (static_cast<unsigned char>(c) < 0x20) {
      char b[8];
      ::snprintf(b, sizeof b, "\\u%04x", c);
      out += b;
      continue;
    }
    out += c;
  }
  return out;
}
} // namespace

CacheStore::CacheStore(IOBackend& io, Config cfg) : io_(io), cfg_(std::move(cfg)) {
  io_.mkdirs(cfg_.cacheDir + "/objects", 0700);
  io_.mkdirs(cfg_.cacheDir + "/stats", 0700);
  char host[256] = "unknown";
  ::gethostname(host, sizeof host - 1);
  // Claim the name with O_EXCL: the only authority on whether a stats file is
  // already spoken for is the filesystem, since a rival writer can be a
  // separate copy of this library with its own counter (see g_statsSeq). The
  // empty file left behind is harmless — aggregateStats skips a file with no
  // complete line and does not count it — and reserving it closes the window
  // between choosing a name and first writing to it.
  std::string stem;
  bool collided = false;
  for (unsigned tries = 0; tries < 1024; ++tries) {
    std::ostringstream p;
    p << cfg_.cacheDir << "/stats/" << host << "-" << ::getpid() << "-" << nowS() << "-"
      << g_statsSeq.fetch_add(1, std::memory_order_relaxed);
    stem = p.str();
    int fd = io_.open(stem + ".jsonl", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
      io_.close(fd);
      break;
    }
    if (errno != EEXIST)
      break; // not a name clash (read-only dir, ENOSPC): keep this name and let
             // the ordinary write path report the real error
    collided = true;
  }
  if (collided)
    UCACHE_WARN("another uCache instance is already writing stats for this "
                "process — two copies of the plugin are loaded (check for a "
                "second conf naming a different library in "
                "/etc/xrootd/client.plugins.d, ~/.xrootd/client.plugins.d, or "
                "XRD_PLUGINCONFDIR); each keeps its own counters");
  statsPath_ = stem + ".jsonl";
  // Stats companions share the stem: <stem>.files.jsonl (per-entry
  // records) and <stem>.trace.jsonl (sampled IO trace, opt-in).
  obsSink_ = std::make_shared<FileEntry::ObsSink>(io_, stem + ".files.jsonl");
  if (cfg_.trace == "io") {
    tracer_ = std::make_unique<Tracer>(io_, stem + ".trace.jsonl", cfg_.traceSample);
    stats_.tracer = tracer_.get(); // set before any entry/thread exists
  }
  resolveBudget();
}

uint64_t CacheStore::effectiveMinFree(const Config& cfg, IOBackend& io) {
  if (cfg.minFreeBytes)
    return cfg.minFreeBytes; // explicit, or already resolved
  if (!cfg.budgetAuto)
    return 0; // eviction off, or a byte cap governs instead
  uint64_t avail = 0, total = 0;
  if (io.spaceInfo(cfg.cacheDir, avail, total) == 0 && avail > 0 && total > 0)
    return std::min<uint64_t>(std::min<uint64_t>(50ull << 30, total / 10), avail / 2);
  return 10ull << 30; // statvfs unavailable: the same fixed floor eviction uses
}

void CacheStore::resolveBudget() {
  // Default policy: use the disk, evict at a free-space floor — no
  // fixed byte cap unless the user set UCACHE_MAX_BYTES. A heavy active session
  // fills most of the disk and reclaims LRU under pressure. budgetAuto is set by
  // fromEnv only when UCACHE_MAX_BYTES is unset; programmatic Config keeps full
  // manual control.
  if (cfg_.budgetAuto && cfg_.minFreeBytes == 0) {
    // Leave min(50 GiB, 10% of total) free: a fixed cap so big disks stay
    // mostly usable, a proportion so small disks keep sane headroom. Clamped
    // to <= half of free-at-init so a shared/near-full FS doesn't thrash
    // (evicting our own contribution can always get back above the floor).
    uint64_t avail = 0, total = 0;
    const bool haveSpace = io_.spaceInfo(cfg_.cacheDir, avail, total) == 0 && avail > 0 && total > 0;
    cfg_.minFreeBytes = effectiveMinFree(cfg_, io_);
    if (!haveSpace)
      UCACHE_WARN("statvfs unavailable at init; using fixed %llu-byte free-disk floor",
                  static_cast<unsigned long long>(cfg_.minFreeBytes));
    UCACHE_INFO("auto eviction: free-disk floor minFreeBytes=%llu (no byte cap)",
                static_cast<unsigned long long>(cfg_.minFreeBytes));
  }
  // Seed the running usage total only when a hard byte cap is set (opt-in). The
  // default floor path uses statvfs and needs no per-startup scan.
  if (cfg_.maxBytes > 0)
    approxUsage_.store(usageBytes(), std::memory_order_relaxed);
}

void CacheStore::disableStatsDump() {
  dumpStatsOnDtor_ = false;
  // Only if still untouched: a caller that disables dumping after something was
  // already written would otherwise destroy a real record.
  struct ::stat st {};
  if (io_.stat(statsPath_, &st) == 0 && st.st_size == 0)
    io_.unlink(statsPath_);
}

CacheStore::~CacheStore() {
  if (dumpStatsOnDtor_)
    dumpStats(/*finalDump=*/true);
}

std::shared_ptr<FileEntry> CacheStore::open(const UrlKey& key, uint64_t originSize,
                                            uint64_t originMtime, uint8_t cksumKind,
                                            uint32_t originCksum, bool* declinedForSpace) {
  if (declinedForSpace)
    *declinedForSpace = false;
  {
    std::lock_guard<std::mutex> g(regMu_);
    auto it = registry_.find(key.hashHex);
    if (it != registry_.end()) {
      if (auto live = it->second.lock()) {
        live->obs().opens.fetch_add(1, std::memory_order_relaxed);
        return live; // one entry per key per process
      }
      registry_.erase(it);
    }
  }
  // Decide on CURRENT state, not on what the last check happened to leave behind.
  // The latch lives in this process and starts clear, and open() otherwise runs
  // its eviction check only AFTER admitting — so without this a fresh process
  // would admit one entry before protection engaged, and a workload of one file
  // per process would never engage it at all. maybeEvict() is rate-limited, so
  // the amortised cost of asking here is a compare against a timestamp.
  {
    struct ::stat probe{};
    if (io_.stat(key.metaPath(cfg_.cacheDir), &probe) != 0)
      maybeEvict(); // a would-be NEW entry: refresh the verdict before deciding
  }
  // Growth has stopped: every resident entry was read too recently to evict.
  // Decline entries we do not already hold, and hold that line PER ENTRY. Doing
  // it per PAGE instead would converge on every file partially cached, which for
  // a columnar read still needs an origin round trip per file and so keeps the
  // bytes while giving up most of the benefit. An entry already on disk is always
  // admitted: it is not growth, and refusing it would leave a partial entry.
  struct ::stat mst{};
  if (admissionBlocked_.load(std::memory_order_relaxed) &&
      io_.stat(key.metaPath(cfg_.cacheDir), &mst) != 0) {
    if (declinedForSpace)
      *declinedForSpace = true;
    stats_.admissionsBypassed.fetch_add(1, std::memory_order_relaxed);
    if (!blockedWarned_.exchange(true, std::memory_order_relaxed))
      UCACHE_WARN("cache is full of entries read within the last %us, so new files are "
                  "no longer being cached (reads still work, uncached). Free space with "
                  "`ucache evict --older-than <dur>`, lower `evict_protect_seconds`, or "
                  "use a bigger cache disk",
                  cfg_.evictProtectSeconds);
    return nullptr; // NOT a fail-open: the caller must not count it as one
  }

  auto entry = FileEntry::open(io_, cfg_, stats_, key, originSize, originMtime, cksumKind,
                               originCksum,
                               [this](uint64_t b, bool ev) { notePersisted(b, ev); });
  if (!entry)
    return nullptr;
  {
    std::lock_guard<std::mutex> g(regMu_);
    // Racing opens of the same key: keep the first registered one.
    auto it = registry_.find(key.hashHex);
    if (it != registry_.end()) {
      if (auto live = it->second.lock()) {
        live->obs().opens.fetch_add(1, std::memory_order_relaxed);
        return live;
      }
      it->second = entry;
    } else {
      registry_.emplace(key.hashHex, entry);
    }
    // Distinct-key count; CLI invocations (disableStatsDump) write
    // neither the counters line nor Layer-2 records — don't litter for them.
    if (dumpStatsOnDtor_) {
      if (seenKeys_.insert(key.hashHex).second)
        stats_.filesOpened.fetch_add(1, std::memory_order_relaxed);
      entry->setObsSink(obsSink_);
    }
  }
  entry->obs().opens.fetch_add(1, std::memory_order_relaxed);
  maybeEvict();
  return entry;
}

void CacheStore::notePersisted(uint64_t bytes, bool allowEvict) {
  if (cfg_.maxBytes == 0 && cfg_.minFreeBytes == 0)
    return; // eviction fully disabled
  if (cfg_.maxBytes > 0)
    approxUsage_.fetch_add(bytes, std::memory_order_relaxed);
  if (allowEvict)
    maybeEvict(); // also drives the disk-floor check when byte budget is off
}

void CacheStore::maybeEvict() {
  if (cfg_.maxBytes == 0 && cfg_.minFreeBytes == 0)
    return;
  // Byte-budget trigger: cheap per-process running estimate (best-effort under
  // concurrency), computed BEFORE the rate limit. When it already says we are
  // over the cap, evict WITHOUT waiting for the time tick — otherwise a fast
  // writer overshoots the cap unboundedly between ticks (soak-caught). After an
  // eviction reconciles the total down to low-water, `over` clears until it
  // climbs back, so this fires ~once per (high-low) band of writes, not per write.
  bool over = cfg_.maxBytes > 0 && approxUsage_.load(std::memory_order_relaxed) >
                                       static_cast<uint64_t>(cfg_.highWater * cfg_.maxBytes);
  // Bypass the time rate-limit only when over AND the last pass actually evicted
  // something. If the last pass evicted nothing while over (unreducible: the
  // pinned/open set alone exceeds high-water), fall back to the rate limit so we
  // don't run a full scan+flush on every page-write for no benefit.
  bool bypass = over && lastEvictEvicted_.load(std::memory_order_relaxed) != 0;
  {
    std::lock_guard<std::mutex> g(evictMu_);
    uint64_t now = nowS();
    if (!bypass && cfg_.evictCheckSeconds > 0 &&
        now < lastEvictCheckS_ + static_cast<uint64_t>(cfg_.evictCheckSeconds)) // rate limit
      return; // routine floor / not-yet-over / stuck-over check
    lastEvictCheckS_ = now;
  }
  // Disk-floor trigger: accurate cross-process guard via statvfs, active even
  // when the byte budget is off.
  bool lowDisk = false;
  if (cfg_.minFreeBytes > 0) {
    uint64_t avail = 0, total = 0;
    if (io_.spaceInfo(cfg_.cacheDir, avail, total) == 0 && avail < cfg_.minFreeBytes)
      lowDisk = true;
  }
  if (over || lowDisk)
    evictNow();
}

void CacheStore::scanShard(const std::string& objRoot, const std::string& shard,
                           std::vector<MetaScan>& out) {
  std::vector<std::string> files;
  if (io_.listDir(objRoot + "/" + shard, files) < 0)
    return;
  // Artifact presence from the listing we already have — a stat/unlink per
  // entry just to learn "not there" costs a filesystem round trip each.
  std::set<std::string> names(files.begin(), files.end());
  for (const auto& f : files) {
    if (f.size() < 5 || f.compare(f.size() - 5, 5, ".meta") != 0)
      continue;
    std::string metaPath = objRoot + "/" + shard + "/" + f;
    auto m = MetaFile::loadSummary(io_, metaPath);
    if (!m)
      continue; // torn/corrupt: ignored here; rebuilt on next open
    MetaScan s;
    s.metaPath = std::move(metaPath);
    s.dataPath = objRoot + "/" + shard + "/" + f.substr(0, f.size() - 5) + ".data";
    s.hashHex = f.substr(0, f.size() - 5); // "<hash>.meta" -> "<hash>" = registry key
    s.key = m->key;                        // stored URL key (for CLI reporting)
    s.fileSize = m->fileSize;
    s.atime = m->atime;
    s.cachedBytes = m->cachedBytes();
    s.pinned = m->flags & MetaData::kFlagPinned;
    s.complete = m->flags & MetaData::kFlagComplete;
    uint64_t np = m->npages();
    s.coverage =
        np ? static_cast<double>(m->bitmap.count()) / static_cast<double>(np) : 0.0;
    const std::string stem = f.substr(0, f.size() - 5);
    if (names.count(stem + ".tdata"))
      s.artifacts |= kArtTdata;
    if (names.count(stem + ".tmeta"))
      s.artifacts |= kArtTmeta;
    if (names.count(stem + ".tok"))
      s.artifacts |= kArtTok;
    if (names.count(stem + ".val"))
      s.artifacts |= kArtVal;
    if (names.count(stem + ".cost"))
      s.artifacts |= kArtCost;
    struct ::stat tst; // replica overlay counts toward usage
    if ((s.artifacts & kArtTdata) &&
        io_.stat(objRoot + "/" + shard + "/" + stem + ".tdata", &tst) == 0)
      s.replicaBytes = static_cast<uint64_t>(tst.st_size);
    out.push_back(std::move(s));
  }
}

std::vector<CacheStore::MetaScan> CacheStore::scanObjects() {
  std::vector<MetaScan> out;
  const std::string objRoot = cfg_.cacheDir + "/objects";
  std::vector<std::string> shards;
  if (io_.listDir(objRoot, shards) < 0)
    return out;
  std::sort(shards.begin(), shards.end()); // deterministic result order
  std::vector<std::vector<MetaScan>> perShard(shards.size());
  std::atomic<size_t> next{0};
  auto worker = [&] {
    for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < shards.size();)
      scanShard(objRoot, shards[i], perShard[i]);
  };
  size_t hw = std::thread::hardware_concurrency();
  size_t nThreads = std::min({size_t(16), shards.size(), hw ? hw : size_t(1)});
  if (nThreads > 1) {
    std::vector<std::thread> pool;
    pool.reserve(nThreads - 1);
    for (size_t t = 1; t < nThreads; ++t)
      pool.emplace_back(worker);
    worker();
    for (auto& t : pool)
      t.join();
  } else {
    worker();
  }
  size_t total = 0;
  for (const auto& v : perShard)
    total += v.size();
  out.reserve(total);
  for (auto& v : perShard)
    for (auto& s : v)
      out.push_back(std::move(s));
  return out;
}

uint64_t CacheStore::usageBytes() {
  uint64_t total = 0;
  for (const auto& s : scanObjects())
    total += s.cachedBytes + s.replicaBytes;
  return total;
}

std::vector<CacheStore::EntryInfo> CacheStore::listEntries() {
  std::vector<EntryInfo> out;
  auto scans = scanObjects(); // parallel summary walk
  out.reserve(scans.size());
  for (auto& s : scans) {
    EntryInfo e;
    e.key = std::move(s.key);
    e.fileSize = s.fileSize;
    e.cachedBytes = s.cachedBytes;
    e.replicaBytes = s.replicaBytes;
    e.atime = s.atime;
    e.coverage = s.coverage;
    e.pinned = s.pinned;
    e.complete = s.complete;
    out.push_back(std::move(e));
  }
  return out;
}

int CacheStore::evictNow() {
  const std::string lockPath = cfg_.cacheDir + "/LOCK";
  int lockFd = io_.open(lockPath, O_RDWR | O_CREAT, 0600);
  if (lockFd < 0)
    return -1;
  if (io_.flock(lockFd, LOCK_EX | LOCK_NB) < 0) {
    io_.close(lockFd); // another process is evicting
    return -1;
  }

  // Flush live entries' sidecars BEFORE scanning so the scan reflects in-flight
  // writes (writePages flushes lazily, up to metaFlushSeconds late), and record
  // which entries are OPEN so we never unlink one being actively read/filled.
  // Copy the shared_ptrs out under regMu_ and flush WITHOUT the lock (setPinned/
  // flushMeta do blocking flock+fsync — holding regMu_ across it stalls readers).
  std::set<std::string> liveHashes;
  {
    std::vector<std::shared_ptr<FileEntry>> live;
    {
      std::lock_guard<std::mutex> g(regMu_);
      // Also PRUNE expired weak_ptrs: registry_ otherwise grows without bound
      // for a process that opens many distinct keys (nothing else erases them),
      // and this per-eviction walk would then get progressively slower until
      // eviction can no longer keep up (found by the 30-min soak).
      for (auto it = registry_.begin(); it != registry_.end();) {
        if (auto e = it->second.lock()) {
          liveHashes.insert(it->first);
          live.push_back(std::move(e));
          ++it;
        } else {
          it = registry_.erase(it);
        }
      }
    }
    for (auto& e : live)
      e->flushAll();
  }

  auto scans = scanObjects();
  uint64_t usage = 0;
  for (const auto& s : scans)
    usage += s.cachedBytes + s.replicaBytes;

  // Byte-budget target (0 disables the byte trigger — e.g. a disk-floor-only
  // manual evict). Disk-floor target: read free space once and track it as we
  // unlink (a sparse .data's blocks ~= its cached bytes); resume a little above
  // the floor to avoid thrash.
  const uint64_t byteTarget =
      cfg_.maxBytes ? static_cast<uint64_t>(cfg_.lowWater * cfg_.maxBytes) : 0;
  uint64_t avail = 0, total = 0;
  bool haveSpace = cfg_.minFreeBytes > 0 && io_.spaceInfo(cfg_.cacheDir, avail, total) == 0;
  const uint64_t resumeFree = cfg_.minFreeBytes + cfg_.minFreeBytes / 10;

  auto needEvict = [&] {
    if (cfg_.maxBytes && usage > byteTarget)
      return true;
    if (haveSpace && avail < resumeFree)
      return true;
    return false;
  };

  // Entries read within this window are not candidates, so a running analysis
  // cannot evict its own working set. The cutoff is computed ONCE per pass: a
  // per-entry `now` would let entries read during the pass drift across the
  // boundary and make the decision depend on how long the scan took.
  const uint64_t passNow = nowS(); // file-local helper; one reading for the pass
  const uint64_t protectCutoff =
      cfg_.evictProtectSeconds && passNow > cfg_.evictProtectSeconds
          ? passNow - cfg_.evictProtectSeconds
          : 0; // 0 => protect nothing (window off, or the clock is absurdly early)
  bool sawProtected = false;

  int evicted = 0;
  if (needEvict()) {
    std::sort(scans.begin(), scans.end(),
              [](const MetaScan& a, const MetaScan& b) { return a.atime < b.atime; });
    for (const auto& s : scans) {
      if (!needEvict())
        break;
      if (s.pinned || liveHashes.count(s.hashHex))
        continue; // never evict pinned or currently-open entries
      if (protectCutoff && s.atime >= protectCutoff) {
        // Sorted oldest-first, so everything after this is protected too — but
        // keep scanning rather than breaking: a later entry may be unprotected if
        // atime resolution ties, and the loop is over an in-memory vector.
        sawProtected = true;
        continue;
      }
      // Honor a pin that landed after the scan snapshot (a CLI `pin` racing us).
      if (auto m = MetaFile::load(io_, s.metaPath); m && (m->flags & MetaData::kFlagPinned))
        continue;
      // Data first, then sidecar (a crash between is self-healing). Only credit
      // space actually reclaimed — a failed unlink must not fool the disk floor
      // into stopping early or inflate the usage/stats accounting.
      if (io_.unlink(s.dataPath) != 0)
        continue;
      io_.unlink(s.metaPath);
      // Replica files are evicted with their entry (same LRU unit).
      // Only artifacts the scan saw — no blind ENOENT unlinks.
      const std::string base = s.dataPath.substr(0, s.dataPath.size() - 5);
      if (s.artifacts & kArtTdata)
        io_.unlink(base + ".tdata");
      if (s.artifacts & kArtTmeta)
        io_.unlink(base + ".tmeta");
      if (s.artifacts & kArtTok)
        io_.unlink(base + ".tok"); // replica verify-once marker
      if (s.artifacts & kArtVal)
        io_.unlink(base + ".val"); // cache-freshness marker (UCACHE_REVALIDATE_S)
      if (s.artifacts & kArtCost)
        io_.unlink(base + ".cost"); // CPU-span evidence
      const uint64_t reclaimed = s.cachedBytes + s.replicaBytes;
      usage -= std::min(usage, reclaimed);
      avail += reclaimed; // reclaimed disk (approx; ignored when !haveSpace)
      ++evicted;
      stats_.evictedEntries.fetch_add(1, std::memory_order_relaxed);
      stats_.evictedBytes.fetch_add(reclaimed, std::memory_order_relaxed);
    }
    UCACHE_INFO("eviction: removed %d entries, usage now %llu", evicted,
                static_cast<unsigned long long>(usage));
  }
  sweepReplicaOrphans();
  // Growth stops when the budget is still exceeded AND the only thing standing in
  // the way is the protection window. Deliberately NOT latched when the blocker is
  // the pinned/open set: that case predates this window, is handled by the
  // over-budget rate-limit bypass, and blaming it on the window would make both
  // the WARN and the doctor finding name the wrong cause. Cleared as soon as a
  // victim becomes eligible, so a pass that frees something re-opens admission.
  admissionBlocked_.store(needEvict() && sawProtected, std::memory_order_relaxed);
  if (needEvict() && sawProtected)
    UCACHE_INFO("eviction: over budget with every candidate inside the %us protection "
                "window — new entries will not be admitted until one ages out",
                cfg_.evictProtectSeconds);
  // Reconcile the per-process running estimate to the authoritative scan total,
  // and record whether this pass made progress (drives the over-budget bypass).
  approxUsage_.store(usage, std::memory_order_relaxed);
  lastEvictEvicted_.store(evicted, std::memory_order_relaxed);
  io_.flock(lockFd, LOCK_UN);
  io_.close(lockFd);
  return evicted;
}

void CacheStore::invalidate(const UrlKey& key) {
  {
    std::lock_guard<std::mutex> g(regMu_);
    registry_.erase(key.hashHex);
  }
  io_.unlink(key.dataPath(cfg_.cacheDir));
  io_.unlink(key.metaPath(cfg_.cacheDir));
  // A replica derives from the invalidated bytes — it goes with them.
  const std::string base = key.objectDir(cfg_.cacheDir) + "/" + key.hashHex;
  io_.unlink(base + ".tdata");
  io_.unlink(base + ".tmeta");
  io_.unlink(base + ".tok");
  io_.unlink(base + ".val");
  UCACHE_INFO("invalidated cache entry for %s", key.key.c_str());
}

bool CacheStore::removeEntry(const UrlKey& key) {
  const std::string base = key.objectDir(cfg_.cacheDir) + "/" + key.hashHex;
  struct ::stat st;
  // "Cached" includes a replica-only orphan (.tdata with no .meta/.data): a
  // bare `rm` should still report it removed rather than "not cached".
  bool existed = io_.stat(key.metaPath(cfg_.cacheDir), &st) == 0 ||
                 io_.stat(key.dataPath(cfg_.cacheDir), &st) == 0 ||
                 io_.stat(base + ".tdata", &st) == 0;
  invalidate(key); // detaches the registry + unlinks byte cache and replica
  if (existed)
    approxUsage_.store(usageBytes(), std::memory_order_relaxed);
  return existed;
}

CacheStore::CleanupReport CacheStore::cleanup(CleanupMode mode, uint64_t arg, bool keepPinned,
                                              bool dryRun) {
  CleanupReport rep;
  const std::string lockPath = cfg_.cacheDir + "/LOCK";
  int lockFd = io_.open(lockPath, O_RDWR | O_CREAT, 0600);
  if (lockFd < 0)
    return rep; // locked stays false
  if (io_.flock(lockFd, LOCK_EX | LOCK_NB) < 0) {
    io_.close(lockFd); // another process is evicting
    return rep;
  }
  rep.locked = true;

  // Flush live sidecars before scanning so the scan reflects in-flight writes,
  // and record which entries are OPEN so we never unlink one being actively
  // read/filled (same discipline as evictNow; both are no-ops in the
  // short-lived CLI process whose registry is empty). Copy shared_ptrs out and
  // flush WITHOUT regMu_ (flushMeta blocks on flock+fsync).
  std::set<std::string> liveHashes;
  {
    std::vector<std::shared_ptr<FileEntry>> live;
    {
      std::lock_guard<std::mutex> g(regMu_);
      for (auto it = registry_.begin(); it != registry_.end();) {
        if (auto e = it->second.lock()) {
          liveHashes.insert(it->first);
          live.push_back(std::move(e));
          ++it;
        } else {
          it = registry_.erase(it);
        }
      }
    }
    for (auto& e : live)
      e->flushAll();
  }

  auto scans = scanObjects();

  // An entry is protected from this pass if it is open in-process, or (when
  // keepPinned) pinned — including a pin that landed AFTER the scan snapshot,
  // re-checked from the sidecar exactly as evictNow does (setPinnedByKey uses a
  // per-entry flock, not the cache LOCK, so a CLI `pin` can race us).
  auto protectedEntry = [&](const MetaScan& s) {
    if (liveHashes.count(s.hashHex))
      return true;
    if (keepPinned && s.pinned)
      return true;
    if (keepPinned) {
      if (auto m = MetaFile::load(io_, s.metaPath); m && (m->flags & MetaData::kFlagPinned))
        return true;
    }
    return false;
  };

  // Select victims per mode.
  std::vector<const MetaScan*> victims;
  if (mode == CleanupMode::kToSize) {
    // LRU (oldest first): drop until the total cache size is at or below `arg`.
    std::sort(scans.begin(), scans.end(),
              [](const MetaScan& a, const MetaScan& b) { return a.atime < b.atime; });
    uint64_t total = 0;
    for (const auto& s : scans)
      total += s.cachedBytes + s.replicaBytes;
    for (const auto& s : scans) {
      if (total <= arg)
        break;
      if (protectedEntry(s))
        continue; // stays cached -> stays in `total`, so the target still holds
      victims.push_back(&s);
      total -= std::min(total, s.cachedBytes + s.replicaBytes);
    }
  } else {
    const uint64_t cutoff = nowS() > arg ? nowS() - arg : 0;
    for (const auto& s : scans) {
      if (protectedEntry(s))
        continue;
      if (mode == CleanupMode::kOlderThan && s.atime >= cutoff)
        continue; // used within the window — not stale
      if (mode == CleanupMode::kNewerThan && s.atime < cutoff)
        continue; // NOT used within the window — not the recent pollution
      victims.push_back(&s);
    }
  }

  // Removal fan-out: each victim's unlinks are independent,
  // and on a latency-bound filesystem the serial loop IS the wall time (field
  // measurement: 57k-entry `clear` = 13m37s at 2% CPU). Workers take victims
  // by atomic index; per-victim outcomes land in a pre-sized array so the
  // report keeps the victims' (LRU/scan) order deterministically. Only
  // artifacts the scan actually saw are unlinked — no blind ENOENT round
  // trips (a post-scan publish is reaped by sweepReplicaOrphans).
  std::vector<uint8_t> removed(victims.size(), dryRun ? 1 : 0);
  if (!dryRun) {
    std::atomic<size_t> next{0};
    auto worker = [&] {
      size_t i;
      while ((i = next.fetch_add(1, std::memory_order_relaxed)) < victims.size()) {
        const MetaScan* s = victims[i];
        {
          std::lock_guard<std::mutex> g(regMu_);
          registry_.erase(s->hashHex);
        }
        // Data first (a crash between unlinks is self-healing); credit only
        // what was actually removed — a failed data unlink leaves the entry.
        if (io_.unlink(s->dataPath) != 0)
          continue;
        io_.unlink(s->metaPath);
        const std::string base = s->dataPath.substr(0, s->dataPath.size() - 5);
        if (s->artifacts & kArtTdata)
          io_.unlink(base + ".tdata");
        if (s->artifacts & kArtTmeta)
          io_.unlink(base + ".tmeta");
        if (s->artifacts & kArtTok)
          io_.unlink(base + ".tok");
        if (s->artifacts & kArtVal)
          io_.unlink(base + ".val");
        if (s->artifacts & kArtCost)
          io_.unlink(base + ".cost");
        stats_.evictedEntries.fetch_add(1, std::memory_order_relaxed);
        stats_.evictedBytes.fetch_add(s->cachedBytes + s->replicaBytes,
                                      std::memory_order_relaxed);
        removed[i] = 1;
      }
    };
    size_t hw = std::thread::hardware_concurrency();
    size_t nThreads = std::min({size_t(16), victims.size(), hw ? hw : size_t(1)});
    if (nThreads > 1) {
      std::vector<std::thread> pool;
      pool.reserve(nThreads - 1);
      for (size_t t = 1; t < nThreads; ++t)
        pool.emplace_back(worker);
      worker();
      for (auto& t : pool)
        t.join();
    } else {
      worker();
    }
  }
  for (size_t i = 0; i < victims.size(); ++i) {
    if (!removed[i])
      continue;
    const MetaScan* s = victims[i];
    const uint64_t bytes = s->cachedBytes + s->replicaBytes;
    rep.victims.push_back({s->key, bytes, s->atime});
    rep.bytes += bytes;
  }
  if (!dryRun) {
    sweepReplicaOrphans();
    approxUsage_.store(usageBytes(), std::memory_order_relaxed);
  }
  io_.flock(lockFd, LOCK_UN);
  io_.close(lockFd);
  return rep;
}

void CacheStore::sweepReplicaOrphans() {
  const std::string objRoot = cfg_.cacheDir + "/objects";
  const uint64_t now = nowS();
  std::vector<std::string> shards;
  if (io_.listDir(objRoot, shards) < 0)
    return;
  for (const auto& sh : shards) {
    std::vector<std::string> files;
    if (io_.listDir(objRoot + "/" + sh, files) < 0)
      continue;
    std::set<std::string> metas; // hashes with a live v1 sidecar
    for (const auto& f : files)
      if (f.size() > 5 && f.compare(f.size() - 5, 5, ".meta") == 0)
        metas.insert(f.substr(0, f.size() - 5));
    for (const auto& f : files) {
      bool replicaArtifact = false;
      std::string hash;
      for (const char* suf : {".tdata", ".tmeta", ".tok", ".val", ".cost", ".tdata.tmp", ".tmeta.tmp"}) {
        size_t sl = ::strlen(suf);
        if (f.size() > sl && f.compare(f.size() - sl, sl, suf) == 0) {
          replicaArtifact = true;
          hash = f.substr(0, f.size() - sl);
          break;
        }
      }
      if (!replicaArtifact || metas.count(hash))
        continue;
      // Age guard: never sweep a concurrent publisher's in-flight files.
      const std::string path = objRoot + "/" + sh + "/" + f;
      struct ::stat st;
      if (io_.stat(path, &st) != 0 || now < static_cast<uint64_t>(st.st_mtime) + 3600)
        continue;
      if (io_.unlink(path) == 0) {
        stats_.replicaOrphansSwept.fetch_add(1, std::memory_order_relaxed);
        UCACHE_INFO("swept orphaned replica artifact %s", path.c_str());
      }
    }
  }
}

bool CacheStore::setPinnedByKey(const UrlKey& key, bool pinned) {
  // Live entry: copy the shared_ptr out under regMu_, then mutate WITHOUT the
  // lock — setPinned()->flushMeta() does a blocking flock + fsync, and holding
  // regMu_ across it would stall every reader's open(). Matches the
  // open()/invalidate() discipline of never doing IO under regMu_.
  std::shared_ptr<FileEntry> live;
  {
    std::lock_guard<std::mutex> g(regMu_);
    auto it = registry_.find(key.hashHex);
    if (it != registry_.end())
      live = it->second.lock();
  }
  if (live) {
    live->setPinned(pinned);
    return true;
  }
  // Closed entry: rewrite the sidecar flag directly under the entry flock. No
  // O_CREAT (must already be cached) and no origin size => never re-validates,
  // so this cannot truncate/wipe an entry the way open() would on a mismatch.
  const std::string dataPath = key.dataPath(cfg_.cacheDir);
  const std::string metaPath = key.metaPath(cfg_.cacheDir);
  int fd = io_.open(dataPath, O_RDWR, 0600);
  if (fd < 0)
    return false; // not cached
  io_.flock(fd, LOCK_EX);
  bool ok = false;
  // Never resurrect a sidecar whose .data was evicted out from under us between
  // our open and now (mirrors FileEntry::flushMeta's guard) — else we'd leave an
  // orphaned, pinned, permanently-missing entry that can never be reclaimed.
  struct ::stat st;
  if (io_.fstat(fd, &st) == 0 && st.st_nlink > 0) {
    if (auto m = MetaFile::load(io_, metaPath)) {
      if (pinned)
        m->flags |= MetaData::kFlagPinned;
      else
        m->flags &= ~MetaData::kFlagPinned;
      ok = MetaFile::store(io_, metaPath, *m, cfg_.fsync == FsyncMode::kAll) == 0;
    }
  }
  io_.flock(fd, LOCK_UN);
  io_.close(fd);
  return ok;
}

FileEntry::ScrubResult CacheStore::verify(const UrlKey& key, uint64_t originSize,
                                          uint64_t originMtime, uint8_t cksumKind,
                                          uint32_t originCksum) {
  // Re-open with the entry's own metadata so validation always adopts (never
  // wipes the entry). verifyAll then scrubs the real pages.
  auto e = open(key, originSize, originMtime, cksumKind, originCksum);
  if (!e)
    return {};
  return e->verifyAll();
}

void CacheStore::recordRelayObs(const std::string& url, uint64_t bytes) {
  if (!obsSink_ || bytes == 0)
    return;
  // Normalized exactly like a cached entry's key, or the baseline record could
  // never match the warm runs it exists to be compared against.
  auto key = UrlKey::parse(url, cfg_.keepCgi);
  std::ostringstream os;
  os << "{\"ts\":" << nowS() << ",\"key\":\"" << jsonEscape(key ? key->key : url) << '\"'
     << ",\"opens\":1,\"served_bytes\":0,\"ram_bytes\":0,\"replica_bytes\":0"
     << ",\"disk_reads\":0,\"disk_seq\":0,\"disk_bytes\":0,\"first_touch_bytes\":0"
     << ",\"wire_bytes\":" << bytes << "}\n";
  obsSink_->append(os.str());
}

void CacheStore::dumpStats(bool finalDump) {
  if (finalDump) {
    std::lock_guard<std::mutex> g(regMu_);
    for (auto& [hash, weak] : registry_)
      if (auto e = weak.lock())
        e->emitObsRecord();
  }
  std::ostringstream line;
  line << "{\"ts\":" << nowS() << ",\"pid\":" << ::getpid();
  if (cfg_.disable)
    line << ",\"disabled\":1"; // a BASELINE run: the cache was out of the loop
  line << ',' << stats_.toJsonBody()
       << ",\"entries\":[";
  {
    std::lock_guard<std::mutex> g(regMu_);
    bool first = true;
    for (auto& [hash, weak] : registry_) {
      auto e = weak.lock();
      if (!e)
        continue;
      if (!first)
        line << ',';
      first = false;
      line << "{\"key\":\"" << jsonEscape(e->key().key) << "\",\"file_size\":" << e->fileSize()
           << ",\"cached_bytes\":" << e->cachedBytes() << ",\"page_size\":" << e->pageSize()
           << '}';
    }
  }
  line << "]}\n";
  int fd = io_.open(statsPath_, O_WRONLY | O_CREAT, 0600);
  if (fd < 0)
    return; // stats loss is acceptable; job health is not (fail-open)
  struct ::stat st;
  uint64_t off = io_.fstat(fd, &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
  const std::string s = line.str();
  io_.pwriteFull(fd, s.data(), s.size(), off); // append (single writer per path)
  io_.close(fd);
}

} // namespace ucache
