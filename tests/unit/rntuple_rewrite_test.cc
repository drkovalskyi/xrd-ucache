// The RNTuple recompressor, checked against a committed fixture.
//
// ROOT is the arbiter for whether a rewritten file is READABLE, and that check
// lives in an environment-bound gate because it needs ROOT and real files.
// What can be proved here, hermetically and with no ROOT at all, is the
// property that actually matters for correctness: **every page must decode to
// exactly the same bytes after recompression as before.** A codec change that
// preserves that cannot alter physics; one that breaks it cannot be saved by
// anything downstream.
//
// The fixture is 6.5 KB and deliberately covers what a NanoAOD file does not
// fit into that size: three clusters, a bit-packed boolean column (one bit per
// element, where the byte count is a CEILING), a constant column whose
// identical pages are shared between records, and a variable-length column.
// tests/data/make_rntuple_fixture.C regenerates it.
#include "RNTupleMeta.h"
#include "RNTupleRewrite.h"
#include "TreeMeta.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ucache::transpose;

namespace {

std::string fixture() { return std::string(UCACHE_TEST_DATA_DIR) + "/rntuple_fixture.root"; }

class FileSource : public Source {
public:
  explicit FileSource(int fd, uint64_t size) : fd_(fd), size_(size) {}
  bool read(void* dst, uint64_t n, uint64_t off) override {
    return ::pread(fd_, dst, n, (off_t)off) == (ssize_t)n;
  }
  bool has(uint64_t off, uint64_t n) override { return off + n >= off && off + n <= size_; }

private:
  int fd_;
  uint64_t size_;
};

// Decode every page of `m`, reading through `src`, into one blob per page in
// page-table order. Two files with the same decoded sequence hold the same
// data whatever codec they are written in.
std::vector<std::vector<uint8_t>> decodeAllPages(const RNTupleMeta& m, Source& src) {
  std::vector<std::vector<uint8_t>> out;
  for (const auto& range : m.ranges) {
    for (const auto& pg : range.pages) {
      std::vector<uint8_t> raw(pg.nbytes);
      if (pg.nbytes && !src.read(raw.data(), pg.nbytes, pg.offset)) return {};
      if ((uint64_t)pg.nbytes == pg.uncompressedBytes) {
        out.push_back(std::move(raw)); // stored uncompressed
      } else {
        auto d = decompressFrames(raw.data(), raw.size(), pg.uncompressedBytes);
        if (d.size() != pg.uncompressedBytes) return {};
        out.push_back(std::move(d));
      }
    }
  }
  return out;
}

struct Rewritten {
  std::string path;
  ~Rewritten() {
    if (!path.empty()) ::unlink(path.c_str());
  }
};

} // namespace

TEST(RNTupleFixture, ParsesWithTheStructureTheGateRelevsOn) {
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  EXPECT_EQ(m.ntupleName, "Events");
  EXPECT_EQ(m.nEntries, 2000u);
  // If any of these drift the fixture stopped covering what it was built for.
  EXPECT_EQ(m.nClusters, 3u) << "fixture must span several clusters";
  EXPECT_EQ(m.columns.size(), 6u);
  size_t pages = 0, bitPacked = 0;
  for (const auto& r : m.ranges) pages += r.pages.size();
  for (const auto& c : m.columns)
    if (c.bitsOnStorage == 1) ++bitPacked;
  EXPECT_GT(pages, 20u);
  EXPECT_GE(bitPacked, 1u) << "fixture must contain a bit-packed column";
}

TEST(RNTupleFixture, SharedPagesAreRealInTheFixture) {
  // Several page records pointing at the same bytes is a format feature, not a
  // curiosity: the writer must reuse the transcode and must not punch a page
  // another record still needs.
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  size_t pages = 0;
  std::vector<uint64_t> offs;
  for (const auto& r : m.ranges)
    for (const auto& p : r.pages) {
      ++pages;
      offs.push_back(p.offset);
    }
  std::sort(offs.begin(), offs.end());
  const size_t distinct = std::unique(offs.begin(), offs.end()) - offs.begin();
  EXPECT_LT(distinct, pages) << "fixture must contain at least one shared page";
}

TEST(RNTupleRewrite, EveryPageDecodesToTheSameBytesAfterRecompression) {
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  int fd = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileSource src(fd, m.fileSize);

  const auto before = decodeAllPages(m, src);
  ASSERT_FALSE(before.empty());

  auto rw = buildRNTupleRewrite(m, src, m.fileSize, 1);
  ::close(fd);
  ASSERT_TRUE(rw.error.empty()) << rw.error;
  EXPECT_EQ(rw.rangesRelocated, m.ranges.size());
  EXPECT_GE(rw.sharedPages, 1u) << "a shared page must be reused, not re-encoded";

  Rewritten out{std::string(::testing::TempDir()) + "/ucache_rntuple_rw.root"};
  std::string err;
  ASSERT_TRUE(writeRewrittenRNTuple(fixture(), out.path, rw, err)) << err;

  auto m2 = parseRNTuple(out.path, "");
  ASSERT_TRUE(m2.error.empty()) << m2.error;
  EXPECT_EQ(m2.nEntries, m.nEntries);
  EXPECT_EQ(m2.nClusters, m.nClusters);
  EXPECT_EQ(m2.columns.size(), m.columns.size());
  for (const auto& r : m2.ranges)
    EXPECT_EQ(r.compressionSettings, 501) << "pages must now be ZSTD-1";

  int fd2 = ::open(out.path.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd2, 0);
  FileSource src2(fd2, m2.fileSize);
  const auto after = decodeAllPages(m2, src2);
  ::close(fd2);

  // The whole point: same decoded bytes, different codec.
  ASSERT_EQ(after.size(), before.size());
  for (size_t i = 0; i < before.size(); ++i)
    EXPECT_EQ(after[i], before[i]) << "page " << i << " changed content";
}

TEST(RNTupleRewrite, OverlayStitchesToTheRewrittenFile) {
  // The replica serves the overlay, not the standalone file. If the two ever
  // disagree, a check on one says nothing about the other.
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;
  int fd = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileSource src(fd, m.fileSize);
  auto rw = buildRNTupleRewrite(m, src, m.fileSize, 1);
  ASSERT_TRUE(rw.error.empty()) << rw.error;

  Rewritten out{std::string(::testing::TempDir()) + "/ucache_rntuple_stitch.root"};
  std::string err;
  ASSERT_TRUE(writeRewrittenRNTuple(fixture(), out.path, rw, err)) << err;

  auto ov = rnTupleOverlay(m, rw);
  ASSERT_TRUE(ov.error.empty()) << ov.error;
  std::vector<uint8_t> stitched(ov.meta.virtualSize, 0);
  ASSERT_EQ(::pread(fd, stitched.data(), m.fileSize, 0), (ssize_t)m.fileSize);
  ::close(fd);
  for (const auto& e : ov.meta.extents)
    std::memcpy(stitched.data() + e.virtOff, ov.tdata.data() + e.tdataOff, e.len);

  std::vector<uint8_t> written(ov.meta.virtualSize, 0);
  int fd2 = ::open(out.path.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd2, 0);
  ASSERT_EQ(::pread(fd2, written.data(), written.size(), 0), (ssize_t)written.size());
  ::close(fd2);
  EXPECT_EQ(stitched, written);
}

TEST(RNTupleRewrite, CorruptPageIsRefusedRatherThanRecompressed) {
  // Negative control. A page whose bytes are damaged must stop the build, not
  // be re-encoded into a replica that serves wrong data for ever.
  auto m = parseRNTuple(fixture(), "");
  ASSERT_TRUE(m.error.empty()) << m.error;

  // Copy the fixture and flip a byte inside the LAST page's payload — the
  // first page of a column is not a safe target, since a decoder can be handed
  // a plausible-looking block and produce something.
  const std::string copy = std::string(::testing::TempDir()) + "/ucache_rntuple_rot.root";
  {
    std::vector<uint8_t> all(m.fileSize);
    int f = ::open(fixture().c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(f, 0);
    ASSERT_EQ(::pread(f, all.data(), all.size(), 0), (ssize_t)all.size());
    ::close(f);
    const auto& last = m.ranges.back().pages.back();
    ASSERT_GT(last.nbytes, 4u);
    all[last.offset + last.nbytes / 2] ^= 0xFF;
    int g = ::open(copy.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    ASSERT_GE(g, 0);
    ASSERT_EQ(::write(g, all.data(), all.size()), (ssize_t)all.size());
    ::close(g);
  }
  Rewritten cleanup{copy};

  auto m2 = parseRNTuple(copy, "");
  ASSERT_TRUE(m2.error.empty()) << m2.error; // the page table is still intact
  int fd = ::open(copy.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(fd, 0);
  FileSource src(fd, m2.fileSize);
  auto rw = buildRNTupleRewrite(m2, src, m2.fileSize, 1);
  ::close(fd);
  EXPECT_FALSE(rw.error.empty()) << "a corrupt page must not recompress silently";
}
