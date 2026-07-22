#include "FileEntry.h"

#include "TestUtil.h"
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <thread>

using namespace ucache;
using test::TempDir;

namespace {

struct Fixture {
  TempDir td;
  RealIO io;
  Config cfg;
  Stats stats;
  UrlKey key = *UrlKey::parse("root://h//data/file.root");
  std::vector<uint8_t> src;

  explicit Fixture(uint64_t fileSize = 100000, uint32_t pageSize = 4096) {
    cfg.cacheDir = td.path();
    cfg.pageSize = pageSize;
    src = test::randomBytes(fileSize, 1234);
  }

  std::shared_ptr<FileEntry> open() {
    return FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  }
};

} // namespace

TEST(FileEntry, FreshWriteReadRoundtrip) {
  Fixture fx;
  auto e = fx.open();
  ASSERT_TRUE(e);
  EXPECT_EQ(e->fileSize(), fx.src.size());
  EXPECT_EQ(e->pageSize(), 4096u);
  EXPECT_FALSE(e->hasRange(0, 100));

  // Persist pages 1..3 (page-aligned span).
  e->writePages(4096, 3 * 4096, fx.src.data() + 4096);
  e->flushBuffer(true); // make durable — this test observes the disk pipeline
  EXPECT_TRUE(e->hasRange(4096, 3 * 4096));
  EXPECT_FALSE(e->hasRange(0, 4096));
  EXPECT_FALSE(e->hasRange(4096, 3 * 4096 + 1));

  std::vector<uint8_t> buf(3 * 4096);
  ASSERT_TRUE(e->readCached(4096, buf.size(), buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 4096, buf.size()));

  // Sub-range crossing page boundaries.
  ASSERT_TRUE(e->readCached(5000, 5000, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 5000, 5000));

  EXPECT_EQ(fx.stats.pageWrites.load(), 3u);
  EXPECT_EQ(fx.stats.hitBytes.load(), 3u * 4096 + 5000);
}

TEST(FileEntry, PartialEdgesNotMarked) {
  Fixture fx;
  auto e = fx.open();
  // Span [100, 4096*2+50): only page 1 is fully contained.
  e->writePages(100, 2 * 4096 - 50, fx.src.data() + 100);
  e->flushBuffer(true);
  EXPECT_FALSE(e->hasRange(0, 10));
  EXPECT_TRUE(e->hasRange(4096, 4096));
  EXPECT_FALSE(e->hasRange(8192, 10));
  EXPECT_EQ(fx.stats.pageWrites.load(), 1u);
}

TEST(FileEntry, TailPage) {
  Fixture fx(100000, 4096); // 25 pages; tail = 1696 bytes
  auto e = fx.open();
  // Persist the tail via a span reaching EOF.
  uint64_t tailStart = 24 * 4096;
  e->writePages(tailStart, fx.src.size() - tailStart, fx.src.data() + tailStart);
  e->flushBuffer(true); // cachedBytes counts published pages only
  EXPECT_TRUE(e->hasRange(tailStart, fx.src.size() - tailStart));
  EXPECT_EQ(e->cachedBytes(), fx.src.size() - tailStart);
  std::vector<uint8_t> buf(2000);
  ASSERT_TRUE(e->readCached(tailStart, fx.src.size() - tailStart, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + tailStart, fx.src.size() - tailStart));
  // Beyond-EOF asks are refused.
  EXPECT_FALSE(e->hasRange(tailStart, 4096));
  EXPECT_FALSE(e->readCached(tailStart, 4096, buf.data()));
}

TEST(FileEntry, CrcCatchesCorruption) {
  Fixture fx;
  auto e = fx.open();
  e->writePages(0, 8192, fx.src.data());
  e->flushBuffer(true); // corruption below targets the on-disk copy
  ASSERT_TRUE(e->hasRange(0, 8192));

  // Flip one byte of page 0 on disk behind the entry's back.
  int fd = ::open(fx.key.dataPath(fx.cfg.cacheDir).c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  uint8_t b;
  ASSERT_EQ(::pread(fd, &b, 1, 100), 1);
  b ^= 0xFF;
  ASSERT_EQ(::pwrite(fd, &b, 1, 100), 1);
  ::close(fd);

  std::vector<uint8_t> buf(8192);
  EXPECT_FALSE(e->readCached(0, 8192, buf.data())); // fail-open to origin
  EXPECT_EQ(fx.stats.crcFailures.load(), 1u);
  EXPECT_FALSE(e->hasRange(0, 4096)); // page 0 now absent
  EXPECT_TRUE(e->hasRange(4096, 4096));

  // Refetch heals it.
  e->writePages(0, 4096, fx.src.data());
  ASSERT_TRUE(e->readCached(0, 8192, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 8192));
}

TEST(FileEntry, ReopenAdoptsAndValidationRebuilds) {
  Fixture fx;
  {
    auto e = fx.open();
    e->writePages(0, 8192, fx.src.data());
  } // dtor flushes meta
  {
    auto e = fx.open();
    ASSERT_TRUE(e);
    EXPECT_TRUE(e->hasRange(0, 8192)); // adopted
    EXPECT_EQ(fx.stats.validationsFailed.load(), 0u);
    std::vector<uint8_t> buf(8192);
    ASSERT_TRUE(e->readCached(0, 8192, buf.data()));
    EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 8192));
  }
  {
    // Origin size changed -> validation fails -> fresh entry.
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size() + 1, 0,
                             MetaData::kCksumNone, 0);
    ASSERT_TRUE(e);
    EXPECT_EQ(fx.stats.validationsFailed.load(), 1u);
    EXPECT_FALSE(e->hasRange(0, 4096));
    EXPECT_EQ(e->fileSize(), fx.src.size() + 1);
  }
}

TEST(FileEntry, MtimeAndCksumValidation) {
  Fixture fx;
  fx.cfg.validate = ValidateMode::kSizeMtime;
  {
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size(), 777,
                             MetaData::kCksumNone, 0);
    e->writePages(0, 4096, fx.src.data());
  }
  {
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size(), 777,
                             MetaData::kCksumNone, 0);
    EXPECT_TRUE(e->hasRange(0, 4096)); // same mtime adopts
  }
  {
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size(), 778,
                             MetaData::kCksumNone, 0);
    EXPECT_FALSE(e->hasRange(0, 4096)); // mtime skew rebuilds
    EXPECT_EQ(fx.stats.validationsFailed.load(), 1u);
  }
  fx.cfg.validate = ValidateMode::kCksum;
  {
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size(), 0,
                             MetaData::kCksumAdler32, 0xAA);
    e->writePages(0, 4096, fx.src.data());
  }
  {
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size(), 0,
                             MetaData::kCksumAdler32, 0xBB);
    EXPECT_FALSE(e->hasRange(0, 4096)); // checksum mismatch rebuilds
  }
}

TEST(FileEntry, CksumModeWithoutAChecksumValidatesSizeOnly) {
  // Characterization: the cksum comparison is gated on a
  // caller-provided kind, and the PLUGIN never queries an origin checksum —
  // it always passes kCksumNone. In cksum mode that skips the mtime check
  // too, so validate=cksum currently degrades to size-only end-to-end.
  // Documented in USER_GUIDE; a real origin-checksum source is future work.
  Fixture fx;
  fx.cfg.validate = ValidateMode::kCksum;
  {
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size(), 777,
                             MetaData::kCksumNone, 0);
    e->writePages(0, 4096, fx.src.data());
  }
  {
    // Different mtime, no checksum kind: still adopted (size matched).
    auto e = FileEntry::open(fx.io, fx.cfg, fx.stats, fx.key, fx.src.size(), 778,
                             MetaData::kCksumNone, 0);
    EXPECT_TRUE(e->hasRange(0, 4096));
    EXPECT_EQ(fx.stats.validationsFailed.load(), 0u);
  }
}

TEST(FileEntry, CorruptSidecarStartsFresh) {
  Fixture fx;
  {
    auto e = fx.open();
    e->writePages(0, 4096, fx.src.data());
  }
  // Corrupt the sidecar.
  std::string mp = fx.key.metaPath(fx.cfg.cacheDir);
  int fd = ::open(mp.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  uint8_t junk = 0x5A;
  ASSERT_EQ(::pwrite(fd, &junk, 1, 60), 1);
  ::close(fd);
  auto e = fx.open();
  ASSERT_TRUE(e);
  EXPECT_EQ(fx.stats.metaCorrupt.load(), 1u);
  EXPECT_FALSE(e->hasRange(0, 4096));
}

TEST(FileEntry, AdoptedPageSizeWinsOverConfig) {
  Fixture fx;
  {
    auto e = fx.open();
    e->writePages(0, 8192, fx.src.data());
  }
  fx.cfg.pageSize = 65536; // config changed; entry page size is fixed
  auto e = fx.open();
  ASSERT_TRUE(e);
  EXPECT_EQ(e->pageSize(), 4096u);
  EXPECT_TRUE(e->hasRange(0, 8192));
}

TEST(FileEntry, IdempotentWritesSkipIo) {
  Fixture fx;
  auto e = fx.open();
  e->writePages(0, 8192, fx.src.data());
  e->flushBuffer(true);
  EXPECT_EQ(fx.stats.pageWrites.load(), 2u);
  e->writePages(0, 8192, fx.src.data()); // already present: no new page writes
  e->flushBuffer(true);
  EXPECT_EQ(fx.stats.pageWrites.load(), 2u);
}

TEST(FileEntry, UnlinkedWhileOpenServesButNeverResurrects) {
  Fixture fx;
  auto e = fx.open();
  e->writePages(0, 8192, fx.src.data());
  e->flushMeta(true);
  ASSERT_EQ(::unlink(fx.key.dataPath(fx.cfg.cacheDir).c_str()), 0);
  ASSERT_EQ(::unlink(fx.key.metaPath(fx.cfg.cacheDir).c_str()), 0);
  // Still serves through the open fd (POSIX).
  std::vector<uint8_t> buf(8192);
  ASSERT_TRUE(e->readCached(0, 8192, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 8192));
  // New writes + flush must not recreate the sidecar.
  e->writePages(8192, 4096, fx.src.data() + 8192);
  e->flushMeta(true);
  struct stat st;
  EXPECT_LT(::stat(fx.key.metaPath(fx.cfg.cacheDir).c_str(), &st), 0);
}

TEST(FileEntry, PinnedFlagPersists) {
  Fixture fx;
  {
    auto e = fx.open();
    EXPECT_FALSE(e->pinned());
    e->setPinned(true);
  }
  auto e = fx.open();
  EXPECT_TRUE(e->pinned());
  e->setPinned(false);
  EXPECT_FALSE(e->pinned());
}

TEST(FileEntry, VerifyAllScrub) {
  Fixture fx;
  auto e = fx.open();
  e->writePages(0, 12288, fx.src.data());
  e->flushBuffer(true); // scrub verifies the on-disk pipeline
  auto r = e->verifyAll();
  EXPECT_EQ(r.checked, 3u);
  EXPECT_EQ(r.bad, 0u);
  // Corrupt page 1 on disk; scrub must find and quarantine exactly it.
  int fd = ::open(fx.key.dataPath(fx.cfg.cacheDir).c_str(), O_RDWR);
  uint8_t b = 0;
  ASSERT_EQ(::pwrite(fd, &b, 1, 4096 + 7), 1);
  ::close(fd);
  // (If the byte happened to already be 0, rewrite with 1.)
  r = e->verifyAll();
  if (r.bad == 0) {
    b = 1;
    fd = ::open(fx.key.dataPath(fx.cfg.cacheDir).c_str(), O_RDWR);
    ASSERT_EQ(::pwrite(fd, &b, 1, 4096 + 7), 1);
    ::close(fd);
    r = e->verifyAll();
  }
  EXPECT_EQ(r.bad, 1u);
  EXPECT_FALSE(e->hasRange(4096, 1));
  EXPECT_TRUE(e->hasRange(0, 4096));
}

// Flush policy: exceeding fill_buffer_mb drains the stage on the write
// path itself — no explicit flush needed for a big fill to become durable.
TEST(FileEntry, BufferCapTriggersSelfFlush) {
  Fixture fx(4 * 1024 * 1024, 4096); // 4 MiB file
  fx.cfg.fillBufferMb = 1;           // 1 MiB cap
  auto e = fx.open();
  ASSERT_TRUE(e);
  std::vector<uint8_t> chunk(2 * 1024 * 1024);
  for (size_t i = 0; i < chunk.size(); ++i)
    chunk[i] = static_cast<uint8_t>(i * 31);
  e->writePages(0, chunk.size(), chunk.data()); // 2 MiB > cap: must self-flush
  EXPECT_GE(fx.stats.pageWrites.load(), 256u);  // >= 1 MiB published
  // Everything readable regardless of which pages are still staged.
  std::vector<uint8_t> buf(chunk.size());
  ASSERT_TRUE(e->readCached(0, chunk.size(), buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), chunk.data(), chunk.size()));
}

// Fill-buffer concurrency torture (TSan target): concurrent stagers, readers, and a
// forced flusher; final state must be complete and byte-correct.
TEST(FileEntry, ConcurrentStageReadFlush) {
  Fixture fx(256 * 4096, 4096); // 1 MiB, 256 pages
  auto e = fx.open();
  ASSERT_TRUE(e);
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int w = 0; w < 4; ++w)
    ts.emplace_back([&, w] {
      for (int r = 0; r < 200; ++r) {
        uint64_t page = (w * 200 + r * 7) % 250;
        e->writePages(page * 4096, 2 * 4096, fx.src.data() + page * 4096);
      }
    });
  for (int rd = 0; rd < 4; ++rd)
    ts.emplace_back([&] {
      std::vector<uint8_t> buf(3 * 4096);
      while (!stop.load(std::memory_order_relaxed)) {
        uint64_t page = static_cast<uint64_t>(::rand()) % 250;
        if (e->readCached(page * 4096, 4096, buf.data())) {
          ASSERT_EQ(0, memcmp(buf.data(), fx.src.data() + page * 4096, 4096));
        }
      }
    });
  ts.emplace_back([&] {
    for (int i = 0; i < 50; ++i)
      e->flushBuffer(true);
  });
  for (size_t i = 0; i < 4; ++i)
    ts[i].join();
  stop = true;
  for (size_t i = 4; i < ts.size(); ++i)
    ts[i].join();
  e->flushAll();
  std::vector<uint8_t> buf(4096);
  for (uint64_t p = 0; p < 250; ++p) {
    ASSERT_TRUE(e->readCached(p * 4096, 4096, buf.data())) << "page " << p;
    EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + p * 4096, 4096));
  }
}

// The serve-side counters must distinguish access geometry —
// disk vs RAM tier, sequential vs scattered, first-touch vs re-read. This is
// the unit half of the workflow-distinguishability gate.
TEST(FileEntry, ObservabilityCountersTrackServeGeometry) {
  Fixture fx(64 * 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 16 * 4096, fx.src.data());
  e->flushBuffer(true); // on disk: the reads below are the DISK tier

  std::vector<uint8_t> buf(16 * 4096);
  // Sequential full read: 16 preads, 15 at the previous read's end.
  ASSERT_TRUE(e->readCached(0, 16 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 16u);
  EXPECT_EQ(fx.stats.hitDiskBytes.load(), 16u * 4096);
  EXPECT_EQ(fx.stats.hitDiskSeq.load(), 15u);
  EXPECT_EQ(fx.stats.firstTouchBytes.load(), 16u * 4096);
  EXPECT_EQ(fx.stats.ramHitBytes.load(), 0u);

  // Re-read the same range: disk reads double, first-touch does NOT move —
  // the re-read factor (served/first_touch) now reads 2x.
  ASSERT_TRUE(e->readCached(0, 16 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 32u);
  EXPECT_EQ(fx.stats.firstTouchBytes.load(), 16u * 4096);
  EXPECT_EQ(fx.stats.hitBytes.load(), 2u * 16 * 4096);

  // Scattered single-page reads (stride 2): none sequential beyond the first
  // pair boundary — seq counter must not advance.
  uint64_t seqBefore = fx.stats.hitDiskSeq.load();
  for (int i = 0; i < 16; i += 2)
    ASSERT_TRUE(e->readCached(static_cast<uint64_t>(i) * 4096, 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskSeq.load(), seqBefore);

  // Staged pages (not yet flushed) serve from RAM: ram_hit_bytes counts,
  // disk read count does not move.
  e->writePages(32 * 4096, 4 * 4096, fx.src.data() + 32 * 4096);
  uint64_t diskBefore = fx.stats.hitDiskReads.load();
  ASSERT_TRUE(e->readCached(32 * 4096, 4 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), diskBefore);
  EXPECT_EQ(fx.stats.ramHitBytes.load(), 4u * 4096);
  EXPECT_EQ(fx.stats.firstTouchBytes.load(), 20u * 4096);

  // Per-entry mirror (Layer 2 record source) agrees.
  EXPECT_EQ(e->obs().ramBytes.load(), 4u * 4096);
  EXPECT_EQ(e->obs().firstTouchBytes.load(), 20u * 4096);
  EXPECT_EQ(e->obs().wireBytes.load(), 20u * 4096);
}

// Write side: coalesced flush runs are counted with their sizes, and
// a cap-triggered drain is a "stall" (wall time the fill thread lost).
TEST(FileEntry, ObservabilityCountersTrackFlushRuns) {
  Fixture fx(64 * 4096);
  fx.cfg.fillBufferMb = 1; // 1 MiB cap
  auto e = fx.open();
  ASSERT_TRUE(e);
  // Two disjoint 8-page spans staged, one forced flush: 2 runs, 16 pages.
  e->writePages(0, 8 * 4096, fx.src.data());
  e->writePages(32 * 4096, 8 * 4096, fx.src.data() + 32 * 4096);
  e->flushBuffer(true);
  EXPECT_EQ(fx.stats.flushRuns.load(), 2u);
  EXPECT_EQ(fx.stats.flushRunBytes.load(), 16u * 4096);
  EXPECT_EQ(fx.stats.bufferStalls.load(), 0u); // forced, not cap-triggered

  // Push past the 1 MiB per-entry cap: writePages self-flushes = a stall.
  std::vector<uint8_t> big(2 << 20);
  for (size_t i = 0; i < big.size(); ++i)
    big[i] = static_cast<uint8_t>(i);
  Fixture fx2(8 << 20);
  fx2.cfg.fillBufferMb = 1;
  auto e2 = fx2.open();
  ASSERT_TRUE(e2);
  e2->writePages(0, big.size(), big.data());
  EXPECT_GE(fx2.stats.bufferStalls.load(), 1u);
}
