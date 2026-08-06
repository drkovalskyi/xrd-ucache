#include "ReadRounding.h"

#include "FileEntry.h"
#include "TestUtil.h"
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <vector>

using namespace ucache;
using test::TempDir;

namespace {

constexpr uint64_t kP = 4096;
constexpr uint64_t kBig = 8ull * 1024 * 1024; // well above kMaxReadvElem

// Does [s,e) cover every page that a later read of [off,len) will ask for?
// hasRange() asks for pages [off/P, (off+len-1)/P] and writePages() stores only
// pages wholly inside the span, so this is exactly the cacheability condition.
bool coversPagesOf(std::pair<uint64_t, uint64_t> span, uint64_t off, uint64_t len,
                   uint64_t fileSize, uint64_t pageSize = kP) {
  const uint64_t first = off / pageSize;
  const uint64_t last = (off + len - 1) / pageSize;
  for (uint64_t i = first; i <= last; ++i) {
    const uint64_t pStart = i * pageSize;
    const uint64_t pEnd = std::min<uint64_t>(pStart + pageSize, fileSize);
    if (pStart < span.first || pEnd > span.second)
      return false;
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// The rule itself.
// ---------------------------------------------------------------------------

TEST(ReadRounding, SmallUnalignedRoundsBothEnds) {
  auto s = roundSpan(kP, 1u << 20, 1000, 5000);
  EXPECT_EQ(s.first, 0u);
  EXPECT_EQ(s.second, 2 * kP);
  EXPECT_TRUE(coversPagesOf(s, 1000, 5000, 1u << 20));
}

// The defect, stated as its fix: a single read far above the readv element
// ceiling still rounds, so every page it touches can be stored.
TEST(ReadRounding, LargeUnalignedWholeReadStillRounds) {
  const uint64_t fileSize = 32ull * 1024 * 1024;
  auto s = roundSpan(kP, fileSize, 1000, kBig);
  EXPECT_EQ(s.first % kP, 0u);
  EXPECT_EQ(s.second % kP, 0u);
  EXPECT_EQ(s.first, 0u);
  EXPECT_EQ(s.second, kBig + kP);
  EXPECT_TRUE(coversPagesOf(s, 1000, kBig, fileSize));
  // The whole cost of rounding: at most two pages.
  EXPECT_LE((s.second - s.first) - kBig, 2 * kP);
}

TEST(ReadRounding, AlreadyAlignedIsUnchanged) {
  const uint64_t fileSize = 32ull * 1024 * 1024;
  auto s = roundSpan(kP, fileSize, kP, kBig);
  EXPECT_EQ(s.first, kP);
  EXPECT_EQ(s.second, kP + kBig);
}

// At or past EOF the clamp puts end below start; the subtraction that follows
// must not wrap into a ~16 EiB span (which reached the caller as a wire length
// and a buffer allocation).
TEST(ReadRounding, PastEofDoesNotWrap) {
  const uint64_t fileSize = 100000;
  auto s = roundSpan(kP, fileSize, 500000, 10);
  EXPECT_EQ(s.first, 500000u);
  EXPECT_EQ(s.second, 500010u);
  EXPECT_EQ(s.second - s.first, 10u);
}

TEST(ReadRounding, EndClampsToFileSize) {
  const uint64_t fileSize = 100000; // not a page multiple
  auto s = roundSpan(kP, fileSize, 90000, 20000);
  EXPECT_EQ(s.first, 90000 / kP * kP);
  EXPECT_EQ(s.second, fileSize); // never rounds past EOF
}


// A rounded span must stay inside the uint32_t the wire read length is passed
// as; unroundable is a missed optimization, truncated is a wrong answer.
TEST(ReadRounding, HugeSpanFallsBackRatherThanTruncating) {
  const uint64_t fileSize = 8ull * 1024 * 1024 * 1024; // 8 GiB
  const uint64_t len = std::numeric_limits<uint32_t>::max();
  auto s = roundSpan(kP, fileSize, 1000, len);
  EXPECT_EQ(s.second - s.first, len);
  EXPECT_LE(s.second - s.first, std::numeric_limits<uint32_t>::max());
}

// ---------------------------------------------------------------------------
// Cutting a rounded span into legal vector-read elements. An element above the
// ceiling is REFUSED by the protocol -- the request fails and the reader gets an
// error -- so these are correctness properties, not sizing preferences.
// ---------------------------------------------------------------------------

namespace {

// Cut [s,e) the way issueMissVRead does.
std::vector<std::pair<uint64_t, uint64_t>> cutRun(uint64_t s, uint64_t e, uint64_t P) {
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (uint64_t at = s; at < e;) {
    const uint64_t cut = readvElemEnd(at, e, P);
    out.emplace_back(at, cut);
    at = cut;
  }
  return out;
}

} // namespace

TEST(ReadvCut, UnderCeilingIsOnePiece) {
  auto v = cutRun(0, kMaxReadvElem, kP);
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].first, 0u);
  EXPECT_EQ(v[0].second, kMaxReadvElem);
}

TEST(ReadvCut, EveryPieceIsLegalAlignedAndTilesTheRun) {
  const uint64_t P = kP, s = 0, e = 15ull * 1024 * 1024; // RNTuple cluster scale
  auto v = cutRun(s, e, P);
  ASSERT_GE(v.size(), 8u);
  uint64_t at = s;
  for (size_t i = 0; i < v.size(); ++i) {
    EXPECT_EQ(v[i].first, at) << "piece " << i << " must start where the last ended";
    EXPECT_LE(v[i].second - v[i].first, kMaxReadvElem) << "piece " << i << " is illegal";
    if (i + 1 < v.size()) {
      EXPECT_EQ(v[i].second % P, 0u) << "interior cut " << i << " must be page-aligned";
    }
    at = v[i].second;
  }
  EXPECT_EQ(at, e); // no gap, no overlap, no truncation
}

// The failure this fixes: one residual chunk larger than the ceiling used to be
// emitted as a single illegal element and the read failed outright.
TEST(ReadvCut, OversizedSingleChunkBecomesLegalPieces) {
  const uint64_t len = 3695786; // the byte count from the observed failure
  auto v = cutRun(620631956 / kP * kP, 620631956 / kP * kP + len + kP, kP);
  ASSERT_GE(v.size(), 2u);
  for (const auto& p : v)
    EXPECT_LE(p.second - p.first, kMaxReadvElem);
}

TEST(ReadvCut, PageLargerThanCeilingStillTerminates) {
  const uint64_t P = 4ull * 1024 * 1024; // hypothetical page above the ceiling
  auto v = cutRun(0, 3 * P, P);
  ASSERT_EQ(v.size(), 3u);
  for (const auto& p : v)
    EXPECT_EQ(p.second - p.first, P); // one page per element, never zero-length
}

TEST(ReadvCut, UnalignedStartStillProducesLegalPieces) {
  auto v = cutRun(1000, 1000 + 9ull * 1024 * 1024, kP);
  ASSERT_GE(v.size(), 4u);
  uint64_t at = 1000;
  for (const auto& p : v) {
    EXPECT_EQ(p.first, at);
    EXPECT_LE(p.second - p.first, kMaxReadvElem);
    at = p.second;
  }
  EXPECT_EQ(at, 1000 + 9ull * 1024 * 1024);
}

TEST(ReadRounding, LargePageSize) {
  const uint64_t P = 1024 * 1024; // 1 MiB pages: rounding can add 2 MiB
  const uint64_t fileSize = 64ull * 1024 * 1024;
  auto s = roundSpan(P, fileSize, P + 7, kBig);
  EXPECT_EQ(s.first, P);
  EXPECT_EQ(s.second % P, 0u);
  EXPECT_TRUE(coversPagesOf(s, P + 7, kBig, fileSize, P));
}

// ---------------------------------------------------------------------------
// The property that makes it matter: fetch what you can store, and the next
// identical read is a hit. This is the gate that did not exist -- reverting the
// fix leaves every other test green and fails this one.
// ---------------------------------------------------------------------------

namespace {

struct Fixture {
  TempDir td;
  RealIO io;
  Config cfg;
  Stats stats;
  UrlKey key = *UrlKey::parse("root://h//data/file.root");
  std::vector<uint8_t> src;

  explicit Fixture(uint64_t fileSize, uint32_t pageSize = kP) {
    cfg.cacheDir = td.path();
    cfg.pageSize = pageSize;
    src = test::randomBytes(fileSize, 4242);
  }

  std::shared_ptr<FileEntry> open() {
    return FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  }
};

} // namespace

TEST(ReadRounding, RoundedLargeReadIsServedBack) {
  const uint64_t fileSize = 32ull * 1024 * 1024;
  const uint64_t off = 1000, len = kBig; // unaligned, above the readv ceiling
  Fixture fx(fileSize);
  auto e = fx.open();
  ASSERT_TRUE(e);

  // Fetch exactly what the plugin would fetch for this single read...
  auto span = roundSpan(fx.cfg.pageSize, fileSize, off, len);
  e->writePages(span.first, span.second - span.first, fx.src.data() + span.first);
  e->flushBuffer(true);

  // ...and the identical read is now resident and correct.
  EXPECT_TRUE(e->hasRange(off, len));
  std::vector<uint8_t> buf(len);
  ASSERT_TRUE(e->readCached(off, len, buf.data()));
  EXPECT_EQ(0, memcmp(buf.data(), fx.src.data() + off, len));
}

// The failure mode itself, so the mechanism is pinned and not just the fix: an
// unrounded span loses its edge pages, the read misses, and a refetch of the
// same span stores nothing new -- so it misses on every pass, forever.
TEST(ReadRounding, UnroundedLargeReadIsPermanentlyUncacheable) {
  const uint64_t fileSize = 32ull * 1024 * 1024;
  const uint64_t off = 1000, len = kBig;
  Fixture fx(fileSize);
  auto e = fx.open();
  ASSERT_TRUE(e);

  e->writePages(off, len, fx.src.data() + off); // as if never rounded
  e->flushBuffer(true);
  EXPECT_FALSE(e->hasRange(off, len));
  EXPECT_FALSE(e->readCached(off, len, std::vector<uint8_t>(len).data()));

  // Refetching stages nothing: the interior is already resident and the two
  // edge pages are still partial. This is why re-running never clears it.
  const uint64_t before = fx.stats.pageWrites.load();
  e->writePages(off, len, fx.src.data() + off);
  e->flushBuffer(true);
  EXPECT_EQ(fx.stats.pageWrites.load(), before);
  EXPECT_FALSE(e->hasRange(off, len));
}
