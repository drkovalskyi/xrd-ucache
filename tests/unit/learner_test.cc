// Learner (deriveHotBranches / withCounters / branchCodec) contract tests:
// a partially-read counter must never fail a build (it is left
// out), and the recompress_codecs policy filters by source codec.
#include "Transposer.h"

#include <cstring>
#include <gtest/gtest.h>
#include <map>

using namespace ucache::transpose;

namespace {

// Byte source backed by explicit ranges: has() = range registered, read() =
// bytes from the map (zero-filled where unspecified).
struct MockSource : Source {
  std::map<uint64_t, std::vector<uint8_t>> ranges; // off -> bytes
  bool has(uint64_t off, uint64_t n) override {
    auto it = ranges.find(off);
    return it != ranges.end() && it->second.size() >= n;
  }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    auto it = ranges.find(off);
    if (it == ranges.end() || it->second.size() < n)
      return false;
    std::memcpy(dst, it->second.data(), n);
    return true;
  }
};

void bePut32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
void bePut16(uint8_t* p, uint16_t v) {
  p[0] = v >> 8;
  p[1] = v;
}

// A minimal valid basket TKey (ver 4, 32-bit seeks) followed by a compression
// frame magic. keylen = 30 (26 fixed + three 1-byte empty strings + pad).
std::vector<uint8_t> basketBytes(uint32_t nbytes, uint32_t objlen, char m0, char m1) {
  const uint16_t keylen = 30;
  std::vector<uint8_t> b(nbytes, 0);
  bePut32(b.data(), nbytes);
  bePut16(b.data() + 4, 4); // ver < 1000: 32-bit seeks, strings at q=26
  bePut32(b.data() + 6, objlen);
  bePut16(b.data() + 14, keylen);
  bePut16(b.data() + 16, 1);   // cycle
  bePut32(b.data() + 18, 0);   // seekkey
  b[26] = 0;                   // cls ""
  b[27] = 0;                   // name ""
  b[28] = 0;                   // title ""
  if (nbytes > keylen + 1) {
    b[keylen] = static_cast<uint8_t>(m0);
    b[keylen + 1] = static_cast<uint8_t>(m1);
  }
  return b;
}

BranchInfo mkBranch(const std::string& name, uint64_t seek, int32_t nb,
                    const std::string& counter = "") {
  BranchInfo b;
  b.name = name;
  b.writeBasket = 1;
  b.basketSeek = {static_cast<int64_t>(seek)};
  b.basketBytes = {nb};
  b.leafCount = counter;
  return b;
}

} // namespace

TEST(Learner, PartiallyCachedCounterIsLeftOutNotFatal) {
  FileMeta fm;
  fm.branches.push_back(mkBranch("mm_pt", 1000, 100, "nmm"));
  fm.branches.push_back(mkBranch("nmm", 5000, 50)); // the counter branch
  MockSource src;
  src.ranges[1000] = std::vector<uint8_t>(100, 0); // mm_pt fully cached
  // nmm's basket deliberately ABSENT (the real-cache failure: "nmm: basket 0
  // not available" used to fail the whole file's build).
  auto hot = deriveHotBranches(fm, src);
  ASSERT_EQ(hot.size(), 1u);
  EXPECT_EQ(hot[0], "mm_pt"); // dependent kept, unbuildable counter left out
}

TEST(Learner, CoveredCounterRidesAlong) {
  FileMeta fm;
  fm.branches.push_back(mkBranch("mm_pt", 1000, 100, "nmm"));
  fm.branches.push_back(mkBranch("nmm", 5000, 50));
  MockSource src;
  src.ranges[1000] = std::vector<uint8_t>(100, 0);
  src.ranges[5000] = std::vector<uint8_t>(50, 0);
  auto hot = deriveHotBranches(fm, src);
  ASSERT_EQ(hot.size(), 2u);
  EXPECT_EQ(hot[0], "mm_pt");
  EXPECT_EQ(hot[1], "nmm");
}

TEST(Learner, CodecFilterKeepsOnlyListedSources) {
  FileMeta fm;
  fm.branches.push_back(mkBranch("lzma_b", 1000, 100));
  fm.branches.push_back(mkBranch("zstd_b", 3000, 100));
  MockSource src;
  src.ranges[1000] = basketBytes(100, 500, 'X', 'Z'); // LZMA frame magic
  src.ranges[3000] = basketBytes(100, 500, 'Z', 'S'); // ZSTD frame magic
  EXPECT_EQ(branchCodec(fm, fm.branches[0], src), "lzma");
  EXPECT_EQ(branchCodec(fm, fm.branches[1], src), "zstd");

  auto lzmaOnly = deriveHotBranches(fm, src, {"lzma"});
  ASSERT_EQ(lzmaOnly.size(), 1u);
  EXPECT_EQ(lzmaOnly[0], "lzma_b");

  auto both = deriveHotBranches(fm, src, {"lzma", "zstd"});
  EXPECT_EQ(both.size(), 2u);

  auto unfiltered = deriveHotBranches(fm, src); // empty list = no filter
  EXPECT_EQ(unfiltered.size(), 2u);
}

TEST(Learner, UncompressedAndUnreadableExcludedByFilter) {
  FileMeta fm;
  fm.branches.push_back(mkBranch("raw_b", 1000, 130));
  fm.branches.push_back(mkBranch("garbage_b", 3000, 100));
  MockSource src;
  src.ranges[1000] = basketBytes(130, 100, 0, 0); // objlen == nbytes - keylen: raw
  src.ranges[3000] = std::vector<uint8_t>(100, 0xFF); // unparseable key
  EXPECT_EQ(branchCodec(fm, fm.branches[0], src), "none");
  EXPECT_EQ(branchCodec(fm, fm.branches[1], src), "");
  EXPECT_TRUE(deriveHotBranches(fm, src, {"lzma"}).empty());
  EXPECT_EQ(deriveHotBranches(fm, src).size(), 2u); // no filter: both qualify
}
