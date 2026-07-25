// Reading a hole must not look like a corrupt file.
//
// An overlay build works from a bitmap snapshot: the sidecar is loaded once,
// then baskets are read for as long as the build takes. Reclaim clears the
// bits, flushes the sidecar, and only then punches the bytes — so a build that
// started earlier can be pointed at pages that have since become holes. They
// read back as zeros. A builder that does not verify what it reads carries
// those zeros into the codec and reports "unexpected basket key" or "basket
// decompression failed": a malformed-file verdict on a healthy file, and a
// `failed` count that cannot be trusted.
//
// Two things are pinned here: CacheSource catches the hole via the per-page
// checksum the serving path already relies on, and the builder classifies a
// source that cannot vouch for its bytes as retryable rather than failed.
// Genuinely malformed bytes must still fail hard.
#include "CacheSource.h"
#include "MetaFile.h"
#include "TreeMeta.h"
#include "Transposer.h"
#include "vendor/crc32c.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "TestUtil.h"

using namespace ucache;
using namespace ucache::transpose;

namespace {

constexpr uint32_t kPageSize = 4096;
constexpr uint64_t kBasketOff = 8192; // page-aligned, two pages in
constexpr uint32_t kBasketLen = 4096;

// A one-branch, one-basket tree. buildOverlay walks the baskets before it
// touches tree metadata or the keys list, so this is all the geometry the
// availability paths need.
FileMeta oneBasketTree() {
  FileMeta fm;
  fm.large = false;
  fm.headerSeekWidth = 4;
  fm.dirSeekWidth = 4;
  fm.fend = 65536;
  fm.keyslistSeek = 32768;
  fm.treeBlob.assign(128, 0xAB);
  BranchInfo b;
  b.name = "Muon_pt";
  b.writeBasket = 1;
  b.basketSeek = {static_cast<int64_t>(kBasketOff)};
  b.basketBytes = {static_cast<int32_t>(kBasketLen)};
  b.seekArrayOff = 8;
  b.bytesArrayOff = 40;
  fm.branches.push_back(b);
  return fm;
}

// Claims coverage and hands back whatever it was given — the shape of a
// builder source with no verification.
struct BlindSource : Source {
  std::vector<uint8_t> bytes;
  bool has(uint64_t, uint64_t) override { return true; }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (off + n > bytes.size())
      return false;
    std::memcpy(dst, bytes.data() + off, static_cast<size_t>(n));
    return true;
  }
};

// Claims coverage (the snapshot said so) but refuses the read (the checksum
// says otherwise) — the shape of CacheSource over a punched page.
struct StaleSnapshotSource : Source {
  bool has(uint64_t, uint64_t) override { return true; }
  bool read(void*, uint64_t, uint64_t) override { return false; }
};

// An entry on disk: `fileSize` bytes of content, a sidecar bitmap marking
// every page present, and honest per-page checksums.
struct Entry {
  std::string path;
  MetaData meta;
  int fd = -1;
  ~Entry() {
    if (fd >= 0)
      ::close(fd);
  }
};

void fillEntry(Entry& e, const ucache::test::TempDir& td, const std::vector<uint8_t>& content) {
  e.path = td.path() + "/entry.data";
  FILE* f = ::fopen(e.path.c_str(), "wb");
  ::fwrite(content.data(), 1, content.size(), f);
  ::fclose(f);
  e.meta = MetaData::fresh("root://h//f.root", content.size(), kPageSize);
  for (uint64_t i = 0; i < e.meta.npages(); ++i) {
    e.meta.bitmap.set(i);
    e.meta.pageCrcs[i] = crc32c(content.data() + i * kPageSize, e.meta.pageBytes(i));
  }
  e.fd = ::open(e.path.c_str(), O_RDONLY | O_CLOEXEC);
}

// Punch a page the way reclaim does, but leave the sidecar alone: bit still
// set, checksum still the pre-punch one. This is precisely the state a build
// holding an older snapshot sees.
void zeroPageOnDisk(const std::string& path, uint64_t page) {
  int fd = ::open(path.c_str(), O_WRONLY);
  ASSERT_GE(fd, 0);
  std::vector<uint8_t> zeros(kPageSize, 0);
  ASSERT_EQ(::pwrite(fd, zeros.data(), zeros.size(), static_cast<off_t>(page * kPageSize)),
            static_cast<ssize_t>(zeros.size()));
  ::close(fd);
}

} // namespace

// ------------------------------------------------- CacheSource verification

TEST(TransposeHole, IntactPagesReadBack) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(16384, 7));
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;

  std::vector<uint8_t> got(kBasketLen);
  EXPECT_TRUE(src.has(kBasketOff, kBasketLen));
  ASSERT_TRUE(src.read(got.data(), kBasketLen, kBasketOff));
  std::vector<uint8_t> want(kBasketLen);
  ASSERT_EQ(::pread(e.fd, want.data(), want.size(), static_cast<off_t>(kBasketOff)),
            static_cast<ssize_t>(want.size()));
  EXPECT_EQ(got, want);
}

TEST(TransposeHole, UnalignedReadsSpanningPagesAreVerified) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(16384, 11));
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;
  // Straddles three pages and starts mid-page: the checksum is per page, the
  // read is not, so the buffer arithmetic has to be right.
  const uint64_t off = kPageSize + 100, n = 2 * kPageSize + 55;
  std::vector<uint8_t> got(n), want(n);
  ASSERT_TRUE(src.read(got.data(), n, off));
  ASSERT_EQ(::pread(e.fd, want.data(), want.size(), static_cast<off_t>(off)),
            static_cast<ssize_t>(want.size()));
  EXPECT_EQ(got, want);
}

TEST(TransposeHole, PunchedPageIsRefusedNotReturnedAsZeros) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(16384, 13));
  zeroPageOnDisk(e.path, kBasketOff / kPageSize);
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;

  // The stale snapshot still claims the page — that is the whole problem.
  EXPECT_TRUE(src.has(kBasketOff, kBasketLen));
  std::vector<uint8_t> got(kBasketLen, 0xEE);
  EXPECT_FALSE(src.read(got.data(), kBasketLen, kBasketOff));
  // A neighbouring page is untouched and must still be readable.
  EXPECT_TRUE(src.read(got.data(), kBasketLen, 0));
}

TEST(TransposeHole, TailPageAndShortSidecarsAreHandled) {
  ucache::test::TempDir td;
  Entry e;
  fillEntry(e, td, ucache::test::randomBytes(kPageSize + 100, 17)); // short tail page
  CacheSource src;
  src.fd = e.fd;
  src.meta = &e.meta;
  std::vector<uint8_t> got(100);
  EXPECT_TRUE(src.read(got.data(), 100, kPageSize)); // tail page verifies on its real length
  EXPECT_FALSE(src.read(got.data(), 100, kPageSize + 50));  // past EOF
  // A summary-loaded sidecar carries no checksum table: nothing to verify
  // against, so a read must refuse rather than pass bytes through unchecked.
  MetaData summary = e.meta;
  summary.pageCrcs.clear();
  src.meta = &summary;
  EXPECT_FALSE(src.read(got.data(), 100, 0));
}

// ------------------------------------------------- builder classification

TEST(TransposeHole, StaleSnapshotBuildIsTransientNotFailed) {
  FileMeta fm = oneBasketTree();
  StaleSnapshotSource src;
  Overlay ov = buildOverlay(fm, src, {"Muon_pt"});
  ASSERT_FALSE(ov.error.empty());
  EXPECT_TRUE(ov.transient) << "reclaimed bytes are retryable, not a build failure: " << ov.error;
  EXPECT_NE(ov.error.find("no longer readable"), std::string::npos) << ov.error;
}

TEST(TransposeHole, UncachedBasketIsTransient) {
  FileMeta fm = oneBasketTree();
  struct NothingCached : Source {
    bool has(uint64_t, uint64_t) override { return false; }
    bool read(void*, uint64_t, uint64_t) override { return false; }
  } src;
  Overlay ov = buildOverlay(fm, src, {"Muon_pt"});
  ASSERT_FALSE(ov.error.empty());
  EXPECT_TRUE(ov.transient) << ov.error;
  EXPECT_NE(ov.error.find("not available"), std::string::npos) << ov.error;
}

TEST(TransposeHole, MalformedBasketStillFailsHard) {
  FileMeta fm = oneBasketTree();
  BlindSource src;
  src.bytes.assign(kBasketOff + kBasketLen, 0xAB); // not a TBasket key
  Overlay ov = buildOverlay(fm, src, {"Muon_pt"});
  ASSERT_FALSE(ov.error.empty());
  EXPECT_FALSE(ov.transient) << "a malformed file is a real failure: " << ov.error;
  EXPECT_NE(ov.error.find("unexpected basket key"), std::string::npos) << ov.error;
}

// The field symptom, kept as a fact: hand zeros to a builder whose source does
// not verify and the hole is indistinguishable from corruption. This is what
// the background worker logged for entries whose v1 bytes had been reclaimed
// under it, and the reason CacheSource checksums every page it reads.
TEST(TransposeHole, UnverifiedHoleIsIndistinguishableFromCorruption) {
  FileMeta fm = oneBasketTree();
  BlindSource src;
  src.bytes.assign(kBasketOff + kBasketLen, 0); // a punched range, read raw
  Overlay ov = buildOverlay(fm, src, {"Muon_pt"});
  ASSERT_FALSE(ov.error.empty());
  EXPECT_FALSE(ov.transient);
  EXPECT_NE(ov.error.find("unexpected basket key"), std::string::npos) << ov.error;
}
