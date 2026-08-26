// The run-level reader and the gain estimator, against synthesized stats
// directories. Everything here is hermetic: no cache, no plugin, no origin.
//
// Several cases are NEGATIVE CONTROLS for specific ways the estimate could
// lie, and they are the point of the file: the replica tier must not be
// flattered by serving decompressed bytes, a cache that is SLOWER than the
// origin must say so rather than round up to 1, and every condition the
// estimate was not validated under must suppress it rather than caveat it.
#include "RunLog.h"

#include "TestUtil.h"
#include <fstream>
#include <gtest/gtest.h>
#include <string>

using namespace ucache;

namespace {

struct FileLine {
  std::string key;
  uint64_t served = 0, replica = 0, wire = 0, ts = 0;
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
      << ",\"first_touch_bytes\":" << f.served << ",\"wire_bytes\":" << f.wire << "}\n";
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
