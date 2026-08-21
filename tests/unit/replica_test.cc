// Replica store tests. Fail-open FIRST: a replica that is torn, stale,
// corrupt, or mid-publish must never be adopted — worst case is "no
// replica" (the v1 view), never wrong bytes.
#include "ReplicaFile.h"
#include "ReplicaStore.h"

#include "IOBackend.h"
#include "CacheStore.h"
#include "TestUtil.h"
#include "testing/FaultIO.h"
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>

using namespace ucache;
using test::TempDir;

// The D1 coexistence contract is compile-time: the v1 sidecar format must
// not change with replicas present (a v1.0.0 reader clobbers unknown versions).
static_assert(MetaData::kFormatVersion == 1, "replicas must not bump the v1 .meta format");

namespace {

constexpr uint32_t kP = ReplicaMeta::kOverlayPageSize;

UrlKey key() { return *UrlKey::parse("root://origin//data/f.root"); }

ReplicaMeta sampleMeta(uint64_t originSize, uint64_t overlayLen) {
  ReplicaMeta m;
  m.encoding = ReplicaMeta::kZstd1;
  m.encoderVersion = 10506;
  m.originSize = originSize;
  m.virtualSize = originSize + overlayLen; // extension past origin EOF
  // Two overlay extents: a small patched window near BOF and the extension.
  m.extents.push_back({100, 16, 0});
  m.extents.push_back({originSize, overlayLen - 16, 16});
  m.superseded.push_back({4096, 8192});
  return m;
}

struct Fixture {
  TempDir td;
  RealIO real;
  FaultIO io{real};
  Config cfg;
  Stats stats;
  std::unique_ptr<CacheStore> store;
  std::unique_ptr<ReplicaStore> rs;

  Fixture() {
    cfg.cacheDir = td.path();
    cfg.pageSize = 4096;
    store = std::make_unique<CacheStore>(io, cfg);
    store->disableStatsDump();
    rs = std::make_unique<ReplicaStore>(io, cfg, stats);
  }
};

} // namespace

// ---------------------------------------------------------------- format --

TEST(ReplicaFile, RoundTrip) {
  auto m = sampleMeta(1 << 20, 3 * kP + 123);
  m.key = "root://origin:1094//data/f.root";
  m.tdataBytes = 3 * kP + 123;
  m.pageCrcs = {1, 2, 3, 4};
  auto buf = ReplicaFile::serialize(m);
  auto d = ReplicaFile::deserialize(buf.data(), buf.size());
  ASSERT_TRUE(d);
  EXPECT_EQ(d->key, m.key);
  EXPECT_EQ(d->originSize, m.originSize);
  EXPECT_EQ(d->virtualSize, m.virtualSize);
  EXPECT_EQ(d->tdataBytes, m.tdataBytes);
  EXPECT_EQ(d->encoding, ReplicaMeta::kZstd1);
  EXPECT_EQ(d->encoderVersion, 10506u);
  ASSERT_EQ(d->extents.size(), 2u);
  EXPECT_EQ(d->extents[1].virtOff, m.originSize);
  ASSERT_EQ(d->superseded.size(), 1u);
  EXPECT_EQ(d->superseded[0].len, 8192u);
  EXPECT_EQ(d->pageCrcs, m.pageCrcs);
}

TEST(ReplicaFile, TornCorruptAndMismatchedAreAbsent) {
  auto m = sampleMeta(1 << 20, kP);
  m.tdataBytes = kP;
  m.pageCrcs = {7};
  auto buf = ReplicaFile::serialize(m);
  // truncation
  EXPECT_FALSE(ReplicaFile::deserialize(buf.data(), buf.size() - 1));
  // bit flip
  auto bad = buf;
  bad[buf.size() / 2] ^= 0x40;
  EXPECT_FALSE(ReplicaFile::deserialize(bad.data(), bad.size()));
  // future format version
  auto v2 = buf;
  v2[4] = 2; // little-endian version byte
  EXPECT_FALSE(ReplicaFile::deserialize(v2.data(), v2.size()));
}

TEST(ReplicaFile, InconsistentGeometryRejected) {
  // Overlapping extents.
  auto m = sampleMeta(1 << 20, 2 * kP);
  m.tdataBytes = 2 * kP;
  m.pageCrcs = {0, 0};
  m.extents = {{100, 200, 0}, {150, 100, 200}};
  auto buf = ReplicaFile::serialize(m);
  EXPECT_FALSE(ReplicaFile::deserialize(buf.data(), buf.size()));
  // Extent past tdataBytes.
  m = sampleMeta(1 << 20, kP);
  m.tdataBytes = kP;
  m.pageCrcs = {0};
  m.extents = {{0, kP + 1, 0}};
  buf = ReplicaFile::serialize(m);
  EXPECT_FALSE(ReplicaFile::deserialize(buf.data(), buf.size()));
  // virtualSize < originSize.
  m = sampleMeta(1 << 20, kP);
  m.tdataBytes = kP;
  m.pageCrcs = {0};
  m.virtualSize = m.originSize - 1;
  m.extents.clear();
  buf = ReplicaFile::serialize(m);
  EXPECT_FALSE(ReplicaFile::deserialize(buf.data(), buf.size()));
}

// ------------------------------------------------- fail-open (tested first) --

TEST(ReplicaStoreFailOpen, NoReplicaIsSilentlyAbsent) {
  Fixture f;
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);
  EXPECT_EQ(f.stats.replicaInvalid.load(), 0u); // absence is not an error
}

TEST(ReplicaStoreFailOpen, TornSidecarQuarantined) {
  Fixture f;
  auto overlay = test::randomBytes(kP + 500, 42);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  // Truncate the sidecar to a torn state.
  const std::string mPath = ReplicaStore::tmetaPath(key(), f.cfg.cacheDir);
  ASSERT_EQ(::truncate(mPath.c_str(), 20), 0);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);
  EXPECT_EQ(f.stats.replicaInvalid.load(), 1u);
  struct ::stat st; // quarantined: both files gone
  EXPECT_NE(f.io.stat(mPath, &st), 0);
  EXPECT_NE(f.io.stat(ReplicaStore::tdataPath(key(), f.cfg.cacheDir), &st), 0);
}

TEST(ReplicaStoreFailOpen, OriginChangeInvalidatesReplica) {
  Fixture f;
  auto overlay = test::randomBytes(2 * kP, 43);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  EXPECT_EQ(f.rs->openView(key(), (1 << 20) + 1), nullptr); // size changed
  EXPECT_EQ(f.stats.replicaInvalid.load(), 1u);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr); // quarantined => gone
}

TEST(ReplicaStoreFailOpen, TdataCorruptionCaughtAtOpen) {
  Fixture f;
  auto overlay = test::randomBytes(3 * kP + 77, 44);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  // Flip one byte in the middle overlay page.
  const std::string dPath = ReplicaStore::tdataPath(key(), f.cfg.cacheDir);
  int fd = ::open(dPath.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  uint8_t b;
  ASSERT_EQ(::pread(fd, &b, 1, kP + 100), 1);
  b ^= 1;
  ASSERT_EQ(::pwrite(fd, &b, 1, kP + 100), 1);
  ::close(fd);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);
  EXPECT_GE(f.stats.replicaCrcFailures.load(), 1u);
  EXPECT_EQ(f.stats.replicaOpens.load(), 0u);
}

TEST(ReplicaStoreFailOpen, CorruptionAfterOpenLatchesInvalid) {
  Fixture f;
  auto overlay = test::randomBytes(2 * kP, 45);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  auto v = f.rs->openView(key(), 1 << 20);
  ASSERT_TRUE(v);
  // Corrupt page 1 AFTER the open-time verify.
  const std::string dPath = ReplicaStore::tdataPath(key(), f.cfg.cacheDir);
  int fd = ::open(dPath.c_str(), O_RDWR);
  uint8_t b;
  ASSERT_EQ(::pread(fd, &b, 1, kP + 5), 1);
  b ^= 0x80;
  ASSERT_EQ(::pwrite(fd, &b, 1, kP + 5), 1);
  ::close(fd);
  std::vector<uint8_t> buf(100);
  EXPECT_FALSE(v->read(kP, 100, buf.data())); // never silent bad bytes
  EXPECT_TRUE(v->invalid());
  // Page 0 is still fine but the view is latched — reads there still work
  // mechanically (per-page verify), which is what the repair ladder needs.
  EXPECT_TRUE(v->read(0, 100, buf.data()));
}

TEST(ReplicaStoreFailOpen, PublishFailuresLeaveNothingAdoptable) {
  Fixture f;
  auto overlay = test::randomBytes(kP, 46);
  auto meta = sampleMeta(1 << 20, overlay.size());

  f.io.failNth(IoOp::kPwrite, 1, ENOSPC); // overlay write fails
  EXPECT_LT(f.rs->publish(key(), meta, overlay.data(), overlay.size()), 0);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);

  f.io.reset();
  f.io.failNth(IoOp::kRename, 1, EIO); // .tdata rename fails
  EXPECT_LT(f.rs->publish(key(), meta, overlay.data(), overlay.size()), 0);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);

  f.io.reset();
  f.io.failNth(IoOp::kRename, 2, EIO); // .tmeta rename fails after .tdata landed
  EXPECT_LT(f.rs->publish(key(), meta, overlay.data(), overlay.size()), 0);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);
  struct ::stat st; // and the orphan .tdata was cleaned up eagerly
  EXPECT_NE(f.io.stat(ReplicaStore::tdataPath(key(), f.cfg.cacheDir), &st), 0);

  f.io.reset();
  f.io.dieAt(IoOp::kFdatasync, 1, EIO); // "crash" mid-publish
  EXPECT_LT(f.rs->publish(key(), meta, overlay.data(), overlay.size()), 0);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);
  EXPECT_EQ(f.stats.replicaOpens.load(), 0u);
}

TEST(ReplicaStoreFailOpen, InconsistentMetaRejectedAtPublish) {
  Fixture f;
  auto overlay = test::randomBytes(kP, 47);
  auto meta = sampleMeta(1 << 20, overlay.size());
  meta.extents = {{100, 200, 0}, {150, 100, 200}}; // overlapping
  EXPECT_EQ(f.rs->publish(key(), meta, overlay.data(), overlay.size()), -EINVAL);
}

// ------------------------------------------------------- publish + serve --

TEST(ReplicaStore, PublishOpenMapRead) {
  Fixture f;
  const uint64_t originSize = 1 << 20;
  auto overlay = test::randomBytes(2 * kP + 999, 48);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(originSize, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  EXPECT_EQ(f.stats.replicaPublished.load(), 1u);

  auto v = f.rs->openView(key(), originSize);
  ASSERT_TRUE(v);
  EXPECT_EQ(f.stats.replicaOpens.load(), 1u);
  EXPECT_EQ(v->virtualSize(), originSize + overlay.size());

  // Map across the patched window: original | overlay(16) | original.
  auto segs = v->map(0, 4096);
  ASSERT_EQ(segs.size(), 3u);
  EXPECT_FALSE(segs[0].overlay);
  EXPECT_EQ(segs[0].len, 100u);
  EXPECT_TRUE(segs[1].overlay);
  EXPECT_EQ(segs[1].off, 100u);
  EXPECT_EQ(segs[1].len, 16u);
  EXPECT_EQ(segs[1].tdataOff, 0u);
  EXPECT_FALSE(segs[2].overlay);
  EXPECT_EQ(segs[2].off, 116u);

  // Map inside the extension is pure overlay, with the right tdata offset.
  segs = v->map(originSize + 10, 1000);
  ASSERT_EQ(segs.size(), 1u);
  EXPECT_TRUE(segs[0].overlay);
  EXPECT_EQ(segs[0].tdataOff, 26u); // 16 + 10
  // Reads past virtualSize clamp to empty.
  EXPECT_TRUE(v->map(v->virtualSize(), 100).empty());

  // Overlay reads return the exact published bytes (straddling pages).
  std::vector<uint8_t> got(kP + 200);
  ASSERT_TRUE(v->read(kP - 100, got.size(), got.data()));
  EXPECT_EQ(0, std::memcmp(got.data(), overlay.data() + kP - 100, got.size()));
  // Out-of-bounds overlay read refuses.
  EXPECT_FALSE(v->read(overlay.size() - 10, 20, got.data()));
}

// Overlay reads coalesce too: a span covering many overlay pages costs one
// pread per run, not one per page, with every page's CRC still checked. Same
// currency as the byte tier — read operations, not just bytes.
TEST(ReplicaStore, OverlayReadsCoalesceIntoOnePreadPerRun) {
  Fixture f;
  const uint64_t originSize = 1 << 20;
  auto overlay = test::randomBytes(40 * kP, 77); // 40 pages: 2.5 MiB
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(originSize, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  auto v = f.rs->openView(key(), originSize);
  ASSERT_TRUE(v);

  // 8 pages, unaligned at both ends: ONE pread (the run fits the cap).
  std::vector<uint8_t> got(8 * kP);
  const uint64_t off = 2 * kP + 77, len = 7 * kP + 1000;
  f.stats.replicaReads.store(0);
  f.stats.replicaReadBytes.store(0);
  ASSERT_TRUE(v->read(off, len, got.data()));
  EXPECT_EQ(f.stats.replicaReads.load(), 1u);
  EXPECT_EQ(f.stats.replicaReadBytes.load(), 8u * kP); // whole pages checksummed
  EXPECT_EQ(f.stats.replicaReadSize.b[19].load(), 1u); // 8x64 KiB = 512 KiB, log2 = 19
  EXPECT_EQ(0, std::memcmp(got.data(), overlay.data() + off, len));

  // The cap splits a longer run: 40 pages = 2.5 MiB spans three 1 MiB preads.
  std::vector<uint8_t> all(overlay.size());
  f.stats.replicaReads.store(0);
  ASSERT_TRUE(v->read(0, all.size(), all.data()));
  EXPECT_EQ(f.stats.replicaReads.load(),
            (overlay.size() + kMaxCoalescedRead - 1) / kMaxCoalescedRead);
  // Pin the ceiling exactly: the two full runs moved 1 MiB each.
  EXPECT_EQ(f.stats.replicaReadSize.b[20].load(), 2u); // log2(1 MiB) = 20
  EXPECT_EQ(0, std::memcmp(all.data(), overlay.data(), all.size()));
}

// The founding premise of coalescing — read granularity is independent of
// checksum granularity — has to hold on THIS tier too: every page of a run is
// verified, not just the one the run starts at. Corrupt a page in the MIDDLE of
// a run and read the whole run in one call.
TEST(ReplicaStore, CorruptMiddlePageOfAnOverlayRunIsCaught) {
  Fixture f;
  auto overlay = test::randomBytes(4 * kP, 91);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  const std::string dPath = ReplicaStore::tdataPath(key(), f.cfg.cacheDir);
  struct ::stat st;
  ASSERT_EQ(::stat(dPath.c_str(), &st), 0);
  struct timespec keep[2] = {statAtime(st), statMtime(st)};

  // Page 2 of 4 — not the first page of the run the read below will build.
  int fd = ::open(dPath.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  uint8_t b;
  ASSERT_EQ(::pread(fd, &b, 1, 2 * kP + 17), 1);
  b ^= 0x55;
  ASSERT_EQ(::pwrite(fd, &b, 1, 2 * kP + 17), 1);
  ::close(fd);
  // Restore mtime so the verify-once marker skips the open-time scan: this must
  // be caught by the PER-READ check, inside a coalesced run.
  ASSERT_EQ(::utimensat(AT_FDCWD, dPath.c_str(), keep, 0), 0);

  auto v = f.rs->openView(key(), 1 << 20);
  ASSERT_TRUE(v);
  std::vector<uint8_t> buf(4 * kP);
  EXPECT_FALSE(v->read(0, 4 * kP, buf.data())); // one pread, page 2 rejected
  EXPECT_EQ(f.stats.replicaCrcFailures.load(), 1u);
  EXPECT_TRUE(v->invalid());
}

TEST(ReplicaStore, RepublishReplacesAtomically) {
  Fixture f;
  auto o1 = test::randomBytes(kP, 49);
  auto o2 = test::randomBytes(2 * kP, 50);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, o1.size()), o1.data(), o1.size()), 0);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, o2.size()), o2.data(), o2.size()), 0);
  auto v = f.rs->openView(key(), 1 << 20);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->meta().tdataBytes, o2.size());
  std::vector<uint8_t> got(o2.size());
  ASSERT_TRUE(v->read(0, got.size(), got.data()));
  EXPECT_EQ(0, std::memcmp(got.data(), o2.data(), got.size()));
}

// ------------------------------------------------------ punch-and-clear --

TEST(ReplicaStore, PunchAndClearKeepsV1Semantics) {
  Fixture f;
  const uint64_t size = 64 * 4096;
  auto content = test::randomBytes(size, 51);
  auto e = f.store->open(key(), size);
  ASSERT_TRUE(e);
  e->writePages(0, size, content.data());
  e->flushBuffer(true); // punch reclaims ON-DISK pages; make them durable first
  ASSERT_TRUE(e->hasRange(0, size));

  // Supersede [4096, 4096+8*4096) plus a deliberately unaligned tail range.
  std::vector<ReplicaMeta::Range> ranges = {{4096, 8 * 4096}, {20 * 4096 + 100, 4096}};
  uint64_t punched = f.rs->punchSuperseded(*e, ranges);
  EXPECT_EQ(punched, 8u * 4096u); // unaligned range covers no FULL page
  EXPECT_EQ(f.stats.replicaPunchedBytes.load(), punched);

  // Full pages inside range 1 are absent; edges and the unaligned range stay.
  EXPECT_FALSE(e->hasRange(4096, 8 * 4096));
  EXPECT_TRUE(e->hasRange(0, 4096));
  EXPECT_TRUE(e->hasRange(12 * 4096, 4096));
  EXPECT_TRUE(e->hasRange(20 * 4096, 4096));
  EXPECT_TRUE(e->hasRange(21 * 4096, 4096));

  // readCached on the punched span refuses (absent), the rest verifies.
  std::vector<uint8_t> buf(4096);
  EXPECT_FALSE(e->readCached(4096, 4096, buf.data()));
  EXPECT_TRUE(e->readCached(0, 4096, buf.data()));
  EXPECT_EQ(0, std::memcmp(buf.data(), content.data(), 4096));

  // The flushed sidecar agrees (still v1 format) — a fresh reader refetches.
  auto m = MetaFile::load(f.io, key().metaPath(f.cfg.cacheDir));
  ASSERT_TRUE(m);
  EXPECT_FALSE(m->bitmap.get(1));
  EXPECT_TRUE(m->bitmap.get(0));

  // Disk blocks actually reclaimed (xfs/ext4 support punch; if the test FS
  // didn't, punched would be 0 and this block is skipped).
  if (punched) {
    struct ::stat st;
    ASSERT_EQ(::stat(key().dataPath(f.cfg.cacheDir).c_str(), &st), 0);
    EXPECT_LT(static_cast<uint64_t>(st.st_blocks) * 512, size);
  }
}

TEST(ReplicaStore, PunchUnsupportedStillClearsBits) {
  Fixture f;
  const uint64_t size = 16 * 4096;
  auto content = test::randomBytes(size, 52);
  auto e = f.store->open(key(), size);
  ASSERT_TRUE(e);
  e->writePages(0, size, content.data());
  f.io.failNth(IoOp::kPunchHole, 1, EOPNOTSUPP);
  uint64_t punched = f.rs->punchSuperseded(*e, {{0, 4 * 4096}});
  EXPECT_EQ(punched, 0u); // reclaim best-effort...
  EXPECT_FALSE(e->hasRange(0, 4 * 4096)); // ...but the bits are gone regardless
}

// ---------------------------------------------- eviction + orphan sweep --

TEST(ReplicaStore, EvictionAccountsAndRemovesReplica) {
  Fixture f;
  const uint64_t size = 32 * 4096;
  auto content = test::randomBytes(size, 53);
  {
    auto e = f.store->open(key(), size);
    ASSERT_TRUE(e);
    e->writePages(0, size, content.data());
  } // closed: evictable
  auto overlay = test::randomBytes(2 * kP, 54);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(size, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  EXPECT_EQ(f.store->usageBytes(), size + overlay.size()); // replica counted

  auto ls = f.store->listEntries();
  ASSERT_EQ(ls.size(), 1u);
  EXPECT_EQ(ls[0].replicaBytes, overlay.size());

  // Force a full eviction via a tiny byte cap on a fresh store instance.
  Config evictCfg = f.cfg;
  evictCfg.maxBytes = 1; // everything is over budget
  // Orthogonal to the eviction protection window: the entry was written moments
  // ago, which the shipped 1-day window protects by design. Turn it off so this
  // case still measures replica accounting and removal.
  evictCfg.evictProtectSeconds = 0;
  CacheStore s2(f.io, evictCfg);
  s2.disableStatsDump();
  EXPECT_EQ(s2.evictNow(), 1);
  struct ::stat st;
  EXPECT_NE(f.io.stat(ReplicaStore::tdataPath(key(), f.cfg.cacheDir), &st), 0);
  EXPECT_NE(f.io.stat(ReplicaStore::tmetaPath(key(), f.cfg.cacheDir), &st), 0);
  EXPECT_EQ(f.store->usageBytes(), 0u);
}

TEST(ReplicaStore, OrphanSweepIsAgeGuarded) {
  Fixture f;
  // Orphan replica artifacts with NO .meta sibling.
  const std::string dir = key().objectDir(f.cfg.cacheDir);
  ASSERT_EQ(f.io.mkdirs(dir, 0700), 0);
  const std::string orphanT = dir + "/" + key().hashHex + ".tdata";
  const std::string orphanM = dir + "/" + key().hashHex + ".tmeta";
  for (const auto& p : {orphanT, orphanM}) {
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, "x", 1), 1);
    ::close(fd);
  }
  // Fresh orphans survive the sweep (a publisher may be between renames).
  f.store->evictNow();
  struct ::stat st;
  EXPECT_EQ(f.io.stat(orphanT, &st), 0);
  // Age them past the 1 h guard: swept.
  struct utimbuf old{};
  old.actime = old.modtime = ::time(nullptr) - 7200;
  ASSERT_EQ(::utime(orphanT.c_str(), &old), 0);
  ASSERT_EQ(::utime(orphanM.c_str(), &old), 0);
  f.store->evictNow();
  EXPECT_NE(f.io.stat(orphanT, &st), 0);
  EXPECT_NE(f.io.stat(orphanM, &st), 0);
  EXPECT_EQ(f.store->stats().replicaOrphansSwept.load(), 2u); // sweep runs in the store
}

TEST(ReplicaStore, InvalidateDropsReplica) {
  Fixture f;
  const uint64_t size = 8 * 4096;
  {
    auto e = f.store->open(key(), size);
    ASSERT_TRUE(e);
  }
  auto overlay = test::randomBytes(kP, 55);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(size, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  f.store->invalidate(key());
  struct ::stat st;
  EXPECT_NE(f.io.stat(ReplicaStore::tdataPath(key(), f.cfg.cacheDir), &st), 0);
  EXPECT_NE(f.io.stat(ReplicaStore::tmetaPath(key(), f.cfg.cacheDir), &st), 0);
}

TEST(ReplicaStore, VerifyOnceMarkerSkipsScanButPerReadCrcStays) {
  Fixture f;
  auto overlay = test::randomBytes(2 * kP, 56);
  ASSERT_EQ(f.rs->publish(key(), sampleMeta(1 << 20, overlay.size()), overlay.data(),
                          overlay.size()),
            0);
  const std::string dPath = ReplicaStore::tdataPath(key(), f.cfg.cacheDir);
  const std::string tok = ReplicaStore::tokPath(key(), f.cfg.cacheDir);
  struct ::stat st;
  ASSERT_EQ(::stat(tok.c_str(), &st), 0); // publish wrote the marker
  ASSERT_EQ(::stat(dPath.c_str(), &st), 0);
  struct timespec keep[2] = {statAtime(st), statMtime(st)};

  // Corrupt a page but RESTORE the mtime: the advisory marker then matches,
  // the open-time scan is skipped (that's the optimization), and the
  // per-read CRC — the layer that never turns off — still refuses the page.
  int fd = ::open(dPath.c_str(), O_RDWR);
  uint8_t b;
  ASSERT_EQ(::pread(fd, &b, 1, kP + 9), 1);
  b ^= 4;
  ASSERT_EQ(::pwrite(fd, &b, 1, kP + 9), 1);
  ::close(fd);
  ASSERT_EQ(::utimensat(AT_FDCWD, dPath.c_str(), keep, 0), 0);

  auto v = f.rs->openView(key(), 1 << 20);
  ASSERT_TRUE(v); // scan skipped via marker (mtime+size+crcs match)
  std::vector<uint8_t> buf(64);
  EXPECT_TRUE(v->read(0, 64, buf.data()));            // clean page serves
  EXPECT_FALSE(v->read(kP + 1, 64, buf.data()));      // corrupt page refused
  EXPECT_TRUE(v->invalid());

  // Without the marker the full open-time verify runs and quarantines.
  ASSERT_EQ(::unlink(tok.c_str()), 0);
  EXPECT_EQ(f.rs->openView(key(), 1 << 20), nullptr);
  EXPECT_NE(f.io.stat(dPath, &st), 0); // quarantined
}

// Two replica generations coexist in one cache: an older replica with a
// RAW relocated-metadata key and a newer replica with a
// ROOT-COMPRESSED one. Serving is generation-agnostic — the store treats
// .tdata as opaque CRC'd bytes and .tmeta format_version stays 1 — so both
// open, serve their exact bytes, carry a verify-once marker, and untranspose
// independently. (The generation lives entirely in .tdata content, which the
// store never introspects; this test guards against a regression that would.)
TEST(ReplicaStore, MixedGenerationCoexist) {
  Fixture f;
  const uint64_t size = 1 << 20;
  UrlKey kRaw = *UrlKey::parse("root://origin//data/oldgen.root"); // raw metadata
  UrlKey kZ = *UrlKey::parse("root://origin//data/newgen.root");   // compressed metadata
  auto ovRaw = test::randomBytes(2 * kP + 111, 71);
  auto ovZ = test::randomBytes(kP + 777, 72);
  { auto a = f.store->open(kRaw, size); auto b = f.store->open(kZ, size);
    ASSERT_TRUE(a); ASSERT_TRUE(b); }
  ASSERT_EQ(f.rs->publish(kRaw, sampleMeta(size, ovRaw.size()), ovRaw.data(), ovRaw.size()), 0);
  ASSERT_EQ(f.rs->publish(kZ, sampleMeta(size, ovZ.size()), ovZ.data(), ovZ.size()), 0);

  // verify-once markers written for both generations
  struct ::stat st;
  EXPECT_EQ(f.io.stat(ReplicaStore::tokPath(kRaw, f.cfg.cacheDir), &st), 0);
  EXPECT_EQ(f.io.stat(ReplicaStore::tokPath(kZ, f.cfg.cacheDir), &st), 0);

  // both open and serve their own bytes from the one cache dir
  auto vRaw = f.rs->openView(kRaw, size);
  auto vZ = f.rs->openView(kZ, size);
  ASSERT_TRUE(vRaw);
  ASSERT_TRUE(vZ);
  std::vector<uint8_t> b1(ovRaw.size()), b2(ovZ.size());
  ASSERT_TRUE(vRaw->read(0, b1.size(), b1.data()));
  ASSERT_TRUE(vZ->read(0, b2.size(), b2.data()));
  EXPECT_EQ(0, std::memcmp(b1.data(), ovRaw.data(), b1.size()));
  EXPECT_EQ(0, std::memcmp(b2.data(), ovZ.data(), b2.size()));

  // untranspose one generation; the other keeps serving unaffected
  f.store->invalidate(kRaw);
  EXPECT_NE(f.io.stat(ReplicaStore::tmetaPath(kRaw, f.cfg.cacheDir), &st), 0);
  EXPECT_TRUE(f.rs->openView(kZ, size));
  f.store->invalidate(kZ);
  EXPECT_NE(f.io.stat(ReplicaStore::tmetaPath(kZ, f.cfg.cacheDir), &st), 0);
}

// ------------------------------------------------ torn publish (kill -9) --

TEST(ReplicaCrash, TornPublishNeverServable) {
  TempDir td;
  Config cfg;
  cfg.cacheDir = td.path();
  const uint64_t originSize = 1 << 20;
  const int iters = ::getenv("UCACHE_TEST_QUICK") ? 5 : 30;
  std::mt19937_64 rng(20260705);

  for (int it = 0; it < iters; ++it) {
    const uint64_t n = kP + rng() % (3 * kP);
    pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
      // Child: publish deterministic content, then spin so the parent's
      // kill lands at a random point inside or after publish.
      RealIO io;
      Stats stats;
      ReplicaStore rs(io, cfg, stats);
      auto overlay = test::randomBytes(n, n); // content derived from size
      rs.publish(key(), sampleMeta(originSize, n), overlay.data(), overlay.size());
      for (;;)
        ::usleep(1000);
    }
    ::usleep(rng() % 4000); // 0-4 ms: spans tmp write, fsync, both renames
    ::kill(pid, SIGKILL);
    int st = 0;
    ::waitpid(pid, &st, 0);

    // Parent: whatever survived must be all-or-nothing.
    RealIO io;
    Stats stats;
    ReplicaStore rs(io, cfg, stats);
    auto v = rs.openView(key(), originSize);
    if (v) {
      const uint64_t got = v->meta().tdataBytes;
      auto expect = test::randomBytes(got, got); // same derivation as the child
      std::vector<uint8_t> buf(got);
      ASSERT_TRUE(v->read(0, got, buf.data())) << "iter " << it;
      ASSERT_EQ(0, std::memcmp(buf.data(), expect.data(), got)) << "iter " << it;
    }
    rs.drop(key());
  }
}
