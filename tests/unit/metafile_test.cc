#include "MetaFile.h"

#include "TestUtil.h"
#include <fcntl.h>
#include <gtest/gtest.h>

using namespace ucache;
using test::TempDir;

namespace {
MetaData sample() {
  MetaData m = MetaData::fresh("root://h:1094/f", 100000, 4096); // 25 pages
  m.flags = MetaData::kFlagPinned;
  m.originMtime = 1234567;
  m.cksumKind = MetaData::kCksumAdler32;
  m.originCksum = 0xDEADBEEF;
  m.bitmap.set(0);
  m.bitmap.set(24);
  m.pageCrcs[0] = 111;
  m.pageCrcs[24] = 222;
  return m;
}
} // namespace

TEST(MetaFile, SerializeDeserializeRoundtrip) {
  MetaData m = sample();
  auto buf = MetaFile::serialize(m);
  auto d = MetaFile::deserialize(buf.data(), buf.size());
  ASSERT_TRUE(d);
  EXPECT_EQ(d->pageSize, 4096u);
  EXPECT_EQ(d->flags, MetaData::kFlagPinned);
  EXPECT_EQ(d->fileSize, 100000u);
  EXPECT_EQ(d->atime, m.atime);
  EXPECT_EQ(d->originMtime, 1234567u);
  EXPECT_EQ(d->cksumKind, MetaData::kCksumAdler32);
  EXPECT_EQ(d->originCksum, 0xDEADBEEFu);
  EXPECT_EQ(d->key, "root://h:1094/f");
  EXPECT_EQ(d->npages(), 25u);
  EXPECT_EQ(d->bitmap.count(), 2u);
  EXPECT_TRUE(d->bitmap.get(0));
  EXPECT_TRUE(d->bitmap.get(24));
  EXPECT_EQ(d->pageCrcs[0], 111u);
  EXPECT_EQ(d->pageCrcs[24], 222u);
}

TEST(MetaFile, TailPageAccounting) {
  MetaData m = MetaData::fresh("k", 100000, 4096);
  EXPECT_EQ(m.pageBytes(0), 4096u);
  EXPECT_EQ(m.pageBytes(24), 100000u - 24 * 4096u); // 1696
  m.bitmap.set(24);
  EXPECT_EQ(m.cachedBytes(), 1696u);
  m.bitmap.set(0);
  EXPECT_EQ(m.cachedBytes(), 4096u + 1696u);
}

TEST(MetaFile, RejectsCorruption) {
  auto buf = MetaFile::serialize(sample());
  // Truncated.
  EXPECT_FALSE(MetaFile::deserialize(buf.data(), buf.size() - 1));
  EXPECT_FALSE(MetaFile::deserialize(buf.data(), 10));
  // Any flipped byte must fail the whole-image CRC.
  for (size_t off : {0ul, 5ul, 20ul, 57ul, buf.size() - 1}) {
    auto bad = buf;
    bad[off] ^= 0x40;
    EXPECT_FALSE(MetaFile::deserialize(bad.data(), bad.size())) << "offset " << off;
  }
}

TEST(MetaFile, RejectsWrongVersion) {
  auto buf = MetaFile::serialize(sample());
  buf[4] = 99; // format_version low byte
  // Recompute nothing: version check happens before CRC, must still reject.
  EXPECT_FALSE(MetaFile::deserialize(buf.data(), buf.size()));
}

TEST(MetaFile, StoreLoadAtomicRename) {
  TempDir td;
  RealIO io;
  std::string path = td.path() + "/x.meta";
  MetaData m = sample();
  ASSERT_EQ(MetaFile::store(io, path, m, /*fsync=*/true), 0);
  auto d = MetaFile::load(io, path);
  ASSERT_TRUE(d);
  EXPECT_EQ(d->key, m.key);
  // No tmp file left behind.
  struct ::stat st;
  EXPECT_LT(io.stat(path + ".tmp", &st), 0);
  // Rewrite with changed content wins atomically.
  m.bitmap.set(5);
  m.pageCrcs[5] = 55;
  ASSERT_EQ(MetaFile::store(io, path, m, false), 0);
  d = MetaFile::load(io, path);
  ASSERT_TRUE(d);
  EXPECT_TRUE(d->bitmap.get(5));
}

TEST(MetaFile, LoadMissingOrTorn) {
  TempDir td;
  RealIO io;
  EXPECT_FALSE(MetaFile::load(io, td.path() + "/absent.meta"));
  // Torn write simulation: store then truncate mid-image.
  std::string path = td.path() + "/torn.meta";
  ASSERT_EQ(MetaFile::store(io, path, sample(), false), 0);
  int fd = io.open(path, O_RDWR, 0);
  ASSERT_GE(fd, 0);
  struct ::stat st;
  ASSERT_EQ(io.fstat(fd, &st), 0);
  ASSERT_EQ(io.ftruncate(fd, st.st_size / 2), 0);
  io.close(fd);
  EXPECT_FALSE(MetaFile::load(io, path));
}

// The scan-path summary load must agree with the full load on every
// reporting field while skipping the per-page crc table entirely.
TEST(MetaFile, LoadSummaryMatchesLoad) {
  TempDir td;
  RealIO io;
  std::string path = td.path() + "/x.meta";
  ASSERT_EQ(MetaFile::store(io, path, sample(), false), 0);
  auto full = MetaFile::load(io, path);
  auto sum = MetaFile::loadSummary(io, path);
  ASSERT_TRUE(full);
  ASSERT_TRUE(sum);
  EXPECT_EQ(sum->key, full->key);
  EXPECT_EQ(sum->pageSize, full->pageSize);
  EXPECT_EQ(sum->flags, full->flags);
  EXPECT_EQ(sum->fileSize, full->fileSize);
  EXPECT_EQ(sum->atime, full->atime);
  EXPECT_EQ(sum->originMtime, full->originMtime);
  EXPECT_EQ(sum->cksumKind, full->cksumKind);
  EXPECT_EQ(sum->originCksum, full->originCksum);
  EXPECT_EQ(sum->cachedBytes(), full->cachedBytes());
  EXPECT_EQ(sum->bitmap.count(), full->bitmap.count());
  EXPECT_EQ(sum->npages(), full->npages());
  EXPECT_TRUE(sum->bitmap.get(0));
  EXPECT_TRUE(sum->bitmap.get(24));
  // The crc table is NOT loaded — that is the point of the summary.
  EXPECT_TRUE(sum->pageCrcs.empty());
  EXPECT_EQ(full->pageCrcs.size(), full->npages());
}

// Geometry gate: any size change (torn truncate, stray growth) rejects the
// summary just like the whole-image crc rejects the full load.
TEST(MetaFile, LoadSummaryRejectsSizeMismatch) {
  TempDir td;
  RealIO io;
  std::string path = td.path() + "/g.meta";
  ASSERT_EQ(MetaFile::store(io, path, sample(), false), 0);
  ASSERT_TRUE(MetaFile::loadSummary(io, path));
  struct ::stat st;
  ASSERT_EQ(io.stat(path, &st), 0);
  int fd = io.open(path, O_RDWR, 0);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(io.ftruncate(fd, st.st_size - 1), 0); // torn tail
  io.close(fd);
  EXPECT_FALSE(MetaFile::loadSummary(io, path));
  EXPECT_FALSE(MetaFile::load(io, path));
  // Grown image rejected too.
  fd = io.open(path, O_RDWR, 0);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(io.ftruncate(fd, st.st_size + 8), 0);
  io.close(fd);
  EXPECT_FALSE(MetaFile::loadSummary(io, path));
  EXPECT_FALSE(MetaFile::load(io, path));
}

TEST(MetaFile, LoadSummaryRejectsBadHeader) {
  TempDir td;
  RealIO io;
  EXPECT_FALSE(MetaFile::loadSummary(io, td.path() + "/absent.meta"));
  std::string path = td.path() + "/h.meta";
  ASSERT_EQ(MetaFile::store(io, path, sample(), false), 0);
  // Bad magic.
  int fd = io.open(path, O_RDWR, 0);
  ASSERT_GE(fd, 0);
  const char x = 'X';
  ASSERT_EQ(io.pwriteFull(fd, &x, 1, 0), 1);
  io.close(fd);
  EXPECT_FALSE(MetaFile::loadSummary(io, path));
  // Bad version (rewrite a clean image first).
  ASSERT_EQ(MetaFile::store(io, path, sample(), false), 0);
  fd = io.open(path, O_RDWR, 0);
  ASSERT_GE(fd, 0);
  const uint32_t v = MetaData::kFormatVersion + 1;
  ASSERT_EQ(io.pwriteFull(fd, &v, 4, 4), 4);
  io.close(fd);
  EXPECT_FALSE(MetaFile::loadSummary(io, path));
  // Hostile pageSize (0 / non-power-of-two) in an otherwise sane header.
  ASSERT_EQ(MetaFile::store(io, path, sample(), false), 0);
  fd = io.open(path, O_RDWR, 0);
  ASSERT_GE(fd, 0);
  const uint32_t badPs = 3;
  ASSERT_EQ(io.pwriteFull(fd, &badPs, 4, 8), 4);
  io.close(fd);
  EXPECT_FALSE(MetaFile::loadSummary(io, path));
}

// A summary must never resurrect crcs through store(): serialize() writes the
// table as absent (all-zero) when pageCrcs is empty rather than reading OOB.
TEST(MetaFile, SerializeEmptyCrcsWritesAbsentTable) {
  MetaData m = sample();
  m.pageCrcs.clear();
  auto buf = MetaFile::serialize(m);
  auto d = MetaFile::deserialize(buf.data(), buf.size());
  ASSERT_TRUE(d);
  EXPECT_EQ(d->pageCrcs.size(), d->npages());
  for (uint32_t c : d->pageCrcs)
    EXPECT_EQ(c, 0u);
}
