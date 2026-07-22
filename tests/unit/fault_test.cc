// Fault injection over FaultIO: short writes, ENOSPC, EIO, torn
// meta — every path must end in clean degradation: no crash, no exception,
// no bad data served, counters reflecting the event.
#include "CacheStore.h"
#include "testing/FaultIO.h"

#include "TestUtil.h"
#include <cerrno>
#include <gtest/gtest.h>

using namespace ucache;
using test::TempDir;

namespace {
struct Fx {
  TempDir td;
  RealIO real;
  FaultIO io{real};
  Config cfg;
  Stats stats;
  UrlKey key = *UrlKey::parse("root://h//f");
  std::vector<uint8_t> src = test::randomBytes(64 * 4096, 77);

  Fx() {
    cfg.cacheDir = td.path();
    cfg.pageSize = 4096;
  }
  std::shared_ptr<FileEntry> open() {
    return FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  }
};
} // namespace

TEST(Fault, EnospcDuringPageWriteFailsOpen) {
  // Buffered fill: writePages stages in RAM; the disk write (and thus the ENOSPC) moves
  // to the flush boundary. Staged pages are dropped on flush failure — the
  // range degrades back to a miss, exactly the old fail-open contract.
  Fx fx;
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 8192, fx.src.data()); // 2 pages staged
  e->flushBuffer(true);                  // durable
  fx.io.failNth(IoOp::kPwrite, 1, ENOSPC);
  e->writePages(8192, 8192, fx.src.data() + 8192);
  EXPECT_TRUE(e->hasRange(8192, 8192)); // staged: readable pre-flush
  e->flushBuffer(true);                 // fails: no crash, pages dropped
  EXPECT_GE(fx.stats.failopenEvents.load(), 1u);
  EXPECT_FALSE(e->hasRange(8192, 8192)); // nothing published
  // Cached data unaffected and correct.
  std::vector<uint8_t> buf(8192);
  ASSERT_TRUE(e->readCached(0, 8192, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 8192));
  // Recovery: next write+flush succeeds.
  e->writePages(8192, 8192, fx.src.data() + 8192);
  e->flushBuffer(true);
  EXPECT_TRUE(e->hasRange(8192, 8192));
}

TEST(Fault, LegacyDirectWriteEnospc) {
  // fill_buffer_mb = 0 keeps the pre-buffering immediate-write path byte-for-byte.
  Fx fx;
  fx.cfg.fillBufferMb = 0;
  auto e = fx.open();
  ASSERT_TRUE(e);
  fx.io.failNth(IoOp::kPwrite, 1, ENOSPC);
  e->writePages(0, 8192, fx.src.data());
  EXPECT_GE(fx.stats.failopenEvents.load(), 1u);
  EXPECT_FALSE(e->hasRange(0, 8192));
  e->writePages(0, 8192, fx.src.data());
  EXPECT_TRUE(e->hasRange(0, 8192));
}

TEST(Fault, StagedPagesServeWithoutDiskReads) {
  // Staged pages must serve from RAM — arm a pread fault to PROVE the
  // disk is never touched while the range is staged.
  Fx fx;
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 16384, fx.src.data()); // staged, not flushed
  fx.io.dieAt(IoOp::kPread, 1, EIO);      // any disk read would now fail
  std::vector<uint8_t> buf(16384);
  ASSERT_TRUE(e->readCached(0, 16384, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 16384));
  fx.io.reset();
  e->flushBuffer(true); // now durable; disk read path takes over
  ASSERT_TRUE(e->readCached(4096, 4096, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 4096, 4096));
}

TEST(Fault, EioOnReadDegradesToMiss) {
  Fx fx;
  auto e = fx.open();
  e->writePages(0, 4096, fx.src.data());
  e->flushBuffer(true); // this test exercises the DISK read path
  fx.io.failNth(IoOp::kPread, 1, EIO);
  std::vector<uint8_t> buf(4096);
  EXPECT_FALSE(e->readCached(0, 4096, buf.data())); // clean miss signal
  // The page was quarantined; refetch heals (staged copy serves immediately).
  e->writePages(0, 4096, fx.src.data());
  ASSERT_TRUE(e->readCached(0, 4096, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 4096));
}

TEST(Fault, ShortWriteIsRetriedTransparently) {
  Fx fx;
  auto e = fx.open();
  fx.io.shortWriteNth(1, 100); // first data pwrite comes up short
  e->writePages(0, 4096, fx.src.data());
  EXPECT_TRUE(e->hasRange(0, 4096)); // pwriteFull retried to completion
  std::vector<uint8_t> buf(4096);
  ASSERT_TRUE(e->readCached(0, 4096, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 4096));
  EXPECT_EQ(fx.stats.failopenEvents.load(), 0u);
}

TEST(Fault, MetaStoreFailuresKeepOldSidecar) {
  Fx fx;
  {
    auto e = fx.open();
    e->writePages(0, 8192, fx.src.data());
    e->flushMeta(true); // good sidecar on disk
  }
  // tmp-file open fails -> store fails -> old sidecar intact.
  MetaData m = MetaData::fresh("other", 4096, 4096);
  fx.io.failNth(IoOp::kOpen, 1, EACCES);
  EXPECT_LT(MetaFile::store(fx.io, fx.key.metaPath(fx.cfg.cacheDir), m, false), 0);
  // pwrite fails mid-store -> old sidecar intact, tmp removed.
  fx.io.failNth(IoOp::kPwrite, 1, ENOSPC);
  EXPECT_LT(MetaFile::store(fx.io, fx.key.metaPath(fx.cfg.cacheDir), m, false), 0);
  // rename fails -> old sidecar intact.
  fx.io.failNth(IoOp::kRename, 1, EIO);
  EXPECT_LT(MetaFile::store(fx.io, fx.key.metaPath(fx.cfg.cacheDir), m, false), 0);
  auto d = MetaFile::load(fx.io, fx.key.metaPath(fx.cfg.cacheDir));
  ASSERT_TRUE(d);
  EXPECT_EQ(d->key, fx.key.key); // still the original entry's sidecar
  struct ::stat st;
  EXPECT_LT(fx.real.stat(fx.key.metaPath(fx.cfg.cacheDir) + ".tmp", &st), 0);
}

TEST(Fault, MetaFlushFailureRetriesLater) {
  Fx fx;
  auto e = fx.open();
  e->writePages(0, 4096, fx.src.data());
  fx.io.failNth(IoOp::kRename, 1, EIO); // the flush's sidecar store fails
  e->flushBuffer(true); // publishes the page; its meta store hits the fault
  EXPECT_GE(fx.stats.failopenEvents.load(), 1u);
  e->flushMeta(true); // dirty was retained; this one succeeds
  auto d = MetaFile::load(fx.real, fx.key.metaPath(fx.cfg.cacheDir));
  ASSERT_TRUE(d);
  EXPECT_TRUE(d->bitmap.get(0));
}

TEST(Fault, DyingBackendDuringPopulationNeverServesWrongBytes) {
  Fx fx;
  {
    auto e = fx.open();
    // Persist a first batch and its sidecar...
    for (uint64_t off = 0; off < 8 * 16384; off += 16384)
      e->writePages(off, 16384, fx.src.data() + off);
    e->flushMeta(true);
    // ...then the backend dies partway through the rest of the run.
    fx.io.dieAt(IoOp::kPwrite, 5, EIO);
    for (uint64_t off = 8 * 16384; off + 16384 <= fx.src.size(); off += 16384)
      e->writePages(off, 16384, fx.src.data() + off);
    // Entry destructor flushes meta — fails too; must not crash.
  }
  fx.io.reset();
  auto e = fx.open();
  ASSERT_TRUE(e);
  // Whatever survived must be byte-perfect; scrub confirms zero bad pages.
  e->verifyAll();
  auto r = e->verifyAll();
  EXPECT_EQ(r.bad, 0u);
  std::vector<uint8_t> buf(fx.src.size());
  for (uint64_t p = 0; p < fx.src.size() / 4096; ++p) {
    if (e->hasRange(p * 4096, 4096)) {
      ASSERT_TRUE(e->readCached(p * 4096, 4096, buf.data()));
      ASSERT_EQ(0, memcmp(buf.data(), fx.src.data() + p * 4096, 4096)) << "page " << p;
    }
  }
}

TEST(Fault, OpenFailuresReturnNull) {
  Fx fx;
  fx.io.failNth(IoOp::kMkdirs, 1, EACCES);
  EXPECT_EQ(fx.open(), nullptr);
  fx.io.reset();
  fx.io.failNth(IoOp::kOpen, 1, EMFILE);
  EXPECT_EQ(fx.open(), nullptr);
  fx.io.reset();
  fx.io.failNth(IoOp::kFtruncate, 2, EIO); // fresh-entry sizing fails
  EXPECT_EQ(fx.open(), nullptr);
  fx.io.reset();
  auto e = fx.open(); // and a clean open still works afterwards
  EXPECT_NE(e, nullptr);
}

TEST(Fault, FdatasyncFailureNeverPublishesBits) {
  // D1 data-before-bit ordering under fsync loss: payload lands but the sync
  // fails, so the bits must never publish — pages are refetched, not trusted.
  Fx fx;
  fx.cfg.fsync = FsyncMode::kData;
  auto e = fx.open();
  ASSERT_TRUE(e);
  fx.io.failNth(IoOp::kFdatasync, 1, EIO);
  e->writePages(0, 8192, fx.src.data());
  e->flushBuffer(true); // pwrites succeed, fdatasync fails: pages dropped
  EXPECT_GE(fx.stats.failopenEvents.load(), 1u);
  EXPECT_FALSE(e->hasRange(0, 4096)); // nothing published
  EXPECT_FALSE(e->hasRange(4096, 4096));
  // Recovery: the same span persists once fdatasync works again.
  e->writePages(0, 8192, fx.src.data());
  e->flushBuffer(true);
  EXPECT_TRUE(e->hasRange(0, 8192));
  std::vector<uint8_t> buf(8192);
  ASSERT_TRUE(e->readCached(0, 8192, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 8192));
}

TEST(Fault, PunchHoleHardFailureStillClearsBits) {
  // A punchHole error that is NOT EOPNOTSUPP/ENOSYS (e.g. EIO) takes the warn
  // branch: reclaim is best-effort, but the released bits stay gone (D1: bits
  // durable before the bytes vanish) and untouched pages still serve.
  Fx fx;
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 16 * 4096, fx.src.data());
  fx.io.failNth(IoOp::kPunchHole, 1, EIO);
  uint64_t punched = e->releaseRanges({{0, 4 * 4096}});
  EXPECT_EQ(punched, 0u);             // nothing reclaimed...
  EXPECT_FALSE(e->hasRange(0, 4096)); // ...but the range is released regardless
  std::vector<uint8_t> buf(4096);
  ASSERT_TRUE(e->readCached(4 * 4096, 4096, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 4 * 4096, 4096));
}

TEST(Fault, EvictionStatvfsFailureDoesNotBreakReads) {
  // The eviction disk-floor check calls statvfs on the write path. If it errors,
  // eviction is simply skipped — reads must still succeed.
  TempDir td;
  RealIO real;
  FaultIO io{real};
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.maxBytes = 1u << 20;       // 1 MB
  cfg.minFreeBytes = 1ull << 30; // floor active -> maybeEvict calls spaceInfo
  cfg.evictCheckSeconds = 0;
  CacheStore store(io, cfg);
  UrlKey k = *UrlKey::parse("root://h//evict-failopen");
  auto src = test::randomBytes(8 * 4096, 43);
  auto e = store.open(k, src.size());
  ASSERT_TRUE(e);
  io.failSpaceInfo(true);                     // every eviction-check statvfs errors
  e->writePages(0, src.size(), src.data());   // fires maybeEvict -> spaceInfo fails
  std::vector<uint8_t> buf(4096);
  ASSERT_TRUE(e->readCached(0, 4096, buf.data())); // no crash; cached data intact
  EXPECT_EQ(0, memcmp(buf.data(), src.data(), 4096));
}
