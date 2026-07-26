#include "CacheStore.h"
#include "testing/FaultIO.h"

#include "TestUtil.h"
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <sys/file.h>
#include <thread>

using namespace ucache;
using test::TempDir;

namespace {
UrlKey keyN(int n) {
  return *UrlKey::parse("root://h//data/file" + std::to_string(n) + ".root");
}
// Force an entry's on-disk atime (entries created in the same second otherwise
// share time()); used to make LRU order deterministic.
void setAtime(IOBackend& io, const Config& cfg, const UrlKey& k, uint64_t atime) {
  auto m = MetaFile::load(io, k.metaPath(cfg.cacheDir));
  ASSERT_TRUE(m);
  m->atime = atime;
  ASSERT_EQ(MetaFile::store(io, k.metaPath(cfg.cacheDir), *m, false), 0);
}
} // namespace

TEST(CacheStore, RegistrySharesOneEntryPerKey) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  CacheStore store(io, cfg);
  auto a = store.open(keyN(1), 100000);
  auto b = store.open(keyN(1), 100000);
  ASSERT_TRUE(a);
  EXPECT_EQ(a.get(), b.get()); // same object shared
  auto c = store.open(keyN(2), 100000);
  EXPECT_NE(a.get(), c.get());
  // After all refs drop, a new open builds a new object (adopting the meta).
  FileEntry* raw = a.get();
  a.reset();
  b.reset();
  auto d = store.open(keyN(1), 100000);
  EXPECT_TRUE(d);
  (void)raw;
}

TEST(CacheStore, OpenFailureFailsOpen) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path() + "/subdir-file";
  // Make cacheDir path unusable: a *file* where the dir should be.
  std::ofstream(cfg.cacheDir.c_str()) << "x";
  CacheStore store(io, cfg);
  auto e = store.open(keyN(1), 1000);
  EXPECT_EQ(e, nullptr); // caller degrades to pass-through
}

TEST(CacheStore, UsageAndEviction) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 1000000;
  cfg.highWater = 0.5; // 500 KB
  cfg.lowWater = 0.25; // 250 KB
  CacheStore store(io, cfg);

  auto src = test::randomBytes(200 * 4096, 5); // 800 KB source
  // Three entries, 160 KB cached each; distinct atimes. One entry fits
  // under the 250 KB low-water target, so LRU must remove exactly two.
  for (int n = 0; n < 3; ++n) {
    auto e = store.open(keyN(n), src.size());
    ASSERT_TRUE(e);
    e->writePages(0, 40 * 4096, src.data());
    e->flushMeta(true);
    // Force distinct atimes: rewrite sidecar with a controlled atime.
    // (Entries created in the same second share time(nullptr).)
    auto m = MetaFile::load(io, keyN(n).metaPath(cfg.cacheDir));
    ASSERT_TRUE(m);
    m->atime = 1000 + n; // entry 0 oldest
    ASSERT_EQ(MetaFile::store(io, keyN(n).metaPath(cfg.cacheDir), *m, false), 0);
  }
  uint64_t usage = store.usageBytes();
  EXPECT_EQ(usage, 3u * 40 * 4096);

  int evicted = store.evictNow();
  EXPECT_EQ(evicted, 3 - 1); // down to <= 250 KB leaves one entry
  EXPECT_LE(store.usageBytes(), 250000u);
  EXPECT_EQ(store.stats().evictedEntries.load(), 2u);
  // LRU: oldest (0) and next (1) gone, newest (2) survives.
  struct ::stat st;
  EXPECT_LT(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0);
  EXPECT_LT(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0);
  EXPECT_EQ(io.stat(keyN(2).dataPath(cfg.cacheDir), &st), 0);
}

TEST(CacheStore, EvictNowFailsCleanlyWhenAnotherProcessHoldsLock) {
  // The real deployment is many processes on one cache dir: while one holds
  // <dir>/LOCK, evictNow must report busy (-1) without blocking or evicting —
  // this is what cmdEvict surfaces as "another process holds the eviction lock".
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 100000;
  CacheStore store(io, cfg);
  auto src = test::randomBytes(50 * 4096, 7);
  {
    auto e = store.open(keyN(0), src.size());
    ASSERT_TRUE(e);
    e->writePages(0, 40 * 4096, src.data()); // over budget: evictable once closed
    e->flushMeta(true);
  }
  // Simulate the concurrent evictor: BSD flock conflicts across open-file-
  // descriptions, so a second fd in this process behaves like another process.
  int fd = io.open(cfg.cacheDir + "/LOCK", O_RDWR | O_CREAT, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(io.flock(fd, LOCK_EX), 0);
  EXPECT_EQ(store.evictNow(), -1);
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // nothing removed
  ASSERT_EQ(io.flock(fd, LOCK_UN), 0);
  io.close(fd);
  EXPECT_EQ(store.evictNow(), 1); // lock released: the over-budget entry goes
}

TEST(CacheStore, EvictionSkipsPinned) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 100000;
  cfg.highWater = 0.5;
  cfg.lowWater = 0.1; // target 10 KB: below one entry -> wants to evict all
  CacheStore store(io, cfg);
  auto src = test::randomBytes(20 * 4096, 6);
  for (int n = 0; n < 2; ++n) {
    auto e = store.open(keyN(n), src.size());
    e->writePages(0, 10 * 4096, src.data());
    if (n == 0)
      e->setPinned(true);
    e->flushMeta(true);
  }
  store.evictNow();
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // pinned survives
  EXPECT_LT(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0);
}

// ---- CLI cleanup surface (rm / clear / evict --older-than / --to-size) -----

namespace {
// A store with N entries of `pagesEach` 4 KiB pages, distinct atimes (entry i
// oldest..newest), all closed and flushed — the shape the CLI cleanup path sees.
struct CleanupFx {
  TempDir td;
  RealIO io;
  Config cfg;
  std::unique_ptr<CacheStore> store;
  explicit CleanupFx(int n, int pagesEach, uint64_t baseAtime = 1000) {
    cfg.cacheDir = td.path();
    store = std::make_unique<CacheStore>(io, cfg);
    auto src = test::randomBytes(pagesEach * 4096, 11);
    for (int i = 0; i < n; ++i) {
      auto e = store->open(keyN(i), src.size());
      e->writePages(0, pagesEach * 4096, src.data());
      e->flushMeta(true);
    } // entries closed here
    for (int i = 0; i < n; ++i)
      setAtime(io, cfg, keyN(i), baseAtime + i); // 0 oldest .. n-1 newest
  }
  bool present(int i) {
    struct ::stat st;
    return io.stat(keyN(i).dataPath(cfg.cacheDir), &st) == 0;
  }
};
} // namespace

TEST(CacheStore, RemoveEntryDropsByKeyAndReportsMissing) {
  CleanupFx fx(1, 8);
  EXPECT_TRUE(fx.present(0));
  EXPECT_TRUE(fx.store->removeEntry(keyN(0)));
  EXPECT_FALSE(fx.present(0));
  struct ::stat st;
  EXPECT_LT(fx.io.stat(keyN(0).metaPath(fx.cfg.cacheDir), &st), 0); // sidecar gone too
  EXPECT_FALSE(fx.store->removeEntry(keyN(0)));                     // already gone
}

TEST(CacheStore, RemoveEntryDropsReplicaArtifacts) {
  CleanupFx fx(1, 8);
  // Fake a replica overlay + markers alongside the entry.
  const std::string base = keyN(0).objectDir(fx.cfg.cacheDir) + "/" + keyN(0).hashHex;
  for (const char* suf : {".tdata", ".tmeta", ".tok", ".val"})
    std::ofstream(base + suf) << "x";
  EXPECT_TRUE(fx.store->removeEntry(keyN(0)));
  struct ::stat st;
  for (const char* suf : {".tdata", ".tmeta", ".tok", ".val"})
    EXPECT_LT(fx.io.stat((base + suf).c_str(), &st), 0) << suf << " should be removed";
}

TEST(CacheStore, RemoveEntryHandlesReplicaOnlyOrphan) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  CacheStore store(io, cfg);
  auto key = keyN(0);
  // Only a replica overlay on disk (no .meta/.data) — a sweepable orphan. `rm`
  // must still report it removed (it deleted the .tdata), not "not cached".
  ASSERT_EQ(io.mkdirs(key.objectDir(cfg.cacheDir), 0700), 0);
  const std::string base = key.objectDir(cfg.cacheDir) + "/" + key.hashHex;
  std::ofstream(base + ".tdata") << "x";
  EXPECT_TRUE(store.removeEntry(key));
  struct ::stat st;
  EXPECT_LT(io.stat((base + ".tdata"), &st), 0);
  EXPECT_FALSE(store.removeEntry(key)); // now truly gone
}

TEST(CacheStore, CleanupAllRespectsDryRunAndPins) {
  CleanupFx fx(3, 8);
  fx.store->setPinnedByKey(keyN(1), true);

  // Dry run removes nothing but reports every entry.
  auto dry = fx.store->cleanup(CacheStore::CleanupMode::kAll, 0, /*keepPinned=*/false, true);
  EXPECT_TRUE(dry.locked);
  EXPECT_EQ(dry.victims.size(), 3u);
  EXPECT_GT(dry.bytes, 0u);
  EXPECT_TRUE(fx.present(0) && fx.present(1) && fx.present(2));

  // keepPinned: everything but the pinned entry goes.
  auto keep = fx.store->cleanup(CacheStore::CleanupMode::kAll, 0, /*keepPinned=*/true, false);
  EXPECT_EQ(keep.victims.size(), 2u);
  EXPECT_TRUE(fx.present(1));
  EXPECT_FALSE(fx.present(0));
  EXPECT_FALSE(fx.present(2));

  // Full wipe removes the pinned one too.
  auto all = fx.store->cleanup(CacheStore::CleanupMode::kAll, 0, /*keepPinned=*/false, false);
  EXPECT_EQ(all.victims.size(), 1u);
  EXPECT_EQ(fx.store->listEntries().size(), 0u);
}

TEST(CacheStore, CleanupOlderThanRemovesStaleKeepsRecentAndPinned) {
  CleanupFx fx(2, 8, /*baseAtime=*/1000); // both ancient
  // Make entry 1 "recent" (accessed just now); entry 0 stays ancient.
  setAtime(fx.io, fx.cfg, keyN(1), static_cast<uint64_t>(::time(nullptr)));
  auto rep = fx.store->cleanup(CacheStore::CleanupMode::kOlderThan, 3600, true, false);
  EXPECT_TRUE(rep.locked);
  EXPECT_EQ(rep.victims.size(), 1u);
  EXPECT_FALSE(fx.present(0)); // ancient -> removed
  EXPECT_TRUE(fx.present(1));  // recent -> kept

  // A pinned ancient entry survives an age purge.
  CleanupFx fx2(1, 8, /*baseAtime=*/1000);
  fx2.store->setPinnedByKey(keyN(0), true);
  auto rep2 = fx2.store->cleanup(CacheStore::CleanupMode::kOlderThan, 3600, true, false);
  EXPECT_EQ(rep2.victims.size(), 0u);
  EXPECT_TRUE(fx2.present(0));
}

TEST(CacheStore, CleanupNewerThanRemovesRecentKeepsOldAndPinned) {
  // The mirror of kOlderThan ("undo a polluting run"): entries used
  // WITHIN the window go, older ones stay.
  CleanupFx fx(2, 8, /*baseAtime=*/1000); // both ancient
  setAtime(fx.io, fx.cfg, keyN(1), static_cast<uint64_t>(::time(nullptr)));
  auto rep = fx.store->cleanup(CacheStore::CleanupMode::kNewerThan, 3600, true, false);
  EXPECT_TRUE(rep.locked);
  EXPECT_EQ(rep.victims.size(), 1u);
  EXPECT_TRUE(fx.present(0));  // ancient -> kept
  EXPECT_FALSE(fx.present(1)); // recent -> removed

  // A pinned recent entry survives.
  CleanupFx fx2(1, 8, /*baseAtime=*/1000);
  setAtime(fx2.io, fx2.cfg, keyN(0), static_cast<uint64_t>(::time(nullptr)));
  fx2.store->setPinnedByKey(keyN(0), true);
  auto rep2 = fx2.store->cleanup(CacheStore::CleanupMode::kNewerThan, 3600, true, false);
  EXPECT_EQ(rep2.victims.size(), 0u);
  EXPECT_TRUE(fx2.present(0));
}

TEST(CacheStore, CleanupToSizeEvictsOldestDownToTarget) {
  CleanupFx fx(3, 10); // ~40 KB cached each (10 pages), atimes 0<1<2
  const uint64_t each = 10 * 4096;
  // Target just above one entry: the two oldest must go, the newest survive.
  auto rep = fx.store->cleanup(CacheStore::CleanupMode::kToSize, each + 4096, true, false);
  EXPECT_TRUE(rep.locked);
  EXPECT_EQ(rep.victims.size(), 2u);
  EXPECT_FALSE(fx.present(0)); // oldest
  EXPECT_FALSE(fx.present(1));
  EXPECT_TRUE(fx.present(2)); // newest kept
  EXPECT_LE(fx.store->usageBytes(), each + 4096);
}

TEST(CacheStore, CleanupReportsLockedFalseWhenHeld) {
  CleanupFx fx(2, 8);
  int fd = fx.io.open(fx.cfg.cacheDir + "/LOCK", O_RDWR | O_CREAT, 0600);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(fx.io.flock(fd, LOCK_EX), 0);
  auto rep = fx.store->cleanup(CacheStore::CleanupMode::kAll, 0, false, false);
  EXPECT_FALSE(rep.locked);
  EXPECT_TRUE(rep.victims.empty());
  EXPECT_TRUE(fx.present(0) && fx.present(1)); // nothing removed under contention
  ASSERT_EQ(fx.io.flock(fd, LOCK_UN), 0);
  fx.io.close(fd);
}

TEST(CacheStore, EvictionDisabledWithoutMaxBytes) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  CacheStore store(io, cfg);
  auto src = test::randomBytes(4096, 7);
  auto e = store.open(keyN(0), 4096);
  e->writePages(0, 4096, src.data());
  e->flushMeta(true);
  store.maybeEvict(); // no-op
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0);
}

TEST(CacheStore, VerifyScrub) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  CacheStore store(io, cfg);
  auto src = test::randomBytes(8 * 4096, 8);
  {
    auto e = store.open(keyN(0), src.size());
    e->writePages(0, src.size(), src.data());
  }
  auto r = store.verify(keyN(0), src.size());
  EXPECT_EQ(r.checked, 8u);
  EXPECT_EQ(r.bad, 0u);
}

TEST(CacheStore, VerifyDoesNotWipeUnderMtimeValidation) {
  // Regression: verify must re-open with the entry's OWN mtime/cksum so §7
  // validation adopts (scrubs) instead of treating it as stale and wiping it.
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.validate = ValidateMode::kSizeMtime;
  CacheStore store(io, cfg);
  auto src = test::randomBytes(8 * 4096, 40);
  const uint64_t mtime = 1700000000;
  {
    auto e = store.open(keyN(0), src.size(), mtime);
    ASSERT_TRUE(e);
    e->writePages(0, src.size(), src.data());
    e->flushMeta(true);
  }
  // Verify with the entry's own metadata (as cmdVerify does from the sidecar).
  auto m = MetaFile::load(io, keyN(0).metaPath(cfg.cacheDir));
  ASSERT_TRUE(m);
  auto r = store.verify(keyN(0), m->fileSize, m->originMtime, m->cksumKind, m->originCksum);
  EXPECT_EQ(r.checked, 8u); // scrubbed the real pages (would be 0 if it wiped)
  EXPECT_EQ(r.bad, 0u);
  // The entry survives intact and byte-correct.
  auto e2 = store.open(keyN(0), src.size(), mtime);
  ASSERT_TRUE(e2);
  ASSERT_TRUE(e2->hasRange(0, src.size()));
  std::vector<uint8_t> buf(src.size());
  ASSERT_TRUE(e2->readCached(0, src.size(), buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), src.data(), src.size()));
}

TEST(CacheStore, StatsDumpWritesJsonLine) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  {
    CacheStore store(io, cfg);
    auto src = test::randomBytes(3 * 4096, 9);
    auto e = store.open(keyN(0), src.size());
    e->writePages(0, src.size(), src.data());
    e->flushBuffer(true); // cached_bytes in the dump counts published pages
    std::vector<uint8_t> buf(4096);
    ASSERT_TRUE(e->readCached(0, 4096, buf.data()));
    store.dumpStats();
  } // dtor dumps a second line
  std::vector<std::string> names;
  ASSERT_EQ(io.listDir(td.path() + "/stats", names), 0);
  // The per-file record lands next to the counters file.
  ASSERT_EQ(names.size(), 2u);
  std::string counters, files;
  for (const auto& n : names) {
    if (n.size() > 12 && n.compare(n.size() - 12, 12, ".files.jsonl") == 0)
      files = n;
    else
      counters = n;
  }
  ASSERT_FALSE(counters.empty());
  ASSERT_FALSE(files.empty());
  std::ifstream in(td.path() + "/stats/" + counters);
  std::string line;
  int lines = 0;
  while (std::getline(in, line)) {
    ++lines;
    EXPECT_EQ(line.front(), '{');
    EXPECT_EQ(line.back(), '}');
    EXPECT_NE(line.find("\"opens\":1"), std::string::npos);
    EXPECT_NE(line.find("\"files_opened\":1"), std::string::npos);
    if (lines == 1) { // entry still live in the registry at first dump
      EXPECT_NE(line.find("\"cached_bytes\":12288"), std::string::npos);
    }
  }
  EXPECT_EQ(lines, 2);
  // The per-file record carries the entry's lifetime observation: one open,
  // 4096 bytes served (all first-touch), zero replica bytes.
  std::ifstream fin(td.path() + "/stats/" + files);
  ASSERT_TRUE(std::getline(fin, line));
  EXPECT_NE(line.find("\"opens\":1"), std::string::npos);
  EXPECT_NE(line.find("\"served_bytes\":4096"), std::string::npos);
  EXPECT_NE(line.find("\"first_touch_bytes\":4096"), std::string::npos);
  EXPECT_NE(line.find("\"replica_bytes\":0"), std::string::npos);
  EXPECT_NE(line.find("\"wire_bytes\":12288"), std::string::npos);
}

TEST(CacheStore, InvalidateRemovesEntry) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  CacheStore store(io, cfg);
  auto src = test::randomBytes(4 * 4096, 10);
  auto e = store.open(keyN(0), src.size());
  ASSERT_TRUE(e);
  e->writePages(0, src.size(), src.data());
  e->flushMeta(true);
  store.invalidate(keyN(0));
  struct ::stat st;
  EXPECT_LT(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0);
  EXPECT_LT(io.stat(keyN(0).metaPath(cfg.cacheDir), &st), 0);
  // Open handle keeps serving via its fd; flush never resurrects the sidecar.
  std::vector<uint8_t> buf(4096);
  EXPECT_TRUE(e->readCached(0, 4096, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), src.data(), 4096));
  e->flushMeta(true);
  EXPECT_LT(io.stat(keyN(0).metaPath(cfg.cacheDir), &st), 0);
  // A fresh open after invalidation starts empty.
  e.reset();
  auto e2 = store.open(keyN(0), src.size());
  ASSERT_TRUE(e2);
  EXPECT_FALSE(e2->hasRange(0, 4096));
}

TEST(CacheStore, PageWriteTriggersEvictionWithoutReopen) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 1000000;    // 1 MB
  cfg.highWater = 0.5;       // 500 KB
  cfg.lowWater = 0.25;       // 250 KB
  cfg.evictCheckSeconds = 0; // no rate limit for the test
  cfg.metaFlushSeconds = 0;  // flush sidecars immediately so the authoritative
                             // eviction scan sees the just-written entry
  CacheStore store(io, cfg);
  auto src = test::randomBytes(60 * 4096, 21);

  // Two older entries, 160 KB each (320 KB), with ancient atimes.
  for (int n = 0; n < 2; ++n) {
    auto e = store.open(keyN(n), src.size());
    ASSERT_TRUE(e);
    e->writePages(0, 40 * 4096, src.data());
    e->flushMeta(true);
    setAtime(io, cfg, keyN(n), 1000 + n);
  }
  ASSERT_LE(store.usageBytes(), 320u * 1024); // under high-water

  // A long-lived third entry opened ONCE under the watermark; a single write
  // then pushes total past HIGH_WATER. Eviction must fire from the write path
  // (no new open()), purging the two old entries.
  auto e2 = store.open(keyN(2), src.size());
  ASSERT_TRUE(e2);
  e2->writePages(0, 50 * 4096, src.data()); // +200 KB -> ~520 KB > 500 KB

  struct ::stat st;
  EXPECT_LT(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // evicted
  EXPECT_LT(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0); // evicted
  EXPECT_EQ(io.stat(keyN(2).dataPath(cfg.cacheDir), &st), 0); // fresh, survives
  EXPECT_GE(store.stats().evictedEntries.load(), 2u);
}

TEST(CacheStore, StatvfsFloorTriggersEvictionRespectingPins) {
  TempDir td;
  RealIO real;
  FaultIO io{real};
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 100ull << 30; // huge: the byte trigger never fires
  cfg.minFreeBytes = 50ull << 30;
  cfg.evictCheckSeconds = 0;
  CacheStore store(io, cfg);
  io.forceAvail(100ull << 30); // plenty of disk during setup (no eviction)
  auto src = test::randomBytes(10 * 4096, 22);
  for (int n = 0; n < 3; ++n) {
    auto e = store.open(keyN(n), src.size());
    ASSERT_TRUE(e);
    e->writePages(0, src.size(), src.data());
    if (n == 0)
      e->setPinned(true);
    e->flushMeta(true);
  }
  // Disk now below the floor even though the byte budget is nowhere near: the
  // statvfs floor must drive eviction, and still skip pinned entries.
  io.forceAvail(10ull << 30);
  int ev = store.evictNow();
  EXPECT_GT(ev, 0);
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // pinned survives
  EXPECT_LT(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0);
  EXPECT_LT(io.stat(keyN(2).dataPath(cfg.cacheDir), &st), 0);
}

TEST(CacheStore, SetPinnedByKeyProtectsClosedEntry) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  // High-water (500 KB) above the ~84 KB setup so eviction fires only on the
  // explicit evictNow below (not mid-setup, now that being-over evicts eagerly);
  // low-water 50 KB so that evictNow removes one 40 KB entry and stops.
  cfg.maxBytes = 1000000;
  cfg.highWater = 0.5;
  cfg.lowWater = 0.05;
  CacheStore store(io, cfg);
  auto src = test::randomBytes(10 * 4096, 23);
  // Live path: pin an entry that is currently open (as a running analysis holds
  // it) — mutates the in-memory entry directly.
  {
    auto live = store.open(keyN(5), src.size());
    live->writePages(0, 4096, src.data());
    ASSERT_TRUE(store.setPinnedByKey(keyN(5), true));
    EXPECT_TRUE(live->pinned());
    ASSERT_TRUE(store.setPinnedByKey(keyN(5), false));
    EXPECT_FALSE(live->pinned());
  }
  for (int n = 0; n < 2; ++n) {
    auto e = store.open(keyN(n), src.size());
    e->writePages(0, 10 * 4096, src.data());
    e->flushMeta(true);
  } // both entries CLOSED (registry weak-refs expired)
  setAtime(io, cfg, keyN(0), 1000); // oldest -> first LRU candidate
  setAtime(io, cfg, keyN(1), 2000);
  // Pin the CLOSED oldest entry by key (exercises the sidecar-rewrite path).
  ASSERT_TRUE(store.setPinnedByKey(keyN(0), true));
  store.evictNow();
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // pinned survives
  EXPECT_LT(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0); // evicted
}

TEST(CacheStore, RunningUsageTracksAndReconciles) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 10000000; // 10 MB: no eviction
  cfg.evictCheckSeconds = 0;
  CacheStore store(io, cfg);
  EXPECT_EQ(store.approxUsageBytes(), 0u); // fresh cache seeds to empty
  auto src = test::randomBytes(40 * 4096, 24);
  for (int n = 0; n < 3; ++n) {
    auto e = store.open(keyN(n), src.size());
    e->writePages(0, 40 * 4096, src.data());
    e->flushMeta(true);
  }
  // Single process: the cheap running estimate equals an authoritative scan.
  EXPECT_EQ(store.approxUsageBytes(), store.usageBytes());
  EXPECT_EQ(store.usageBytes(), 3u * 40 * 4096);
  store.evictNow(); // reconciles (no eviction here); invariant holds
  EXPECT_EQ(store.approxUsageBytes(), store.usageBytes());
}

TEST(CacheStore, AutoBudgetIsFloorGoverned) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.budgetAuto = true; // as fromEnv sets when UCACHE_MAX_BYTES is unset
  CacheStore store(io, cfg);
  // Default: no fixed byte cap; a free-disk floor governs growth (use the disk,
  // evict at the floor). The floor is set and clamped to <= half of free-at-init.
  EXPECT_EQ(store.config().maxBytes, 0u);
  EXPECT_GT(store.config().minFreeBytes, 0u);
}

TEST(CacheStore, OpenEntryNotEvictedMidFillByteTrigger) {
  // An entry being actively filled must never be unlinked from under itself,
  // even when its own writes push the cache past the budget.
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 100000;
  cfg.highWater = 0.5; // 50 KB
  cfg.lowWater = 0.1;  // 10 KB
  cfg.evictCheckSeconds = 0;
  cfg.metaFlushSeconds = 0;
  CacheStore store(io, cfg);
  auto src = test::randomBytes(40 * 4096, 30);
  auto e = store.open(keyN(0), src.size());
  ASSERT_TRUE(e);
  e->writePages(0, 20 * 4096, src.data()); // 80 KB > high-water -> triggers eviction
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // open entry survives
  EXPECT_TRUE(e->hasRange(0, 20 * 4096));
}

TEST(CacheStore, OpenEntryNotEvictedMidFillDiskFloor) {
  TempDir td;
  RealIO real;
  FaultIO io{real};
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 100ull << 30;
  cfg.minFreeBytes = 50ull << 30;
  cfg.evictCheckSeconds = 0;
  CacheStore store(io, cfg);
  io.forceAvail(100ull << 30);
  auto src = test::randomBytes(20 * 4096, 31);
  auto e = store.open(keyN(0), src.size());
  ASSERT_TRUE(e);
  e->writePages(0, 10 * 4096, src.data()); // first half, disk fine
  io.forceAvail(10ull << 30);              // now below the floor
  e->writePages(10 * 4096, 10 * 4096, src.data() + 10 * 4096); // triggers disk-floor evict
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // open entry survives
  EXPECT_TRUE(e->hasRange(0, 20 * 4096));
}

TEST(CacheStore, DiskFloorEvictsWithByteBudgetDisabled) {
  // The statvfs floor is the primary cross-process guard and MUST work even
  // when the byte budget is off (maxBytes==0).
  TempDir td;
  RealIO real;
  FaultIO io{real};
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 0;               // byte budget OFF
  cfg.minFreeBytes = 50ull << 30; // disk floor ON
  cfg.evictCheckSeconds = 0;
  CacheStore store(io, cfg);
  io.forceAvail(100ull << 30);
  auto src = test::randomBytes(10 * 4096, 32);
  for (int n = 0; n < 2; ++n) {
    auto e = store.open(keyN(n), src.size());
    e->writePages(0, src.size(), src.data());
    e->flushMeta(true);
  }
  io.forceAvail(10ull << 30);
  EXPECT_GT(store.evictNow(), 0); // floor triggers eviction with maxBytes==0
}

TEST(CacheStore, EvictNowWithoutBudgetOrFloorKeepsEverything) {
  // maxBytes==0 && minFreeBytes==0: byteTarget is 0, but the guard must prevent
  // a manual evictNow from wiping the whole cache.
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path(); // both off
  CacheStore store(io, cfg);
  auto src = test::randomBytes(10 * 4096, 33);
  for (int n = 0; n < 2; ++n) {
    auto e = store.open(keyN(n), src.size());
    e->writePages(0, src.size(), src.data());
    e->flushMeta(true);
  }
  EXPECT_EQ(store.evictNow(), 0);
  struct ::stat st;
  EXPECT_EQ(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0);
  EXPECT_EQ(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0);
}

TEST(CacheStore, SetPinnedByKeyOnEvictedEntryDoesNotResurrect) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  CacheStore store(io, cfg);
  auto src = test::randomBytes(4 * 4096, 34);
  {
    auto e = store.open(keyN(0), src.size());
    e->writePages(0, src.size(), src.data());
    e->flushMeta(true);
  } // closed, on disk
  io.unlink(keyN(0).dataPath(cfg.cacheDir)); // simulate the entry being evicted
  io.unlink(keyN(0).metaPath(cfg.cacheDir));
  EXPECT_FALSE(store.setPinnedByKey(keyN(0), true)); // not cached -> false
  struct ::stat st;
  EXPECT_LT(io.stat(keyN(0).metaPath(cfg.cacheDir), &st), 0); // no orphaned sidecar
}

TEST(CacheStore, DiskFloorEvictsOnlyEnoughToClearFloor) {
  // Freeing one entry lifts avail back above resumeFree -> eviction stops with
  // the rest surviving (hysteresis / partial stop).
  TempDir td;
  RealIO real;
  FaultIO io{real};
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 100ull << 30; // byte trigger off (usage tiny)
  cfg.minFreeBytes = 1000000;  // resumeFree = 1,100,000
  cfg.evictCheckSeconds = 0;
  CacheStore store(io, cfg);
  io.forceAvail(2000000); // above resumeFree during setup
  const uint64_t ENTRY = 10 * 4096;
  auto src = test::randomBytes(ENTRY, 35);
  for (int n = 0; n < 3; ++n) {
    auto e = store.open(keyN(n), ENTRY);
    e->writePages(0, ENTRY, src.data());
    e->flushMeta(true);
    setAtime(io, cfg, keyN(n), 1000 + n);
  }
  io.forceAvail(1100000 - ENTRY); // one entry's worth below resumeFree
  EXPECT_EQ(store.evictNow(), 1);
  struct ::stat st;
  EXPECT_LT(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // oldest evicted
  EXPECT_EQ(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0); // rest survive
  EXPECT_EQ(io.stat(keyN(2).dataPath(cfg.cacheDir), &st), 0);
}

TEST(CacheStore, BudgetEnforcedDespiteLazySidecarFlush) {
  // Regression for the reconcile bug: with metaFlushSeconds large, a freshly
  // written open entry's sidecar isn't flushed by writePages, yet evictNow's
  // pre-scan flush makes the scan authoritative, so the byte budget is enforced
  // (older closed entries evicted). Would evict only 1 (or 0) before the fix.
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 1000000; // 1 MB
  cfg.highWater = 0.5;    // 500 KB
  cfg.lowWater = 0.25;    // 250 KB
  cfg.evictCheckSeconds = 0;
  cfg.metaFlushSeconds = 3600; // effectively never auto-flushes during the test
  CacheStore store(io, cfg);
  auto src = test::randomBytes(60 * 4096, 36);
  for (int n = 0; n < 2; ++n) {
    auto e = store.open(keyN(n), src.size());
    e->writePages(0, 40 * 4096, src.data()); // 160 KB each, 320 KB total
    e->flushMeta(true);                       // old entries' sidecars ARE current
    setAtime(io, cfg, keyN(n), 1000 + n);
  }
  auto e2 = store.open(keyN(2), src.size());
  ASSERT_TRUE(e2);
  e2->writePages(0, 50 * 4096, src.data()); // +200 KB -> 520 KB; sidecar NOT flushed
  // Buffered fill: pages stage in RAM; the budget check fires when the buffer drains
  // (cap/interval/close). Simulate the trigger explicitly — the point under
  // test is scan authority over lazy sidecars, not the buffer policy.
  e2->flushBuffer(true);
  struct ::stat st;
  EXPECT_LT(io.stat(keyN(0).dataPath(cfg.cacheDir), &st), 0); // old evicted to low-water
  EXPECT_LT(io.stat(keyN(1).dataPath(cfg.cacheDir), &st), 0);
  EXPECT_EQ(io.stat(keyN(2).dataPath(cfg.cacheDir), &st), 0); // open entry survives
  EXPECT_GE(store.stats().evictedEntries.load(), 2u);
}

// The cleanup victim loop fans out over worker threads and
// unlinks only artifacts the scan listed. Enough entries to engage the pool;
// artifacts sprinkled on a subset must vanish with their entries, and the
// report must stay complete and byte-accurate.
TEST(CacheStore, CleanupParallelRemovesEntriesAndListedArtifacts) {
  CleanupFx fx(120, 2);
  // Replica artifacts on every 7th entry, .cost on every 11th.
  for (int i = 0; i < 120; i += 7) {
    const std::string base = keyN(i).objectDir(fx.cfg.cacheDir) + "/" + keyN(i).hashHex;
    for (const char* suf : {".tdata", ".tmeta", ".tok", ".val"})
      std::ofstream(base + suf) << "x";
  }
  for (int i = 0; i < 120; i += 11)
    std::ofstream(keyN(i).objectDir(fx.cfg.cacheDir) + "/" + keyN(i).hashHex + ".cost") << "1";

  // Dry run first: full report, nothing touched.
  auto dry = fx.store->cleanup(CacheStore::CleanupMode::kAll, 0, false, true);
  EXPECT_EQ(dry.victims.size(), 120u);
  EXPECT_TRUE(fx.present(0) && fx.present(119));

  auto rep = fx.store->cleanup(CacheStore::CleanupMode::kAll, 0, false, false);
  EXPECT_TRUE(rep.locked);
  EXPECT_EQ(rep.victims.size(), 120u);
  EXPECT_EQ(rep.bytes, dry.bytes); // parallel removal credits exactly the scan
  for (int i = 0; i < 120; ++i)
    EXPECT_FALSE(fx.present(i)) << "entry " << i;
  struct ::stat st;
  for (int i = 0; i < 120; i += 7) {
    const std::string base = keyN(i).objectDir(fx.cfg.cacheDir) + "/" + keyN(i).hashHex;
    for (const char* suf : {".tdata", ".tmeta", ".tok", ".val"})
      EXPECT_LT(fx.io.stat(base + suf, &st), 0) << i << suf;
  }
  for (int i = 0; i < 120; i += 11)
    EXPECT_LT(fx.io.stat(keyN(i).objectDir(fx.cfg.cacheDir) + "/" + keyN(i).hashHex + ".cost",
                         &st), 0) << i << ".cost";
  EXPECT_EQ(fx.store->listEntries().size(), 0u);
}

// Eviction's victim loop uses the same artifact mask: replica files and the
// freshness/cost markers leave WITH their evicted entry.
TEST(CacheStore, EvictNowDropsListedArtifactsWithEntry) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 100 * 4096; // force everything over budget
  cfg.highWater = 0.5;
  cfg.lowWater = 0.25;
  CacheStore store(io, cfg);
  auto src = test::randomBytes(60 * 4096, 12);
  {
    auto e = store.open(keyN(0), src.size());
    e->writePages(0, 60 * 4096, src.data());
    e->flushMeta(true);
  }
  const std::string base = keyN(0).objectDir(cfg.cacheDir) + "/" + keyN(0).hashHex;
  for (const char* suf : {".tdata", ".tmeta", ".tok", ".val", ".cost"})
    std::ofstream(base + suf) << "x";
  EXPECT_EQ(store.evictNow(), 1);
  struct ::stat st;
  for (const char* suf : {".tdata", ".tmeta", ".tok", ".val", ".cost"})
    EXPECT_LT(io.stat(base + suf, &st), 0) << suf;
}

// The eviction floor must be answerable from a Config that has NOT been through
// a store's resolveBudget(), because that is what callers outside CacheStore
// hold. Reading Config::minFreeBytes directly instead returns 0 in the DEFAULT
// configuration (automatic floor, nothing set explicitly) — and a caller that
// reads 0 as "no floor, nothing to protect" silently disables itself in exactly
// the configuration almost every user runs. That is not hypothetical: it shipped.
TEST(CacheStoreBudget, EffectiveFloorIsAnswerableBeforeResolution) {
  ucache::test::TempDir td;
  ucache::RealIO io;
  ucache::Config cfg;
  cfg.cacheDir = td.path();
  cfg.budgetAuto = true; // what Config::fromEnv sets when max_bytes is unset
  cfg.minFreeBytes = 0;  // ... and the field a caller would read

  const uint64_t floor = ucache::CacheStore::effectiveMinFree(cfg, io);
  EXPECT_GT(floor, 0u) << "the automatic floor must be visible without a store";

  // And it must agree with what the store actually enforces.
  ucache::CacheStore store(io, cfg);
  EXPECT_EQ(store.config().minFreeBytes, floor);

  // Explicit beats automatic, and is returned unchanged.
  ucache::Config explicitCfg = cfg;
  explicitCfg.minFreeBytes = 7ull << 30;
  EXPECT_EQ(ucache::CacheStore::effectiveMinFree(explicitCfg, io), 7ull << 30);

  // Eviction genuinely off (no auto policy, no floor) is the ONLY case that may
  // report "nothing to protect".
  ucache::Config offCfg = cfg;
  offCfg.budgetAuto = false;
  EXPECT_EQ(ucache::CacheStore::effectiveMinFree(offCfg, io), 0u);
}
