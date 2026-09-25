// ReadMap: the share of a file's data the branches a reader touches hold. The
// numbers are built by hand so each expectation can be checked on paper.
#include "ReadMap.h"

#include "RNTupleMeta.h"
#include "TreeMeta.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace ucache::transpose;

namespace {

// Five branches, four baskets each. Branch b's basket i sits at
// 1000 + 10000*b + 1000*i and is kBytes[b] long, so the branches hold
// 100, 100, 200, 400 and 1200 bytes: 2000 in all.
constexpr int kBytes[5] = {25, 25, 50, 100, 300};

FileMeta fiveBranches() {
  FileMeta fm;
  fm.entries = 200;
  for (int b = 0; b < 5; ++b) {
    BranchInfo br;
    br.name = "b" + std::to_string(b);
    br.entries = 200;
    br.writeBasket = 4;
    for (int i = 0; i < 4; ++i) {
      br.basketSeek.push_back(1000 + 10000 * b + 1000 * i);
      br.basketBytes.push_back(kBytes[b]);
      br.basketEntry.push_back(50 * i);
    }
    br.basketEntry.push_back(200);
    fm.branches.push_back(br);
  }
  return fm;
}

std::pair<uint64_t, uint64_t> basket(int b, int i) {
  return {static_cast<uint64_t>(1000 + 10000 * b + 1000 * i), static_cast<uint64_t>(kBytes[b])};
}

using Groups = std::vector<uint32_t>;

} // namespace

TEST(ReadMap, ARequestNamesTheBranchesItsBytesTouch) {
  auto m = ReadMap::fromTree(fiveBranches());
  EXPECT_EQ(m.groupsOf({basket(0, 0)}), Groups({0}));
  EXPECT_EQ(m.groupsOf({basket(3, 2), basket(0, 1), basket(3, 3)}), Groups({0, 3}));
  // Most of a basket names its branch too.
  auto [o, n] = basket(4, 1);
  EXPECT_EQ(m.groupsOf({{o + 10, n - 20}}), Groups({4}));
  EXPECT_TRUE(m.groupsOf({{o, n / 2 - 1}}).empty()) << "less than half";
  // One chunk over two baskets of one branch and the gap between them.
  EXPECT_EQ(m.groupsOf({{basket(2, 0).first, 1050}}), Groups({2}));
}

TEST(ReadMap, TheShareIsWhatTheBranchesHoldInTheFile) {
  auto m = ReadMap::fromTree(fiveBranches());
  auto s = m.weigh({0, 1});
  EXPECT_EQ(s.asked, 200u);
  EXPECT_EQ(s.total, 2000u);
  EXPECT_EQ(s.groups, 2u);
  s = m.weigh({0, 1, 2, 3});
  EXPECT_EQ(s.asked, 800u);
  s = m.weigh({4});
  EXPECT_EQ(s.asked, 1200u);
  EXPECT_EQ(s.groups, 1u);
}

TEST(ReadMap, ADirectDecisionAsSoonAsTheBranchesReadExceedTheLimit) {
  auto m = ReadMap::fromTree(fiveBranches());
  std::vector<uint32_t> seen;
  ReadMap::Share s;
  // ROOT's read cache learns a wide reader's branches one basket at a time.
  EXPECT_EQ(m.step(seen, {basket(0, 0)}, 25, s), -1);
  EXPECT_EQ(m.step(seen, {basket(1, 0)}, 25, s), -1);
  EXPECT_EQ(m.step(seen, {basket(2, 0)}, 25, s), -1) << "20% so far";
  EXPECT_EQ(m.step(seen, {basket(3, 0)}, 25, s), 1) << "40%: read directly, before any fill";
  EXPECT_EQ(s.asked, 800u);
  EXPECT_EQ(s.groups, 4u);
}

TEST(ReadMap, CachedOnceTheReaderComesBackToItsBranches) {
  auto m = ReadMap::fromTree(fiveBranches());
  std::vector<uint32_t> seen;
  ReadMap::Share s;
  EXPECT_EQ(m.step(seen, {basket(0, 0)}, 25, s), -1);
  EXPECT_EQ(m.step(seen, {basket(1, 0)}, 25, s), -1);
  // The first fill holds the two branches it learned: nothing new, cached.
  EXPECT_EQ(m.step(seen, {basket(0, 1), basket(1, 1)}, 25, s), 0);
  EXPECT_EQ(s.asked, 200u);
  // With no learning phase the first fill finds new branches and waits; the
  // next one decides.
  seen.clear();
  EXPECT_EQ(m.step(seen, {basket(0, 0), basket(1, 0), basket(2, 0)}, 25, s), -1);
  EXPECT_EQ(m.step(seen, {basket(0, 1), basket(1, 1), basket(2, 1)}, 25, s), 0) << "20%";
  // A single basket of a branch already read decides nothing.
  seen.clear();
  EXPECT_EQ(m.step(seen, {basket(0, 0)}, 25, s), -1);
  EXPECT_EQ(m.step(seen, {basket(0, 1)}, 25, s), -1);
}

TEST(ReadMap, AFillSplitIntoRequestsIsWeighedWhole) {
  // A fill cut short by the read cache's size holds the small branches; the
  // rest arrive in the next request, or a fill of many baskets goes out as
  // several vector reads. The first part alone would look narrow.
  auto m = ReadMap::fromTree(fiveBranches());
  std::vector<uint32_t> seen;
  ReadMap::Share s;
  EXPECT_EQ(m.step(seen, {basket(0, 0), basket(1, 0), basket(2, 0)}, 25, s), -1) << "20%, new";
  EXPECT_EQ(m.step(seen, {basket(3, 0), basket(4, 0)}, 25, s), 1) << "the rest: all of it";
}

TEST(ReadMap, TheLimitIsStrictAndAHundredCachesEverything) {
  auto m = ReadMap::fromTree(fiveBranches());
  EXPECT_FALSE(ReadMap::exceeds(m.weigh({0, 1, 2, 3}), 40)) << "equal to the limit stays cached";
  EXPECT_TRUE(ReadMap::exceeds(m.weigh({0, 1, 2, 3}), 39));
  EXPECT_FALSE(ReadMap::exceeds(m.weigh({0, 1, 2, 3, 4}), 100));
  std::vector<uint32_t> seen;
  ReadMap::Share s;
  EXPECT_EQ(m.step(seen, {basket(4, 0), basket(3, 0)}, 100, s), -1);
  EXPECT_EQ(m.step(seen, {basket(4, 1), basket(3, 1)}, 100, s), 0);
}

TEST(ReadMap, ABasketCountsWhenHalfOfItIsRead) {
  // ROOT's first read of a file is its 300-byte header, which runs into a
  // basket written right after it: that basket's branch is not being read.
  FileMeta fm = fiveBranches();
  fm.branches[4].basketSeek[0] = 218;
  auto m = ReadMap::fromTree(fm);
  EXPECT_TRUE(m.groupsOf({{0, 300}}).empty()) << "82 of 300 bytes";
  EXPECT_EQ(m.groupsOf({{0, 218 + 150}}), Groups({4})) << "half";
  EXPECT_TRUE(m.groupsOf({{0, 218 + 149}}).empty());
  // Split over two adjacent chunks, it is still read whole.
  EXPECT_EQ(m.groupsOf({{218, 100}, {318, 100}}), Groups({4}));
}

TEST(ReadMap, MetadataReadsTouchNoUnit) {
  auto m = ReadMap::fromTree(fiveBranches());
  EXPECT_TRUE(m.groupsOf({{0, 900}}).empty()) << "the file's own records, before the first basket";
  EXPECT_TRUE(m.groupsOf({{basket(0, 0).first + 25, 900}}).empty())
      << "the gap between two baskets";
  EXPECT_FALSE(m.touches(0, 900));
  EXPECT_TRUE(m.touches(basket(1, 3).first + 10, 5));
}

TEST(ReadMap, BasketsWithNoSeekOrInAnotherFileAreLeftOut) {
  FileMeta fm = fiveBranches();
  fm.branches[1].externalFile = true;
  fm.branches[2].basketSeek[0] = 0;
  auto m = ReadMap::fromTree(fm);
  EXPECT_TRUE(m.groupsOf({basket(1, 0)}).empty());
  const auto s = m.weigh({0, 1, 2});
  EXPECT_EQ(s.total, 2000u - 100u - 50u);
  EXPECT_EQ(s.asked, 100u + 150u) << "branch 1 elsewhere, branch 2's basket 0 has no seek";
}

TEST(ReadMap, RNTuplePagesWeighByColumnAndTheHeadNever) {
  // Two columns over two clusters; pages of 1000 bytes (+8 checksum on column 1).
  RNTupleMeta rm;
  auto page = [](uint64_t off, uint32_t n, bool ck) {
    PageInfo p;
    p.offset = off;
    p.nbytes = n;
    p.hasChecksum = ck;
    return p;
  };
  for (uint32_t cl = 0; cl < 2; ++cl)
    for (uint32_t col = 0; col < 2; ++col) {
      ColumnRange cr;
      cr.clusterId = cl;
      cr.columnId = col;
      const uint64_t base = 200000 + 100000 * cl + 10000 * col;
      cr.pages = {page(base, 1000, col == 1), page(base + 2000, 1000, col == 1)};
      rm.ranges.push_back(cr);
    }
  auto m = ReadMap::fromRNTuple(rm);
  EXPECT_EQ(m.groupsOf({{200000, 1000}}), Groups({0}));
  EXPECT_EQ(m.groupsOf({{200000, 1000}, {210000, 1008}}), Groups({0, 1}));
  const auto s = m.weigh({1});
  EXPECT_EQ(s.asked, 4 * 1008u) << "checksums included";
  EXPECT_EQ(s.total, 4 * 1000u + 4 * 1008u);
  EXPECT_TRUE(m.groupsOf({{0, 128 * 1024}}).empty()) << "the head block ROOT reads at open";
}

TEST(ReadMap, RNTupleFixtureParsesIntoAMap) {
  RNTupleMeta rm =
      parseRNTuple(std::string(UCACHE_TEST_DATA_DIR) + "/rntuple_fixture.root", "");
  ASSERT_TRUE(rm.error.empty()) << rm.error;
  auto m = ReadMap::fromRNTuple(rm);
  EXPECT_FALSE(m.empty());
  EXPECT_TRUE(m.isRNTuple());
  EXPECT_GT(m.memoryBytes(), 0u);
}

TEST(ReadMap, DataRunsMergeTouchingUnits) {
  FileMeta fm = fiveBranches();
  // Branch 0's four baskets back to back: one run.
  for (int i = 0; i < 4; ++i)
    fm.branches[0].basketSeek[i] = 500 + 25 * i;
  auto m = ReadMap::fromTree(fm);
  auto runs = m.dataRuns();
  ASSERT_FALSE(runs.empty());
  EXPECT_EQ(runs.front().first, 500u);
  EXPECT_EQ(runs.front().second, 600u);
  EXPECT_EQ(runs.size(), 1u + 16u) << "branch 0 merged, the other sixteen baskets apart";
}
