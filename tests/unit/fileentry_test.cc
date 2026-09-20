#include "FileEntry.h"

#include "TestUtil.h"
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
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
  // A contiguous run of resident pages costs ONE pread, whatever its page
  // count: read granularity is independent of checksum granularity.
  ASSERT_TRUE(e->readCached(0, 16 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 1u);
  EXPECT_EQ(fx.stats.hitDiskBytes.load(), 16u * 4096); // same bytes as before
  EXPECT_EQ(fx.stats.hitDiskSeq.load(), 0u);           // nothing preceded it
  EXPECT_EQ(fx.stats.firstTouchBytes.load(), 16u * 4096);
  EXPECT_EQ(fx.stats.ramHitBytes.load(), 0u);

  // Re-read the same range: one more pread. It rewinds to offset 0 rather
  // than continuing from the previous read's end, so it is not sequential;
  // first-touch does NOT move — the re-read factor (served/first_touch) now
  // reads 2x.
  ASSERT_TRUE(e->readCached(0, 16 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 2u);
  EXPECT_EQ(fx.stats.hitDiskSeq.load(), 0u);
  EXPECT_EQ(fx.stats.firstTouchBytes.load(), 16u * 4096);
  EXPECT_EQ(fx.stats.hitBytes.load(), 2u * 16 * 4096);

  // Scattered single-page reads (stride 2): one pread each, none sequential —
  // there is nothing to coalesce, so the op count is the page count.
  uint64_t readsBefore = fx.stats.hitDiskReads.load();
  uint64_t seqBefore = fx.stats.hitDiskSeq.load();
  for (int i = 0; i < 16; i += 2)
    ASSERT_TRUE(e->readCached(static_cast<uint64_t>(i) * 4096, 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), readsBefore + 8u);
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

// Hit-path coalescing: ONE pread per contiguous run of resident pages, with
// every page still verified against its own CRC. The op count is the point —
// on op-priced storage it is the currency — so it is asserted directly, and
// the served bytes must be byte-identical to the source either way.
TEST(FileEntry, HitReadsCoalesceIntoOnePreadPerRun) {
  Fixture fx(64 * 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 64 * 4096, fx.src.data());
  e->flushBuffer(true);

  // Unaligned request spanning 11 pages (the readv-chunk shape: partial page
  // at each end) — one pread, and the caller gets exactly its bytes.
  std::vector<uint8_t> buf(11 * 4096);
  const uint64_t off = 3 * 4096 + 100, len = 10 * 4096 + 3000;
  ASSERT_TRUE(e->readCached(off, len, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 1u);
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + off, len));
  // The pread covered whole pages: 11 of them, more bytes than were asked for.
  EXPECT_EQ(fx.stats.hitDiskBytes.load(), 11u * 4096);
  // …and the size histogram was actually fed: 11 pages = 45,056 B -> log2 bucket
  // 15 (32-64 KiB). Without this the histogram can be unwired silently.
  EXPECT_EQ(fx.stats.hitReadSize.b[15].load(), 1u);

  // Page-aligned request: read straight into the caller's buffer, still one op.
  ASSERT_TRUE(e->readCached(8 * 4096, 4 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 2u);
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 8 * 4096, 4 * 4096));

  // A hole splits the run: punch page 4 out of an 8-page span and the request
  // misses (page-granular presence is unchanged by coalescing).
  e->releaseRanges({{4 * 4096, 4096}});
  EXPECT_FALSE(e->readCached(0, 8 * 4096, buf.data()));
}

// A coalesced pread must land STRICTLY inside the caller's buffer. The run is
// read whole-pages-wide, so the bound that keeps it inside the request is the
// only thing standing between "one big read" and a heap overflow — and slack in
// a test buffer hides exactly that. Every read here goes into a buffer sized to
// the request EXACTLY, with a canary immediately after it, across all four
// alignment shapes.
TEST(FileEntry, CoalescedReadStaysInsideTheCallersBuffer) {
  Fixture fx(64 * 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 64 * 4096, fx.src.data());
  e->flushAll();

  struct Shape {
    const char* name;
    uint64_t off, len;
  };
  const Shape shapes[] = {
      {"aligned/aligned", 4 * 4096, 8 * 4096},
      {"aligned/unaligned", 4 * 4096, 8 * 4096 + 1000},
      {"unaligned/aligned", 4 * 4096 + 500, 8 * 4096 - 500},
      {"unaligned/unaligned", 4 * 4096 + 500, 8 * 4096 + 1234},
      {"single partial page", 4096 + 100, 200},
      {"crosses the short tail page", 60 * 4096 + 7, 4 * 4096 - 7},
  };
  for (const Shape& s : shapes) {
    // (1) EXACTLY len bytes on the heap — no slack, no spare capacity — so a
    // sanitizer build poisons the very next byte. `vector::resize` down would
    // not do: it keeps capacity, and an overflow into capacity is invisible.
    std::unique_ptr<uint8_t[]> exact(new uint8_t[s.len]);
    ASSERT_TRUE(e->readCached(s.off, s.len, exact.get())) << s.name;
    EXPECT_EQ(0, memcmp(exact.get(), fx.src.data() + s.off, s.len)) << s.name;

    // (2) the same read into a guarded buffer, so a plain build catches it too.
    std::vector<uint8_t> guarded(s.len + 16, 0xAB);
    ASSERT_TRUE(e->readCached(s.off, s.len, guarded.data())) << s.name;
    for (size_t i = s.len; i < guarded.size(); ++i)
      ASSERT_EQ(guarded[i], 0xAB) << s.name << ": wrote " << (i - s.len + 1)
                                  << " byte(s) past the caller's buffer";
  }
}

// The coalescing cap splits a long run: a 3 MiB contiguous read cannot be one
// pread, and the pieces must be cap-sized (the byte tier's own cap arithmetic —
// the replica tier has a separate test for its side).
TEST(FileEntry, CoalescedRunsAreCappedAtOneMiB) {
  const uint64_t kCap = 1u << 20;
  Fixture fx(3 * kCap);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 3 * kCap, fx.src.data());
  e->flushAll();

  std::vector<uint8_t> buf(3 * kCap);
  ASSERT_TRUE(e->readCached(0, 3 * kCap, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 3u); // not 1, not 768
  EXPECT_EQ(fx.stats.hitDiskBytes.load(), 3u * kCap);
  // Pin the ceiling EXACTLY: 3 reads of 3 MiB means each was 1 MiB, so a cap
  // one page too large or a < / <= slip in the comparison is caught.
  EXPECT_EQ(fx.stats.hitDiskBytes.load() / fx.stats.hitDiskReads.load(), kCap);
  EXPECT_EQ(fx.stats.hitReadSize.b[20].load(), 3u); // log2(1 MiB) = 20
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 3 * kCap));
}

// A truncated .data file must cost only the pages that were really lost. A
// coalesced read cannot tell which page came up short, so it re-reads the run
// page by page rather than discarding the whole megabyte — the pre-coalescing
// blast radius, and one crc_failure per genuinely bad page.
TEST(FileEntry, ShortReadDemotesOnlyThePagesItLost) {
  Fixture fx(16 * 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 16 * 4096, fx.src.data());
  e->flushAll();
  e.reset();

  // Truncate the .data file after page 9: pages 10-15 are gone.
  const std::string dataPath = fx.key.objectDir(fx.cfg.cacheDir) + "/" + fx.key.hashHex + ".data";
  ASSERT_EQ(0, ::truncate(dataPath.c_str(), 10 * 4096));

  auto e2 = fx.open();
  ASSERT_TRUE(e2);
  std::vector<uint8_t> buf(16 * 4096);
  EXPECT_FALSE(e2->readCached(0, 16 * 4096, buf.data()));
  // Pages 0-9 survive and still serve; only the 6 lost pages were demoted.
  EXPECT_TRUE(e2->hasRange(0, 10 * 4096));
  EXPECT_FALSE(e2->hasRange(10 * 4096, 4096));
  EXPECT_EQ(fx.stats.crcFailures.load(), 6u); // one per lost page, not one per run
  EXPECT_TRUE(e2->readCached(0, 10 * 4096, buf.data()));
}

// A request that fails must not consume first-touch attribution for bytes it
// never served: the refetch that follows is what serves them first.
TEST(FileEntry, FailedReadDoesNotConsumeFirstTouch) {
  Fixture fx(16 * 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 8 * 4096, fx.src.data());
  e->flushAll();
  e.reset();

  // Corrupt page 3 of the 8-page run.
  const std::string dataPath = fx.key.objectDir(fx.cfg.cacheDir) + "/" + fx.key.hashHex + ".data";
  int fd = ::open(dataPath.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  uint8_t b = 0;
  ASSERT_EQ(1, ::pread(fd, &b, 1, 3 * 4096 + 11));
  b ^= 0xff;
  ASSERT_EQ(1, ::pwrite(fd, &b, 1, 3 * 4096 + 11));
  ::close(fd);

  auto e2 = fx.open();
  ASSERT_TRUE(e2);
  std::vector<uint8_t> buf(8 * 4096);
  EXPECT_FALSE(e2->readCached(0, 8 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.firstTouchBytes.load(), 0u); // nothing was served
  // The surviving pages are first-touched by the read that actually serves them.
  ASSERT_TRUE(e2->readCached(0, 3 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.firstTouchBytes.load(), 3u * 4096);
}

// Two runs separated by a RAM-staged page: the disk pages before and after it
// cannot be one pread, and the staged page must not be read from disk at all.
TEST(FileEntry, StagedPageSplitsACoalescedRun) {
  Fixture fx(64 * 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 8 * 4096, fx.src.data());
  e->flushBuffer(true);        // pages 0-7 on disk
  e->releaseRanges({{3 * 4096, 4096}}); // page 3 gone from disk
  e->writePages(3 * 4096, 4096, fx.src.data() + 3 * 4096); // page 3 staged in RAM

  std::vector<uint8_t> buf(8 * 4096);
  ASSERT_TRUE(e->readCached(0, 8 * 4096, buf.data()));
  EXPECT_EQ(fx.stats.hitDiskReads.load(), 2u);          // [0-2] and [4-7]
  EXPECT_EQ(fx.stats.hitDiskBytes.load(), 7u * 4096);   // page 3 came from RAM
  EXPECT_EQ(fx.stats.ramHitBytes.load(), 4096u);
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), 8 * 4096));
}

// A corrupted page inside a coalesced run must be caught and demoted — the
// whole point of verifying each page's CRC out of the shared buffer.
TEST(FileEntry, CorruptPageInsideARunIsCaughtAndDemoted) {
  Fixture fx(64 * 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 8 * 4096, fx.src.data());
  e->flushAll();
  e.reset(); // closes the entry: the .data file is complete on disk

  // Flip a byte in the middle page of the run, behind the cache's back.
  const std::string dataPath = fx.key.objectDir(fx.cfg.cacheDir) + "/" + fx.key.hashHex + ".data";
  int fd = ::open(dataPath.c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  uint8_t b = 0;
  ASSERT_EQ(1, ::pread(fd, &b, 1, 5 * 4096 + 7));
  b ^= 0xff;
  ASSERT_EQ(1, ::pwrite(fd, &b, 1, 5 * 4096 + 7));
  ::close(fd);

  auto e2 = fx.open();
  ASSERT_TRUE(e2);
  std::vector<uint8_t> buf(8 * 4096);
  EXPECT_FALSE(e2->readCached(0, 8 * 4096, buf.data())); // run read, page 5 rejected
  EXPECT_EQ(fx.stats.crcFailures.load(), 1u);
  // Only the bad page was demoted: its neighbours still serve.
  EXPECT_TRUE(e2->readCached(0, 5 * 4096, buf.data()));
  EXPECT_TRUE(e2->readCached(6 * 4096, 2 * 4096, buf.data()));
  EXPECT_FALSE(e2->hasRange(5 * 4096, 4096));
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

TEST(FileEntry, AStoreOutOfRoomMeansNoSignatureNotAPrivateOne) {
  // The store hands over a shared footprint so that a file opened twice
  // accumulates into one. When its table is full it hands over NOTHING, and
  // its contract is that such a file records no signature at all -- because a
  // signature that stopped growing would be a confident wrong answer.
  //
  // The entry used to quietly fall back to its own private footprint, which
  // has the entry's lifetime rather than the process's. An application that
  // opens each file twice (ROOT does) then produced two half-footprints and
  // emitted both as confident signatures: precisely the answer the cap exists
  // to avoid, with the two headers each describing a different behaviour.
  Fixture fx;
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->useSharedFootprint(nullptr); // the store declining
  e->noteRead(0, 4096);
  e->noteRead(8192, 4096);
  EXPECT_TRUE(e->footprint().poisoned());
  EXPECT_TRUE(e->footprint().sig(fx.src.size()).empty())
      << "declining to track a file must mean no evidence, not partial evidence";
  EXPECT_EQ(e->footprint().count(fx.src.size()), 0u);
}

TEST(FileEntry, AnEntryGivenARealFootprintStillRecordsNormally) {
  // The control for the leg above: the fallback is poisoned only when the
  // store declines.
  Fixture fx;
  auto e = fx.open();
  ASSERT_TRUE(e);
  ucache::ReadFootprint shared;
  e->useSharedFootprint(&shared);
  e->noteRead(0, 4096);
  EXPECT_FALSE(e->footprint().poisoned());
  EXPECT_FALSE(e->footprint().sig(fx.src.size()).empty());
  EXPECT_EQ(shared.count(fx.src.size()), 1u) << "and it went to the shared one";
}

TEST(FileEntry, AnEntryWithNoSharedFootprintOfEitherKindStillRecords) {
  // The case that actually observes the PRIVATE footprint: an entry that was
  // never handed a shared one. That is how every test here which builds an
  // entry directly gets a signature, and how the entry behaves before the
  // store hands one over.
  //
  // Worth being exact about what this pair does and does not catch, because
  // the leg above used to claim more than it delivered. Poisoning the ACTIVE
  // footprint unconditionally fails these. Poisoning the private fallback
  // unconditionally does NOT -- and that mutation is harmless for the same
  // reason it is invisible: when a shared footprint is present the private one
  // is never consulted. A test cannot catch a change that changes nothing.
  Fixture fx;
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->noteRead(0, 4096);
  EXPECT_FALSE(e->footprint().poisoned())
      << "nothing declined here, so nothing may be poisoned";
  EXPECT_FALSE(e->footprint().sig(fx.src.size()).empty());
  EXPECT_EQ(e->footprint().count(fx.src.size()), 1u);
}

// ---- Several handles on one entry: several processes sharing a cache dir ----
//
// FileEntry keeps no per-process state outside the object, so two handles in
// one process ARE two processes as far as the sidecar is concerned: each has
// its own view, and the .data flock serialises them the same way. Until the
// commit rule existed, the last handle to store decided what the entry held,
// and everyone else's pages sat in .data with no bit and no CRC.

namespace {
// The bytes actually in .data at [off, off+len) -- a truncated span reads as
// zeros, whatever the bitmap claims.
bool rawDataMatches(const Fixture& fx, uint64_t off, uint64_t len) {
  int fd = ::open(fx.key.dataPath(fx.cfg.cacheDir).c_str(), O_RDONLY);
  if (fd < 0)
    return false;
  std::vector<uint8_t> buf(len);
  const bool ok = ::pread(fd, buf.data(), len, static_cast<off_t>(off)) ==
                      static_cast<ssize_t>(len) &&
                  memcmp(buf.data(), fx.src.data() + off, len) == 0;
  ::close(fd);
  return ok;
}
} // namespace

TEST(FileEntry, DisjointFillsFromTwoHandlesBothSurvive) {
  Fixture fx;
  auto a = fx.open();
  auto b = fx.open(); // both hold the entry before either has stored anything
  a->writePages(4096, 3 * 4096, fx.src.data() + 4096);   // pages 1..3
  a->flushAll();
  b->writePages(40960, 3 * 4096, fx.src.data() + 40960); // pages 10..12
  b->flushAll(); // used to store B's view whole and drop A's pages
  a.reset();
  b.reset();
  auto c = fx.open();
  ASSERT_TRUE(c);
  EXPECT_TRUE(c->hasRange(4096, 3 * 4096));
  EXPECT_TRUE(c->hasRange(40960, 3 * 4096));
  EXPECT_EQ(c->cachedBytes(), 6u * 4096);
  std::vector<uint8_t> buf(3 * 4096);
  ASSERT_TRUE(c->readCached(4096, buf.size(), buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 4096, buf.size()));
  ASSERT_TRUE(c->readCached(40960, buf.size(), buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 40960, buf.size()));
  EXPECT_EQ(fx.stats.crcFailures.load(), 0u);
}

TEST(FileEntry, ThreeHandlesCommitInAnyOrder) {
  Fixture fx(100000, 4096); // 25 pages
  auto a = fx.open();
  auto b = fx.open();
  auto c = fx.open();
  a->writePages(0, 4 * 4096, fx.src.data());                     // 0..3
  b->writePages(8 * 4096, 4 * 4096, fx.src.data() + 8 * 4096);   // 8..11
  c->writePages(16 * 4096, 4 * 4096, fx.src.data() + 16 * 4096); // 16..19
  c->flushAll();
  a->flushAll();
  b->flushAll();
  a.reset();
  b.reset();
  c.reset();
  auto d = fx.open();
  EXPECT_EQ(d->cachedBytes(), 12u * 4096);
  EXPECT_TRUE(d->hasRange(0, 4 * 4096));
  EXPECT_TRUE(d->hasRange(8 * 4096, 4 * 4096));
  EXPECT_TRUE(d->hasRange(16 * 4096, 4 * 4096));
}

TEST(FileEntry, CommitAdoptsSiblingPages) {
  Fixture fx;
  auto a = fx.open();
  auto b = fx.open();
  b->writePages(40960, 3 * 4096, fx.src.data() + 40960); // pages 10..12
  b->flushAll();
  EXPECT_FALSE(a->hasRange(40960, 4096)); // A has not looked since it opened
  a->writePages(0, 4096, fx.src.data());  // something of A's own to commit
  a->flushAll();                          // the commit re-reads the image and adopts B's pages
  EXPECT_TRUE(a->hasRange(40960, 3 * 4096));
  std::vector<uint8_t> buf(3 * 4096);
  ASSERT_TRUE(a->readCached(40960, buf.size(), buf.data())); // CRC-verified like any page
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 40960, buf.size()));
  // The idempotent-write skip now covers B's pages too: no second write.
  const uint64_t before = fx.stats.pageWrites.load();
  a->writePages(40960, 3 * 4096, fx.src.data() + 40960);
  a->flushBuffer(true);
  EXPECT_EQ(fx.stats.pageWrites.load(), before);
}

TEST(FileEntry, ClearedPageIsNotResurrectedBySiblingsStaleView) {
  Fixture fx;
  auto a = fx.open();
  auto b = fx.open();
  a->writePages(0, 4 * 4096, fx.src.data()); // pages 0..3
  a->flushAll();
  b->writePages(5 * 4096, 4096, fx.src.data() + 5 * 4096); // page 5
  b->flushAll();                                            // B now holds 0..3 as well
  ASSERT_TRUE(b->hasRange(0, 4 * 4096));
  // A punches 0..3: bits cleared and committed, then the bytes go.
  a->releaseRanges({{0, 4 * 4096}});
  EXPECT_FALSE(a->hasRange(0, 4096));
  // B commits again from a view that still has 0..3 set. A union of views
  // would put those bits back over the hole; a delta commit does not -- and
  // B's own view follows the disk.
  b->writePages(6 * 4096, 4096, fx.src.data() + 6 * 4096); // page 6
  b->flushAll();
  EXPECT_FALSE(b->hasRange(0, 4096));
  a.reset();
  b.reset();
  auto c = fx.open();
  EXPECT_FALSE(c->hasRange(0, 4 * 4096));
  EXPECT_TRUE(c->hasRange(5 * 4096, 2 * 4096));
  EXPECT_EQ(c->cachedBytes(), 2u * 4096);
  EXPECT_EQ(fx.stats.crcFailures.load(), 0u);
}

TEST(FileEntry, PinSetThroughOneHandleSurvivesAnothersCommit) {
  Fixture fx;
  auto a = fx.open();
  auto b = fx.open();
  b->setPinned(true); // stores at once
  a->writePages(0, 4096, fx.src.data());
  a->flushAll(); // A never touched the pin: the on-disk value stands, and A adopts it
  EXPECT_TRUE(a->pinned());
  a.reset();
  b.reset();
  auto c = fx.open();
  EXPECT_TRUE(c->pinned());
  EXPECT_TRUE(c->hasRange(0, 4096));
  // An explicit unpin through one handle wins over a sibling's stale pinned view.
  auto d = fx.open();
  c->setPinned(false);
  d->writePages(4096, 4096, fx.src.data() + 4096);
  d->flushAll();
  c.reset();
  d.reset();
  EXPECT_FALSE(fx.open()->pinned());
}

TEST(FileEntry, FreshOpenDoesNotTruncateSiblingsUnpublishedPages) {
  Fixture fx;
  fx.cfg.fillBufferMb = 0; // bytes land at once; the sidecar waits for the flush interval
  auto a = fx.open();
  a->writePages(0, 4 * 4096, fx.src.data());
  ASSERT_TRUE(rawDataMatches(fx, 0, 4 * 4096));
  auto b = fx.open(); // a second process arriving mid-fill
  EXPECT_TRUE(rawDataMatches(fx, 0, 4 * 4096)) << "the second open truncated the first's pages";
  b.reset();
  a->flushAll();
  a.reset();
  auto c = fx.open();
  std::vector<uint8_t> buf(4 * 4096);
  ASSERT_TRUE(c->readCached(0, buf.size(), buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data(), buf.size()));
  EXPECT_EQ(fx.stats.crcFailures.load(), 0u);
  EXPECT_EQ(fx.stats.validationsFailed.load(), 0u);
}

TEST(FileEntry, FreshOpenStoresTheSidecarAtOnce) {
  Fixture fx;
  auto e = fx.open();
  struct stat st;
  EXPECT_EQ(::stat(fx.key.metaPath(fx.cfg.cacheDir).c_str(), &st), 0);
  // ... and a sibling opening now adopts it: no validation failure, no reset.
  auto f = fx.open();
  ASSERT_TRUE(f);
  EXPECT_EQ(fx.stats.validationsFailed.load(), 0u);
  EXPECT_EQ(fx.stats.metaCorrupt.load(), 0u);
  EXPECT_EQ(fx.stats.opens.load(), 2u);
}

// ------------------------------------------------------------ speculative
// A speculative (prefetched) page serves reads like any staged page but is
// never written while it carries its mark; the mark clears when the reader
// demands the page (served) or a real fill covers it. Pages still marked when
// dropped or at close reach neither sidecar, bitmap nor data file.
TEST(FileEntry, SpeculativePagesServeButNeverWriteUntilServed) {
  Fixture fx(64 * 4096, 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  EXPECT_EQ(e->stageSpeculative(4096, 3 * 4096, fx.src.data() + 4096), 3u * 4096); // pages 1..3
  EXPECT_EQ(e->speculativeBytes(), 3u * 4096);
  EXPECT_TRUE(e->hasRange(4096, 3 * 4096)); // present to readers
  e->flushBuffer(true);
  e->checkpoint();
  e->flushAll();
  EXPECT_EQ(fx.stats.pageWrites.load(), 0u); // nothing written by any drain
  EXPECT_EQ(e->cachedBytes(), 0u);           // nothing published
  EXPECT_TRUE(e->hasRange(4096, 3 * 4096));  // still served from RAM
  std::vector<uint8_t> buf(4096);
  ASSERT_TRUE(e->readCached(2 * 4096, 4096, buf.data())); // page 2 demanded
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + 2 * 4096, 4096));
  EXPECT_EQ(fx.stats.prefetchServedBytes.load(), 4096u);
  EXPECT_EQ(e->speculativeBytes(), 2u * 4096);
  e->flushBuffer(true);
  EXPECT_EQ(fx.stats.pageWrites.load(), 1u); // page 2 only
  EXPECT_EQ(e->cachedBytes(), 4096u);
  EXPECT_TRUE(e->hasRange(4096, 4096)); // 1 and 3 still staged speculatively
  EXPECT_EQ(e->dropAllSpeculative(), 2u * 4096);
  EXPECT_FALSE(e->hasRange(4096, 4096));
  EXPECT_TRUE(e->hasRange(2 * 4096, 4096));
  EXPECT_EQ(fx.stats.prefetchDroppedUnread.load(), 2u * 4096);
  e.reset();
  auto r = fx.open(); // on disk: page 2 and nothing else
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->hasRange(2 * 4096, 4096));
  EXPECT_FALSE(r->hasRange(4096, 4096));
  EXPECT_FALSE(r->hasRange(3 * 4096, 4096));
}

TEST(FileEntry, RealFillCoversSpeculativePage) {
  Fixture fx(16 * 4096, 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  EXPECT_EQ(e->stageSpeculative(5 * 4096, 4096, fx.src.data() + 5 * 4096), 4096u);
  // The demand read fetched the same page (no vector-path dedup): the mark
  // clears, the bytes count as served AND as fetched twice.
  e->writePages(5 * 4096, 4096, fx.src.data() + 5 * 4096);
  EXPECT_EQ(fx.stats.prefetchServedBytes.load(), 4096u);
  EXPECT_EQ(fx.stats.prefetchRefetchedBytes.load(), 4096u);
  EXPECT_EQ(e->speculativeBytes(), 0u);
  e->flushBuffer(true);
  EXPECT_EQ(fx.stats.pageWrites.load(), 1u);
}

TEST(FileEntry, SpeculativeStageSkipsPresentAndStagedPages) {
  Fixture fx(16 * 4096, 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  e->writePages(0, 2 * 4096, fx.src.data()); // pages 0,1
  e->flushBuffer(true);                       //   ... on disk
  e->writePages(2 * 4096, 4096, fx.src.data() + 2 * 4096); // page 2 staged, real
  EXPECT_EQ(e->stageSpeculative(0, 5 * 4096, fx.src.data()), 2u * 4096); // only 3,4 taken
  auto runs = e->absentRuns(0, 8 * 4096); // 5,6,7 absent, one run
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].first, 5u * 4096);
  EXPECT_EQ(runs[0].second, 3u * 4096);
  EXPECT_EQ(e->dropAllSpeculative(), 2u * 4096);
  runs = e->absentRuns(0, 8 * 4096); // 3..7 absent now
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].first, 3u * 4096);
  EXPECT_EQ(runs[0].second, 5u * 4096);
  e->flushBuffer(true); // publishes the real page 2 only
  EXPECT_EQ(fx.stats.pageWrites.load(), 3u);
}

TEST(FileEntry, DropSpeculativeLeavesServedPages) {
  Fixture fx(16 * 4096, 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  EXPECT_EQ(e->stageSpeculative(0, 4 * 4096, fx.src.data()), 4u * 4096);
  std::vector<uint8_t> buf(4096);
  ASSERT_TRUE(e->readCached(4096, 4096, buf.data())); // page 1 served
  EXPECT_EQ(e->dropSpeculative(0, 4 * 4096), 3u * 4096);
  EXPECT_TRUE(e->hasRange(4096, 4096));
  EXPECT_FALSE(e->hasRange(0, 4096));
  EXPECT_FALSE(e->hasRange(2 * 4096, 2 * 4096));
}

TEST(FileEntry, SpeculativePagesAtCloseAreDroppedAndCounted) {
  Fixture fx(16 * 4096, 4096);
  {
    auto e = fx.open();
    ASSERT_TRUE(e);
    EXPECT_EQ(e->stageSpeculative(0, 4 * 4096, fx.src.data()), 4u * 4096);
  }
  EXPECT_EQ(fx.stats.prefetchDroppedUnread.load(), 4u * 4096);
  EXPECT_EQ(fx.stats.pageWrites.load(), 0u);
  auto r = fx.open();
  ASSERT_TRUE(r);
  EXPECT_FALSE(r->hasRange(0, 4 * 4096));
}

// Speculative producer racing readers, real fills, droppers and a forced
// flusher (TSan target). Whatever the interleaving: every byte served is
// correct, and nothing still marked speculative at the end is on disk.
TEST(FileEntry, ConcurrentSpeculativeStageServeDrop) {
  Fixture fx(256 * 4096, 4096);
  auto e = fx.open();
  ASSERT_TRUE(e);
  std::atomic<bool> stop{false};
  std::vector<std::thread> ts;
  for (int w = 0; w < 2; ++w)
    ts.emplace_back([&, w] {
      for (int r = 0; r < 300; ++r) {
        uint64_t page = (w * 300 + r * 11) % 250;
        e->stageSpeculative(page * 4096, 3 * 4096, fx.src.data() + page * 4096);
      }
    });
  for (int w = 0; w < 2; ++w)
    ts.emplace_back([&, w] {
      for (int r = 0; r < 200; ++r) {
        uint64_t page = (w * 200 + r * 7) % 250;
        e->writePages(page * 4096, 2 * 4096, fx.src.data() + page * 4096);
      }
    });
  for (int rd = 0; rd < 3; ++rd)
    ts.emplace_back([&] {
      std::vector<uint8_t> buf(2 * 4096);
      while (!stop.load(std::memory_order_relaxed)) {
        uint64_t page = static_cast<uint64_t>(::rand()) % 250;
        if (e->readCached(page * 4096, 2 * 4096, buf.data())) {
          ASSERT_EQ(0, memcmp(buf.data(), fx.src.data() + page * 4096, 2 * 4096));
        }
      }
    });
  ts.emplace_back([&] {
    for (int i = 0; i < 100; ++i)
      e->dropSpeculative(static_cast<uint64_t>(::rand() % 250) * 4096, 4 * 4096);
  });
  ts.emplace_back([&] {
    for (int i = 0; i < 40; ++i)
      e->flushBuffer(true);
  });
  for (size_t i = 0; i < 4; ++i)
    ts[i].join(); // the two speculative producers and the two fills
  stop = true;  // readers loop until told; joining one earlier deadlocked this test
  for (size_t i = 4; i < ts.size(); ++i)
    ts[i].join();
  const uint64_t specLeft = e->speculativeBytes();
  e->dropAllSpeculative();
  e->flushBuffer(true);
  EXPECT_EQ(e->speculativeBytes(), 0u);
  // Every published page is byte-correct; the count matches the disk.
  auto scrub = e->verifyAll();
  EXPECT_EQ(scrub.bad, 0u);
  EXPECT_EQ(scrub.checked * 4096, e->cachedBytes());
  EXPECT_EQ(fx.stats.prefetchDroppedUnread.load() >= specLeft, true);
}
