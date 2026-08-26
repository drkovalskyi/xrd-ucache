// The run-level reader and the gain estimator, against synthesized stats
// directories. Everything here is hermetic: no cache, no plugin, no origin.
//
// Several cases are NEGATIVE CONTROLS for specific ways the estimate could
// lie, and they are the point of the file: the replica tier must not be
// flattered by serving decompressed bytes, a cache that is SLOWER than the
// origin must say so rather than round up to 1, and every condition the
// estimate was not validated under must suppress it rather than caveat it.
#include "Holdout.h"
#include "RunLog.h"

#include "TestUtil.h"
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

using namespace ucache;

namespace {

struct FileLine {
  std::string key;
  uint64_t served = 0, replica = 0, wire = 0, ts = 0;
  uint64_t spanUs = 0;
  std::string mode;
  // A constructor rather than an aggregate: the trailing fields arrived later
  // and every existing five-argument call site should keep compiling.
  FileLine(std::string k, uint64_t s = 0, uint64_t r = 0, uint64_t w = 0, uint64_t t = 0,
           uint64_t sp = 0, std::string m = {})
      : key(std::move(k)), served(s), replica(r), wire(w), ts(t), spanUs(sp),
        mode(std::move(m)) {}
};

// One process's pair of files, as the store would have left them.
void writeRun(const std::string& dir, const std::string& host, uint64_t pid, uint64_t startS,
              uint64_t endS, const std::string& counterFields,
              const std::vector<FileLine>& files) {
  const std::string stem = dir + "/" + host + "-" + std::to_string(pid) + "-" +
                           std::to_string(startS) + "-0";
  {
    std::ofstream o(stem + ".jsonl");
    o << "{\"ts\":" << endS << ",\"pid\":" << pid << "," << counterFields << "}\n";
  }
  if (files.empty())
    return;
  std::ofstream o(stem + ".files.jsonl");
  for (const auto& f : files)
    o << "{\"ts\":" << (f.ts ? f.ts : endS) << ",\"key\":\"" << f.key
      << "\",\"opens\":1,\"served_bytes\":" << f.served << ",\"ram_bytes\":0"
      << ",\"replica_bytes\":" << f.replica
      << ",\"disk_reads\":1,\"disk_seq\":0,\"disk_bytes\":" << f.served
      << ",\"first_touch_bytes\":" << f.served << ",\"wire_bytes\":" << f.wire
      << ",\"span_us\":" << f.spanUs << ",\"mode\":\"" << f.mode << "\"}\n";
}

constexpr uint64_t kGiB = 1ull << 30;
constexpr uint64_t kMiB = 1ull << 20;

std::string fillCounters(uint64_t originBytes, uint64_t faults = 0) {
  return "\"opens\":2,\"files_opened\":2,\"origin_bytes\":" + std::to_string(originBytes) +
         ",\"served_bytes\":" + std::to_string(originBytes) +
         ",\"hit_bytes\":0,\"crc_failures\":" + std::to_string(faults);
}

std::string warmCounters(uint64_t hitBytes, uint64_t replicaBytes = 0) {
  return "\"opens\":2,\"files_opened\":2,\"origin_bytes\":0,\"hit_bytes\":" +
         std::to_string(hitBytes) +
         ",\"replica_bytes_served\":" + std::to_string(replicaBytes) +
         ",\"served_bytes\":" + std::to_string(hitBytes + replicaBytes);
}

// A fill of two files taking 100 s, then a warm pass over the same two.
void twoFileFill(const std::string& dir, uint64_t durS = 100) {
  writeRun(dir, "host", 100, 1000, 1000 + durS, fillCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, kGiB / 2, 0}, {"root://o//b", kGiB / 2, 0, kGiB / 2, 0}});
}

// A measured BASELINE: the same two files read once with the cache out of the
// loop (UCACHE_DISABLE). This is the only kind of run the estimate accepts as
// a reference.
std::string baselineCounters(uint64_t relayBytes, uint64_t faults = 0) {
  return "\"disabled\":1,\"opens\":2,\"files_opened\":2,\"relay_bytes\":" +
         std::to_string(relayBytes) + ",\"origin_bytes\":0,\"hit_bytes\":0" +
         ",\"crc_failures\":" + std::to_string(faults);
}

void twoFileBaseline(const std::string& dir, uint64_t durS = 100, uint64_t pid = 50,
                     uint64_t startS = 500) {
  writeRun(dir, "host", pid, startS, startS + durS, baselineCounters(kGiB),
           {{"root://o//a", 0, 0, kGiB / 2, 0}, {"root://o//b", 0, 0, kGiB / 2, 0}});
}

} // namespace

TEST(RunLog, ParsesHyphenatedHostAndCounters) {
  test::TempDir td;
  writeRun(td.path(), "DESKTOP-NJ0BO90", 4242, 1700000000, 1700000060,
           "\"opens\":7,\"hit_bytes\":1234,\"origin_bytes\":99", {});
  auto runs = loadRuns(td.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].host, "DESKTOP-NJ0BO90"); // the dash in the host is not a delimiter
  EXPECT_EQ(runs[0].pid, 4242u);
  EXPECT_EQ(runs[0].startS, 1700000000u);
  EXPECT_EQ(runs[0].endS, 1700000060u);
  EXPECT_EQ(runs[0].durationS(), 60u);
  EXPECT_EQ(runs[0].opens, 7u);
  EXPECT_EQ(runs[0].hitBytes, 1234u);
  EXPECT_TRUE(runs[0].complete);
}

TEST(RunLog, LastCompleteLineWinsAndTornTailIgnored) {
  test::TempDir td;
  const std::string stem = td.path() + "/h-1-1000-0";
  std::ofstream o(stem + ".jsonl");
  o << "{\"ts\":1010,\"hit_bytes\":10}\n";
  o << "{\"ts\":1020,\"hit_bytes\":20}\n";
  o << "{\"ts\":1030,\"hit_by"; // killed mid-write
  o.close();
  auto runs = loadRuns(td.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].hitBytes, 20u); // the last COMPLETE line, not the torn one
  EXPECT_EQ(runs[0].endS, 1020u);
}

TEST(RunLog, CompanionsAreNotRuns) {
  test::TempDir td;
  writeRun(td.path(), "h", 1, 1000, 1100, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 0}});
  { std::ofstream o(td.path() + "/h-1-1000-0.trace.jsonl"); o << "{\"op\":\"key\"}\n"; }
  auto runs = loadRuns(td.path());
  EXPECT_EQ(runs.size(), 1u); // .files.jsonl and .trace.jsonl are not runs
  EXPECT_EQ(runs[0].files.size(), 1u);
  EXPECT_EQ(runs[0].files.at("root://o//a").servedBytes, kGiB);
}

TEST(RunLog, NewestFirstAndMissingDirIsEmpty) {
  test::TempDir td;
  writeRun(td.path(), "h", 1, 1000, 1100, warmCounters(kGiB), {});
  writeRun(td.path(), "h", 2, 3000, 3100, warmCounters(kGiB), {});
  auto runs = loadRuns(td.path());
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].startS, 3000u);
  EXPECT_TRUE(loadRuns(td.path() + "/does-not-exist").empty());
}

TEST(Gain, MeasuredBaselineOverWarmWhenFileSetsMatch) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_NEAR(g.gain, 2.0, 0.01); // 100 s with no cache against 50 s with it
  EXPECT_NEAR(g.savedS, 50.0, 0.01);
  EXPECT_EQ(g.matchedFiles, 2u);
  EXPECT_EQ(g.originEquivBytes, kGiB);
}

// THE REGRESSION THIS REDESIGN EXISTS FOR. On the ZSTD-1 RNTuple campaign the
// fill-as-reference estimate reported ~1.7x where the measured truth was
// 0.70x -- a sign flip, undetectable from records. A fill is NEVER a
// reference: with only a fill on record the estimate refuses and says how to
// measure a real baseline.
TEST(Gain, AFillIsNotAReference) {
  test::TempDir td;
  twoFileFill(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("no baseline recorded"), std::string::npos);
  EXPECT_NE(g.reason.find("UCACHE_DISABLE"), std::string::npos);
}

// The zstd1 shape end to end: baseline 175 s, warm 250 s. The answer is BELOW
// one, reported, never clamped and never suppressed -- a cache slower than the
// origin is the finding the user most needs told plainly.
TEST(Gain, ReportsBelowOneWhenTheCacheIsSlower) {
  test::TempDir td;
  twoFileBaseline(td.path(), 175);
  writeRun(td.path(), "host", 400, 2000, 2250, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_LT(g.gain, 1.0);
  EXPECT_NEAR(g.gain, 0.70, 0.01);
  EXPECT_LT(g.savedS, 0.0); // it COST time, and says so
}

// The replica tier serves DECOMPRESSED bytes -- twice the volume in the same
// time here -- and the answer must not move: the comparison is in
// origin-equivalent bytes, not bytes served.
TEST(Gain, UsesOriginEquivalentBytesNotBytesServed) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 300, 2000, 2050, warmCounters(0, 2 * kGiB),
           {{"root://o//a", kGiB, kGiB, 0, 0}, {"root://o//b", kGiB, kGiB, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_NEAR(g.gain, 2.0, 0.01);
  EXPECT_EQ(g.originEquivBytes, kGiB);
}

TEST(Gain, TheBaselineRunItselfIsNotEstimated) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("IS a baseline"), std::string::npos);
}

TEST(Gain, SuppressedForARunThatFilledTheCache) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2200, // fetched 1 GiB, served 8 MiB
           "\"opens\":2,\"origin_bytes\":" + std::to_string(kGiB) +
               ",\"hit_bytes\":" + std::to_string(8 * kMiB) + ",\"served_bytes\":" +
               std::to_string(kGiB),
           {{"root://o//a", kGiB / 2, 0, kGiB / 2, 0},
            {"root://o//b", kGiB / 2, 0, kGiB / 2, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("filled the cache"), std::string::npos);
}

TEST(Gain, SuppressedOnDisjointFileSets) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//x", kGiB / 2, 0, 0, 0}, {"root://o//y", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  EXPECT_FALSE(estimateGain(runs[0], runs).valid);
}

TEST(Gain, SuppressedOnPartialOverlap) {
  test::TempDir td;
  // The baseline covered four files; this run touched one of them. 25% is not
  // a comparison, and partial coverage is what has misled us before.
  writeRun(td.path(), "host", 100, 1000, 1100, baselineCounters(4 * kGiB),
           {{"root://o//a", 0, 0, kGiB, 0},
            {"root://o//b", 0, 0, kGiB, 0},
            {"root://o//c", 0, 0, kGiB, 0},
            {"root://o//d", 0, 0, kGiB, 0}});
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("different files"), std::string::npos);
}

TEST(Gain, SuppressedWhenTheBaselineHitFaults) {
  test::TempDir td;
  writeRun(td.path(), "host", 100, 1000, 1100, baselineCounters(kGiB, /*faults=*/3),
           {{"root://o//a", 0, 0, kGiB / 2, 0}, {"root://o//b", 0, 0, kGiB / 2, 0}});
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  EXPECT_FALSE(estimateGain(runs[0], runs).valid);
}

TEST(Gain, SuppressedBelowTheSizeFloor) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(8 * kMiB),
           {{"root://o//a", 4 * kMiB, 0, 0, 0}, {"root://o//b", 4 * kMiB, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("too little data"), std::string::npos);
}

// Whole-second records cannot time a short run well enough to divide by; the
// floor applies to the run AND to the baseline.
TEST(Gain, SuppressedWhenEitherRunIsTooShortToTime) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2020, warmCounters(kGiB), // 20 s
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("too short"), std::string::npos);

  test::TempDir td2;
  twoFileBaseline(td2.path(), 10); // a 10 s baseline is not a measurement
  writeRun(td2.path(), "host", 200, 2000, 2100, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs2 = loadRuns(td2.path());
  EXPECT_FALSE(estimateGain(runs2[0], runs2).valid);
}

// A baseline recorded AFTER the cached runs still measures them -- people
// usually think of it late -- and with several on record the NEAREST in time
// wins, because origin drift is the only thing separating baselines.
TEST(Gain, UsesTheNearestBaselineIncludingALaterOne) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100, /*pid=*/50, /*startS=*/500);   // far past: 100 s
  twoFileBaseline(td.path(), 200, /*pid=*/60, /*startS=*/4000);  // near future: 200 s
  writeRun(td.path(), "host", 200, 3000, 3050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  const ucache::Run* warm = nullptr;
  for (const auto& r : runs)
    if (r.startS == 3000)
      warm = &r;
  ASSERT_NE(warm, nullptr);
  auto g = estimateGain(*warm, runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_EQ(g.referenceStartS, 4000u); // nearest in time, though it came later
  EXPECT_NEAR(g.gain, 4.0, 0.01);
}

TEST(Totals, AggregatesBytesAndCountsFilesOnce) {
  test::TempDir td;
  twoFileFill(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto t = summarize(runs);
  EXPECT_EQ(t.runs, 2u);
  EXPECT_EQ(t.distinctFiles, 2u); // the UNION across runs, not 4
  EXPECT_EQ(t.originBytes, kGiB);
  EXPECT_EQ(t.cacheBytes(), kGiB);
  EXPECT_EQ(t.durationS, 150u);
  EXPECT_EQ(t.faults, 0u);
}

// The aggregate gain divides only by the spans of the runs it could measure.
// Folding in a baseline's or a fill's span would dilute the answer with time
// the cache was not serving anyone.
TEST(Totals, GainCoversOnlyTheRunsItMeasured) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto t = summarize(runs);
  ASSERT_TRUE(t.haveGain);
  EXPECT_EQ(t.runsEstimated, 1u); // not the baseline itself
  EXPECT_NEAR(t.savedS, 50.0, 0.01);
  EXPECT_NEAR(t.gain, 2.0, 0.01);
}

// A cache that hurts must show up as a NEGATIVE total, not vanish into an
// average with the runs it helped.
TEST(Totals, ANetLossIsReportedAsOne) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2400, warmCounters(kGiB), // 400 s vs 100 s
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto t = summarize(runs);
  ASSERT_TRUE(t.haveGain);
  EXPECT_LT(t.savedS, 0.0);
  EXPECT_LT(t.gain, 1.0);
  EXPECT_NEAR(t.gain, 0.25, 0.01);
}

TEST(Totals, EmptyHistoryIsNotAGain) {
  test::TempDir td;
  auto t = summarize(loadRuns(td.path()));
  EXPECT_EQ(t.runs, 0u);
  EXPECT_FALSE(t.haveGain);
  EXPECT_EQ(t.distinctFiles, 0u);
}

// `stats --reset` moves records into stats/history rather than deleting them;
// the reader sees both places as one history, and a stem present in both (a
// live process keeps appending at the old path after the move) counts once,
// with the LIVE copy winning.
TEST(RunLog, ReadsTheHistoryDirectoryAndDedupsByStem) {
  test::TempDir td;
  const std::string hist = td.path() + "/history";
  ::mkdir(hist.c_str(), 0700);
  writeRun(td.path(), "h", 1, 1000, 1100, warmCounters(kGiB), {});
  writeRun(hist, "h", 2, 3000, 3100, warmCounters(2 * kGiB), {});
  {
    std::ofstream o(hist + "/h-1-1000-0.jsonl"); // stale copy of the LIVE run
    o << "{\"ts\":1050,\"hit_bytes\":1}\n";
  }
  auto runs = loadRuns(td.path(), hist);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].startS, 3000u); // the archived run is a first-class run
  EXPECT_EQ(runs[1].hitBytes, kGiB); // live copy won over the stale duplicate
}

// ---------------------------------------------------------------------------
// In-run measurement (holdout files vs cached files, same process).
// ---------------------------------------------------------------------------

// A run that measured itself: `nCached` cached files and `nHoldout` held out,
// each side at the given µs-per-MB, at concurrency `conc`. The wall is DERIVED
// from the spans (Σspans / conc) so the run is self-consistent — a real slot
// scheduler produces that identity, and tests that want to break it pass
// `wallOverrideS`.
void measuredRun(const std::string& dir, uint64_t pid, uint64_t startS,
                 size_t nCached, double cachedUsPerMB, size_t nHoldout,
                 double holdoutUsPerMB, uint64_t conc = 32, uint64_t perFileMB = 64,
                 uint64_t wallOverrideS = 0) {
  const double totalUs = static_cast<double>(nCached) * cachedUsPerMB * perFileMB +
                         static_cast<double>(nHoldout) * holdoutUsPerMB * perFileMB;
  const uint64_t wallS =
      wallOverrideS ? wallOverrideS
                    : std::max<uint64_t>(1, (uint64_t)(totalUs / conc / 1e6 + 0.5));
  const uint64_t bytes = perFileMB << 20;
  std::vector<FileLine> files;
  std::string counters = "\"opens\":" + std::to_string(nCached + nHoldout) +
                         ",\"origin_bytes\":0,\"hit_bytes\":" +
                         std::to_string(bytes * nCached) + ",\"served_bytes\":" +
                         std::to_string(bytes * (nCached + nHoldout)) +
                         ",\"relay_bytes\":" + std::to_string(bytes * nHoldout) +
                         ",\"handles_high_water\":" + std::to_string(conc);
  for (size_t i = 0; i < nCached; ++i)
    files.push_back({"root://o//c" + std::to_string(i), bytes, 0, 0, 0,
                     (uint64_t)(cachedUsPerMB * perFileMB), "cached"});
  for (size_t i = 0; i < nHoldout; ++i)
    files.push_back({"root://o//h" + std::to_string(i), 0, 0, bytes, 0,
                     (uint64_t)(holdoutUsPerMB * perFileMB), "holdout"});
  writeRun(dir, "host", pid, startS, startS + wallS, counters, files);
}

TEST(InRun, MeasuresGainFromHeldOutFiles) {
  test::TempDir td;
  // Cached files move a MB in 100 µs, held-out ones in 250 µs -> 2.5x.
  // 40 files x 64 MiB, spans summing to ~1 thread-hour equivalent: pick a wall
  // and concurrency that put span coverage inside the band.
  measuredRun(td.path(), 1, 1000, /*nCached=*/36, /*cachedUsPerMB=*/80000.0,
              /*nHoldout=*/9, /*holdoutUsPerMB=*/200000.0, /*conc=*/8);
  auto runs = loadRuns(td.path());
  auto g = inRunGain(runs[0]);
  ASSERT_TRUE(g.valid) << g.reason << " (coverage " << g.spanCoverage << ")";
  EXPECT_NEAR(g.gain, 2.5, 0.01);
  EXPECT_EQ(g.holdoutFiles, 9u);
  EXPECT_EQ(g.cachedFiles, 36u);
}

// THE REGRESSION THIS METHOD EXISTS FOR. On the ZSTD-1 campaign the cache was
// SLOWER than the origin; every record-only model reported a gain. Held-out
// files finish FASTER than cached ones, and the answer is below 1.
TEST(InRun, ReportsBelowOneWhenTheCacheIsSlower) {
  test::TempDir td;
  measuredRun(td.path(), 1, 1000, 36, /*cached=*/200000.0, 9, /*holdout=*/140000.0, 8);
  auto runs = loadRuns(td.path());
  auto g = inRunGain(runs[0]);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_LT(g.gain, 1.0);
  EXPECT_NEAR(g.gain, 0.70, 0.01);
}

// The replica tier serves DECOMPRESSED bytes. Comparing served bytes would
// credit the cache for the expansion; only origin-equivalent bytes may count.
TEST(InRun, UsesOriginEquivalentBytesForTheReplicaTier) {
  test::TempDir td;
  const uint64_t mb = 1ull << 20;
  std::vector<FileLine> files;
  std::string counters = "\"opens\":45,\"origin_bytes\":0,\"hit_bytes\":0,"
                         "\"replica_bytes_served\":0,\"handles_high_water\":2";
  for (size_t i = 0; i < 36; ++i) {
    // 64 MiB origin-equivalent, served as 128 MiB decompressed, in 6400 µs.
    FileLine f{"root://o//c" + std::to_string(i), 128 * mb, 64 * mb, 0, 0, 6400000, "cached"};
    files.push_back(f);
  }
  for (size_t i = 0; i < 9; ++i)
    files.push_back({"root://o//h" + std::to_string(i), 0, 0, 64 * mb, 0, 16000000, "holdout"});
  // Σspans = 36*6400 + 9*16000 ms-equivalents; wall chosen for coverage ~1 at
  // concurrency 2.
  writeRun(td.path(), "host", 1, 1000, 1000 + 189, counters, files);
  auto runs = loadRuns(td.path());
  auto g = inRunGain(runs[0]);
  ASSERT_TRUE(g.valid) << g.reason;
  // 6400 µs / 64 MiB cached vs 16000 / 64 held out = 2.5x. Counting the 128 MiB
  // served would halve cached µs/MB and report 5x.
  EXPECT_NEAR(g.gain, 2.5, 0.01);
  EXPECT_LT(g.gain, 3.0);
}

TEST(InRun, RefusesWithoutHeldOutFiles) {
  test::TempDir td;
  measuredRun(td.path(), 1, 1000, 40, 80000.0, 0, 0.0, 8);
  auto runs = loadRuns(td.path());
  auto g = inRunGain(runs[0]);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("measure_permille"), std::string::npos);
}

TEST(InRun, RefusesOnTooFewFiles) {
  test::TempDir td;
  measuredRun(td.path(), 1, 1000, 36, 80000.0, 3, 200000.0, 8);
  auto runs = loadRuns(td.path());
  auto g = inRunGain(runs[0]);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("too few files"), std::string::npos);
}

// The composition self-check: when file spans do not account for the run's
// thread-time, spans are not comparable slices of the wall and the answer is
// refused rather than reported.
TEST(InRun, RefusesWhenSpansDoNotComposeIntoTheWall) {
  test::TempDir td;
  // Same spans, but the run took ten times as long as its file work accounts
  // for: coverage collapses far below the band.
  measuredRun(td.path(), 1, 1000, 36, 80000.0, 9, 200000.0, /*conc=*/8,
              /*perFileMB=*/64, /*wallOverrideS=*/4000);
  auto runs = loadRuns(td.path());
  auto g = inRunGain(runs[0]);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("spans account for"), std::string::npos);
}

TEST(InRun, RefusesADisabledRun) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100);
  auto runs = loadRuns(td.path());
  auto g = inRunGain(runs[0]);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("cache disabled"), std::string::npos);
}

TEST(Holdout, SelectionIsDeterministicAndRoughlyProportional) {
  size_t hits = 0;
  const size_t n = 20000;
  for (size_t i = 0; i < n; ++i) {
    const std::string k = "root://o//f" + std::to_string(i) + ".root";
    const bool a = holdoutSelected(k, 50, 0, 0);
    EXPECT_EQ(a, holdoutSelected(k, 50, 0, 0)); // same answer every time
    hits += a ? 1 : 0;
  }
  EXPECT_NEAR(static_cast<double>(hits) / n, 0.05, 0.01); // 50 per mille
  EXPECT_FALSE(holdoutSelected("root://o//f0.root", 0, 0, 0));   // off
  EXPECT_TRUE(holdoutSelected("root://o//f0.root", 1000, 0, 0)); // everything
}

TEST(Holdout, RotationChangesTheSetOverTime) {
  size_t moved = 0;
  const size_t n = 4000;
  for (size_t i = 0; i < n; ++i) {
    const std::string k = "root://o//f" + std::to_string(i) + ".root";
    // Same key, two different daily windows.
    if (holdoutSelected(k, 100, 86400, 0) != holdoutSelected(k, 100, 86400, 86400 * 3))
      ++moved;
  }
  EXPECT_GT(moved, n / 100); // the held-out set is not frozen
  // …and within one window it is stable.
  for (size_t i = 0; i < 100; ++i) {
    const std::string k = "root://o//f" + std::to_string(i) + ".root";
    EXPECT_EQ(holdoutSelected(k, 100, 86400, 1000), holdoutSelected(k, 100, 86400, 2000));
  }
}
