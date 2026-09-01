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
  uint64_t originSize = 0;
  std::string mode, readSig;
  // A constructor rather than an aggregate: the trailing fields arrived later
  // and every existing five-argument call site should keep compiling.
  FileLine(std::string k, uint64_t s = 0, uint64_t r = 0, uint64_t w = 0, uint64_t t = 0,
           uint64_t sp = 0, std::string m = {}, uint64_t osz = 0, std::string rs = {})
      : key(std::move(k)), served(s), replica(r), wire(w), ts(t), spanUs(sp),
        originSize(osz), mode(std::move(m)), readSig(std::move(rs)) {}
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
      << ",\"read_sig\":\"" << f.readSig << "\""
      << ",\"span_us\":" << f.spanUs << ",\"origin_size\":"
      << (f.originSize ? f.originSize : f.served) << ",\"mode\":\"" << f.mode << "\"}\n";
}


// An origin-wait histogram totalling roughly `seconds`. Bucket 20 is the log2
// bin around one second, and RunLog takes each bucket at 1.5x its lower edge.
std::string originHist(double seconds) {
  const long counts = static_cast<long>(seconds / 1.572864 + 0.5);
  std::string h = ",\"hist_origin_rt_us\":[";
  for (int i = 0; i < 21; ++i)
    h += (i ? "," : "") + std::string(i == 20 ? std::to_string(counts) : "0");
  return h + "]";
}

constexpr uint64_t kGiB = 1ull << 30;
constexpr uint64_t kMiB = 1ull << 20;

std::string fillCounters(uint64_t originBytes, uint64_t faults = 0) {
  return "\"opens\":2,\"files_opened\":2,\"origin_bytes\":" + std::to_string(originBytes) +
         ",\"served_bytes\":" + std::to_string(originBytes) +
         ",\"hit_bytes\":0,\"crc_failures\":" + std::to_string(faults);
}

// A file as a COLD pass leaves it. The plugin fetches every byte from the
// origin and hands most of them straight to the application; only re-reads of
// pages already staged go back through the byte tier. On a full campaign that
// was 1.3% of the wire bytes (1.9 GB served against 142.9 GB fetched), so a
// sixty-fourth is the right order and the fixture states it rather than
// implying it.
//
// Writing served == wire instead -- "during a fill the same bytes are both
// fetched and served, so the two counters agree" -- is a false model of what
// the plugin emits, and every fill fixture here carried it. It is the model
// under which taking the LARGER of served and wire looks equivalent to adding
// them, which is what let a warm replica run pass as a no-cache baseline.
FileLine filledFile(const std::string& key, uint64_t size, uint64_t ts = 0,
                    const std::string& sig = {}) {
  return FileLine(key, size / 64, 0, size, ts, 0, "fill", size, sig);
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
TEST(Gain, AFillNobodyCanJudgeIsNotAReference) {
  // A fill qualifies as a reference only when its record shows BOTH that the
  // origin did the work and that the cache's own writing was small against the
  // origin wait. This one carries no origin timing at all, so the second half
  // cannot be checked and it is refused -- the same answer the old rule gave
  // for every fill, now given for a reason.
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
// usually think of it late.
TEST(Gain, UsesABaselineRecordedAfterTheRun) {
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
  EXPECT_EQ(g.referenceStartS, 4000u); // recorded later, and still the one used
  EXPECT_NEAR(g.gain, 4.0, 0.01);
}

// With several baselines on record the LATEST wins, even when an older one sits
// closer in time to the run being measured.
//
// What separates two baselines is drift -- the same job against the same origin
// measured 173 s and 1025 s within one evening -- so the question is which one
// best describes conditions, and the most recent measurement is the best
// available estimate of that. There is no way to know the true value during any
// given run, which is why this is an estimator rather than a preference: a rule
// leaning toward the larger or the smaller wall would be choosing a bias.
//
// The two candidates here disagree on purpose. The older one is NEARER (100 s
// away against 2000 s) and would give 2.0; the later one gives 4.0. A test
// where both rules agree would pin nothing, which is what the test above does
// and why this one exists beside it.
TEST(Gain, WithSeveralBaselinesTheLatestWins) {
  test::TempDir td;
  twoFileBaseline(td.path(), 100, /*pid=*/50, /*startS=*/2900);  // near, older
  twoFileBaseline(td.path(), 200, /*pid=*/60, /*startS=*/5000);  // far, latest
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
  EXPECT_EQ(g.referenceStartS, 5000u) << "the nearer baseline is older; the later one is current";
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
// Identity and baseline qualification. A run is only comparable with another
// that did the same work, and only usable as a reference when it was close
// enough to a pure direct run -- bounded on BOTH deviations, because they push
// the answer opposite ways.
// ---------------------------------------------------------------------------

TEST(Signature, SameFilesAgreeAndAShortRunDoesNot) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, fillCounters(kGiB),
           {{"root://o//a", kGiB}, {"root://o//b", kGiB}});
  writeRun(d.path(), "h", 2, 2000, 2100, fillCounters(kGiB),
           {{"root://o//b", kGiB}, {"root://o//a", kGiB}}); // same set, other order
  writeRun(d.path(), "h", 3, 3000, 3100, fillCounters(kGiB),
           {{"root://o//a", kGiB}}); // died early: a DIFFERENT input set
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 3u);
  std::map<uint64_t, std::string> sig;
  for (const auto& r : runs)
    sig[r.pid] = r.sig;
  EXPECT_FALSE(sig[1].empty());
  EXPECT_EQ(sig[1], sig[2]) << "order of opening must not change the signature";
  EXPECT_NE(sig[1], sig[3]) << "a truncated run opened fewer files and is not comparable";
}

TEST(Signature, OriginHostComesFromTheUrl) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, fillCounters(kGiB),
           {{"root://siteA:1094//a", kGiB}, {"root://siteA:1094//b", kGiB},
            {"root://siteB:1094//c", kGiB}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].topOriginHost(), "siteA");
  EXPECT_EQ(runs[0].originHosts.size(), 2u);
}

TEST(Baseline, OriginShareIsWeightedByOriginSize) {
  test::TempDir d;
  // One big file from the origin, one small one served from cache: by FILE
  // COUNT that is 50%, by origin-equivalent bytes it is 90%. Bytes is the
  // honest weight -- a file's cost is its size, not its existence.
  writeRun(d.path(), "h", 1, 1000, 1100, fillCounters(9 * kGiB),
           {{"root://o//big", 0, 0, 9 * kGiB, 0, 0, "relay", 9 * kGiB},
            {"root://o//small", kGiB, 0, 0, 0, 0, "cached", kGiB}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_NEAR(runs[0].originShare(), 0.9, 0.01);
}

TEST(Baseline, OverheadIsWriteTimeOverOriginTime) {
  test::TempDir d;
  // The same fill reported at two widths. Overhead compares cache-write time
  // with ORIGIN-WAIT time, both measured inside this one run, so no thread
  // count enters and the two must agree. A width-normalised measure scaled one
  // of these by 18x and accepted a fill whose wall was 2.12x its own direct
  // reference.
  const std::string w = ",\"buffer_stall_us\":1230000000" + originHist(1000.0);
  writeRun(d.path(), "h", 1, 1000, 1100,
           fillCounters(kGiB) + w + ",\"threads_high_water\":32",
           {{"root://o//a", 0, 0, kGiB, 0, 0, "fill", kGiB}});
  writeRun(d.path(), "h", 2, 2000, 2100,
           fillCounters(kGiB) + w + ",\"threads_high_water\":584",
           {{"root://o//a", 0, 0, kGiB, 0, 0, "fill", kGiB}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 2u);
  for (const auto& r : runs) {
    EXPECT_NEAR(r.overhead(), 1.23, 0.02)
        << "threads_high_water " << r.threadsHighWater << " must not change it";
    EXPECT_FALSE(r.baselineQualified());
  }
}

TEST(Baseline, AQuietFillQualifiesAndALoudOneDoesNot) {
  test::TempDir d;
  // Both are ~100% origin-served, so coverage alone cannot separate them --
  // which is why the write-side bound exists. Real ratios from a campaign
  // whose direct references were known: 0.01 went with a 1.05x wall, 1.23
  // with 2.13x. The bound is the product's own cold-pass target, 1.1x.
  writeRun(d.path(), "h", 1, 1000, 1100,
           fillCounters(kGiB) + ",\"buffer_stall_us\":10000000" + originHist(1000.0),
           {{"root://o//a", 0, 0, kGiB, 0, 0, "fill", kGiB}});
  writeRun(d.path(), "h", 2, 2000, 2100,
           fillCounters(kGiB) + ",\"buffer_stall_us\":1230000000" + originHist(1000.0),
           {{"root://o//a", 0, 0, kGiB, 0, 0, "fill", kGiB}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 2u);
  for (const auto& r : runs) {
    EXPECT_NEAR(r.originShare(), 1.0, 0.01) << "both are origin-served";
    if (r.pid == 1) {
      EXPECT_NEAR(r.overhead(), 0.01, 0.005);
      EXPECT_TRUE(r.baselineQualified());
    } else {
      EXPECT_FALSE(r.baselineQualified());
    }
  }
}

TEST(Baseline, CoresBusyNeedsNoThreadCount) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100,
           fillCounters(kGiB) + ",\"cpu_us\":640000000", // 640 s over a 100 s wall
           {{"root://o//a", kGiB}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_NEAR(runs[0].coresBusy(), 6.4, 0.05);
}

TEST(Datasets, GroupedBySignatureAndCoverageIsByVolume) {
  test::TempDir d;
  // Two input sets. The second has one tiny run and one large one; counting
  // runs would call it half measured, counting BYTES says otherwise.
  writeRun(d.path(), "h", 1, 1000, 1100, fillCounters(kGiB),
           {{"root://o//a", kGiB}, {"root://o//b", kGiB}});
  writeRun(d.path(), "h", 2, 2000, 2100, fillCounters(kGiB), {{"root://o//c", kGiB}});
  const auto runs = loadRuns(d.path());
  const auto sets = byDataset(runs);
  ASSERT_EQ(sets.size(), 2u);
  size_t twoFile = 0, oneFile = 0;
  for (const auto& s : sets) {
    if (s.files == 2)
      ++twoFile;
    if (s.files == 1)
      ++oneFile;
    EXPECT_FALSE(s.sig.empty());
  }
  EXPECT_EQ(twoFile, 1u);
  EXPECT_EQ(oneFile, 1u);
}

// ---------------------------------------------------------------------------
// Read signatures. The input signature says which files a run opened; this
// says which PARTS of them it read. Two analyses over one dataset share the
// former and differ here, and only that difference stops their walls being
// compared as though they were the same work.
// ---------------------------------------------------------------------------

TEST(ReadSignature, SameWorkOnDifferentTiersAgrees) {
  test::TempDir d;
  // A baseline that relayed, and a warm run served entirely from the replica
  // tier. Different routes, different byte counts, same parts of the same
  // files read -- the footprints are recorded in ORIGIN coordinates precisely
  // so these two agree.
  writeRun(d.path(), "h", 1, 1000, 1100,
           "\"opens\":2,\"origin_bytes\":0,\"relay_bytes\":2147483648,\"disabled\":1",
           {{"root://o//a", 0, 0, kGiB, 0, 0, "relay", kGiB, "aa11"},
            {"root://o//b", 0, 0, kGiB, 0, 0, "relay", kGiB, "bb22"}});
  writeRun(d.path(), "h", 2, 2000, 2100,
           "\"opens\":2,\"origin_bytes\":0,\"replica_bytes_served\":2147483648",
           {{"root://o//a", kGiB, kGiB, 0, 0, 0, "cached", kGiB, "aa11"},
            {"root://o//b", kGiB, kGiB, 0, 0, 0, "cached", kGiB, "bb22"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_FALSE(runs[0].readSig.empty());
  EXPECT_EQ(runs[0].readSig, runs[1].readSig)
      << "the same work over two tiers must produce one read signature";
}

TEST(ReadSignature, DifferentAnalysisOverTheSameFilesDiffers) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, fillCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 0, 0, "cached", kGiB, "aa11"}});
  writeRun(d.path(), "h", 2, 2000, 2100, fillCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 0, 0, "cached", kGiB, "cc33"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].sig, runs[1].sig) << "same file set";
  EXPECT_NE(runs[0].readSig, runs[1].readSig) << "different parts read";
}

TEST(ReadSignature, PartialKnowledgeYieldsNoSignature) {
  test::TempDir d;
  // One file's footprint is missing (a replica built before the map existed).
  // Half a signature would compare different work, so there is none.
  writeRun(d.path(), "h", 1, 1000, 1100, fillCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 0, 0, "cached", kGiB, "aa11"},
            {"root://o//b", kGiB, 0, 0, 0, 0, "cached", kGiB, ""}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_TRUE(runs[0].readSig.empty());
}

TEST(Gain, RefusesABaselineThatReadDifferentParts) {
  test::TempDir d;
  // Same files, same sizes, same durations -- but the baseline ran a different
  // analysis. Matching on the file set alone would happily report a speedup.
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1",
           {{"root://o//a", 0, 0, kGiB, 1200, 0, "relay", kGiB, "zz99"}});
  writeRun(d.path(), "h", 2, 2000, 2100,
           "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824",
           {{"root://o//a", kGiB, 0, 0, 2100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  const ucache::Run* warm = nullptr;
  for (const auto& r : runs)
    if (!r.disabled)
      warm = &r;
  ASSERT_TRUE(warm);
  const auto g = estimateGain(*warm, runs);
  EXPECT_FALSE(g.valid) << "gain " << g.gain;
  EXPECT_NE(g.reason.find("read the same parts of only"), std::string::npos) << g.reason;
  EXPECT_NE(g.reason.find("different work"), std::string::npos) << g.reason;
}

// ---- reading the same parts, file by file --------------------------------
//
// A replica is a rewritten container, so the reader asks it for different
// spans than it asks the original. Where only part of a file was relocated, a
// read over the original layout can cross a stretch the replica is never asked
// for, and a whole bucket lands inside that stretch on a few percent of files.
// That is an honest disagreement about a file, not evidence of different work
// -- so agreement is counted per file, and a run is comparable when nearly all
// of them agree.

namespace {

// n files, the first `differ` of which the baseline read differently.
void writeAgreementPair(const std::string& dir, size_t n, size_t differ) {
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < n; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    // Built, not formatted into a fixed buffer: a size_t can be twenty digits
    // and the compiler is right to say so, whatever this loop's bound happens
    // to be. The optimiser proves the bound in a release build and not in a
    // debug one, so a fixed buffer here compiles in one and fails the other.
    const std::string same = "s" + std::to_string(i);
    const std::string other = "x" + std::to_string(i);
    base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, i < differ ? other : same));
    warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, same));
  }
  writeRun(dir, "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1", base);
  writeRun(dir, "h", 2, 2000, 2100, "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824",
           warm);
}

ucache::GainEstimate gainOfWarm(const std::string& dir) {
  const auto runs = loadRuns(dir);
  const ucache::Run* warm = nullptr;
  for (const auto& r : runs)
    if (!r.disabled)
      warm = &r;
  EXPECT_TRUE(warm);
  return warm ? estimateGain(*warm, runs) : ucache::GainEstimate{};
}

} // namespace

TEST(ReadAgreement, AFewFilesReadDifferentlyDoNotVetoTheRun) {
  // The measured case: 26 files of 439 disagreed between the byte tier and the
  // replica, i.e. 94% agreement. Under a whole-run signature that refused the
  // entire measurement.
  test::TempDir d;
  writeAgreementPair(d.path(), 100, 6);
  const auto g = gainOfWarm(d.path());
  EXPECT_TRUE(g.valid) << g.reason;
}

TEST(ReadAgreement, ADifferentAnalysisIsStillRefused) {
  // Two analyses over the same inputs read different columns and so disagree
  // on essentially every file -- nowhere near the threshold.
  test::TempDir d;
  writeAgreementPair(d.path(), 100, 100);
  const auto g = gainOfWarm(d.path());
  EXPECT_FALSE(g.valid) << "gain " << g.gain;
  EXPECT_NE(g.reason.find("read the same parts of only"), std::string::npos) << g.reason;
  EXPECT_NE(g.reason.find("0%"), std::string::npos) << g.reason;
}

TEST(ReadAgreement, TheThresholdHoldsOnBothSides) {
  {
    test::TempDir d;
    writeAgreementPair(d.path(), 100, 10); // exactly 90% -- comparable
    EXPECT_TRUE(gainOfWarm(d.path()).valid);
  }
  {
    test::TempDir d;
    writeAgreementPair(d.path(), 100, 11); // 89% -- not
    const auto g = gainOfWarm(d.path());
    EXPECT_FALSE(g.valid);
    EXPECT_NE(g.reason.find("89%"), std::string::npos) << g.reason;
  }
}

TEST(ReadAgreement, ASingleFileStillHasToMatchExactly) {
  // With one comparable file the fraction can only be 0 or 1, so the rule is
  // as strict as it ever was. Nothing is loosened for small runs.
  test::TempDir d;
  writeAgreementPair(d.path(), 1, 1);
  EXPECT_FALSE(gainOfWarm(d.path()).valid);
}

TEST(ReadAgreement, FilesWithoutAFootprintCountForNeitherSide) {
  // A replica built before the map existed contributes no signature. Those
  // files are UNKNOWN: not agreement (which would admit a different analysis)
  // and not disagreement (which would refuse over no evidence). With every
  // file unknown the file-set match stands on its own, as it did before
  // signatures existed.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < 10; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, ""));
    warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, ""));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1", base);
  writeRun(d.path(), "h", 2, 2000, 2100, "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824",
           warm);
  EXPECT_TRUE(gainOfWarm(d.path()).valid);
}

TEST(ReadAgreement, ABaselineThatSignsNothingIsNotSilentlyTrusted) {
  // The work-identity check needs a signature from BOTH runs, so a baseline
  // that signs nothing disables it entirely. It is reachable: a run with the
  // cache switched off may fail to learn the sizes the signature is expressed
  // in, per file, so one teardown can leave a baseline largely unsigned.
  //
  // The answer is NOT to refuse. Refusing made the result non-monotone in
  // evidence -- see AddingOneSignatureNeverDestroysAMeasurement below. The
  // answer is that the comparison says of itself that nothing was checked.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < 20; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, "")); // signs nothing
    warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, "aaaa"));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1", base);
  writeRun(d.path(), "h", 2, 2000, 2100, "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824",
           warm);
  const auto g = gainOfWarm(d.path());
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_FALSE(g.workVerified) << "no pair could be checked, so nothing was verified";
  EXPECT_EQ(g.sigPairs, 0u);
}

TEST(ReadAgreement, OneCheckableFileCannotVouchForFourHundred) {
  // One agreeing pair out of four hundred is not evidence about the other
  // three hundred and ninety-nine, so the comparison must not call itself
  // verified on the strength of it. It is still reported -- absence of
  // evidence is not evidence of different work -- but for what it is.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < 400; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    const std::string sig = i < 1 ? "aaaa" : ""; // one checkable pair, and it AGREES
    base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, sig));
    warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, sig));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1", base);
  writeRun(d.path(), "h", 2, 2000, 2100, "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824",
           warm);
  const auto g = gainOfWarm(d.path());
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_FALSE(g.workVerified) << "one agreeing pair cannot vouch for 400 files";
  EXPECT_EQ(g.sigPairs, 1u);
}

TEST(ReadAgreement, AddingOneSignatureNeverDestroysAMeasurement) {
  // MONOTONICITY, as a property. More evidence must never produce a worse
  // answer, and the first version of the coverage rule broke exactly that: it
  // let a directory with NO signatures anywhere be compared, and refused the
  // same directory once a single file in it carried one. Measured on a real
  // archive, adding one signature to one file of a thousand deleted every
  // measurement in the directory.
  //
  // The pair here is identical but for one signed file on each side.
  auto build = [](const std::string& dir, size_t signedFiles) {
    std::vector<FileLine> base, warm;
    for (size_t i = 0; i < 40; ++i) {
      const std::string key = "root://o//f" + std::to_string(i);
      const std::string sig = i < signedFiles ? "aaaa" : "";
      base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, sig));
      warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, sig));
    }
    writeRun(dir, "h", 1, 1000, 1200,
             "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1", base);
    writeRun(dir, "h", 2, 2000, 2100,
             "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824", warm);
  };
  double none = 0.0, one = 0.0;
  {
    test::TempDir d;
    build(d.path(), 0);
    const auto g = gainOfWarm(d.path());
    ASSERT_TRUE(g.valid) << "nothing signed is the pre-signature case: " << g.reason;
    none = g.gain;
  }
  {
    test::TempDir d;
    build(d.path(), 1);
    const auto g = gainOfWarm(d.path());
    EXPECT_TRUE(g.valid) << "one signature must not delete the measurement: " << g.reason;
    one = g.gain;
  }
  EXPECT_NEAR(none, one, 1e-9) << "and it must not change the answer either";
}

TEST(ReadAgreement, RecordsFromBeforeSignaturesExistedStillMeasureButSaySo) {
  // Neither side signs anything, which is every record written before the
  // footprint existed. Refusing those would retire every measurement taken
  // before the feature, so they are still compared -- but the estimate must
  // not imply a check happened. A reader who cannot tell the difference
  // between "verified" and "unverifiable" has been told the stronger thing.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < 20; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, ""));
    warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, ""));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1", base);
  writeRun(d.path(), "h", 2, 2000, 2100, "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824",
           warm);
  const auto g = gainOfWarm(d.path());
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_FALSE(g.workVerified) << "nothing signed, so nothing was verified";
  EXPECT_EQ(g.sigPairs, 0u);
}

TEST(ReadAgreement, AFullySignedComparisonReportsItselfVerified) {
  // The other side of the same claim: when every file signs on both sides the
  // check really did run, and the estimate says so. Without this the flag
  // could be wired to a constant false and nothing would notice.
  test::TempDir d;
  writeAgreementPair(d.path(), 100, 0);
  const auto g = gainOfWarm(d.path());
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_TRUE(g.workVerified);
  EXPECT_EQ(g.sigPairs, 100u);
}

// NOT TESTED, DELIBERATELY: the coverage floor is a cliff. Adding one more
// AGREEING pair can take coverage over the line, switch the agreement rule on,
// and turn a valid comparison into a refusal. A test pinning the opposite was
// written and then removed with the change it guarded, because closing the
// cliff broke the mixed-era archive in leg 13 and refused honest comparisons at
// this project's own measured benign disagreement rate. The reasoning is beside
// the rule in RunLog.cc; this note exists so the absence looks deliberate
// rather than forgotten.

TEST(ReadAgreement, TheVerifiedCountIsOverTheFilesTheRuleActuallyUsed) {
  // Two counts exist and they are not the same. `matchedFiles` is the plain
  // intersection of the two runs' file sets; `comparedFiles` is the subset the
  // coverage rule runs against, which skips files the REFERENCE never fetched
  // over the wire, because one it never fetched says nothing about what the
  // origin cost. comparedFiles <= matchedFiles always.
  //
  // The readout printed sigPairs over matchedFiles, so a comparison that had
  // cleared the half-way bar could display a ratio below it -- telling the
  // reader the check was weaker than the tool required of itself. Two
  // reviewers found that by reading the code, and nothing in the suite would
  // have caught a third pass reintroducing it.
  //
  // Here 20 files match and the reference fetched 16 of them; all 16 sign on
  // both sides. The rule sees 16 of 16 -- full coverage -- while the
  // intersection is 20, so printing over it would show 16 of 20 and understate
  // the check. The gap cannot be made wider than this: the file-overlap floor
  // already refuses a comparison whose matched work falls below 70%, so the
  // two counts can differ by at most that much.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < 20; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    const bool fetched = i < 16;
    const std::string sig = fetched ? "aaaa" : "";
    base.push_back(FileLine(key, 0, 0, fetched ? kGiB : 0, 1200, 0, "relay",
                            fetched ? kGiB : 0, sig));
    warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, sig));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":8589934592,\"disabled\":1", base);
  writeRun(d.path(), "h", 2, 2000, 2100,
           "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":21474836480", warm);
  const auto g = gainOfWarm(d.path());
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_EQ(g.comparedFiles, 16u) << "the rule's denominator counts only files the reference fetched";
  EXPECT_EQ(g.matchedFiles, 20u) << "the intersection is the larger, different count";
  EXPECT_LT(g.comparedFiles, g.matchedFiles) << "the two counts are not interchangeable";
  EXPECT_EQ(g.sigPairs, 16u);
  EXPECT_TRUE(g.workVerified) << "16 of 16 is full coverage";
}

TEST(ReadAgreement, UnknownFilesCannotDiluteADisagreement) {
  // Half the files carry signatures; of those, 44 of 50 agree, so agreement
  // reads 88% and the comparison is refused. Counting the 50 unknown files as
  // agreement would read 94% and admit it -- on the strength of files nobody
  // has any evidence about.
  //
  // The ratios are picked to sit on OPPOSITE sides of the threshold, so the
  // test pins the choice rather than merely passing: a split where both
  // readings refuse anyway would prove nothing. Coverage is exactly at the
  // floor for the same reason -- the point here is dilution, and a shortage of
  // checkable files is refused earlier by a different rule, which would make
  // this a test of that rule instead.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < 100; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    const bool known = i < 50;
    const bool differs = known && i < 6;
    const std::string bs = known ? "aaaa" : "";
    const std::string ws = known ? (differs ? "bbbb" : "aaaa") : "";
    base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, bs));
    warm.push_back(FileLine(key, kGiB, 0, 0, 2100, 0, "cached", kGiB, ws));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824,\"disabled\":1", base);
  writeRun(d.path(), "h", 2, 2000, 2100, "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824",
           warm);
  EXPECT_FALSE(gainOfWarm(d.path()).valid);
}

// ---- a reference need not be a special run -------------------------------
//
// The rule is "the cache did not appreciably help this run", not "somebody
// remembered to set UCACHE_DISABLE". A first pass over data the cache does not
// have yet meets it, and so does a run that was a MIXTURE -- some files
// relayed straight through, some fetched from the origin, some partly each.
// Deciding per file, all or nothing, made a run that was half cache look
// entirely direct.

TEST(Baseline, AQuietFillIsUsedAsTheReference) {
  test::TempDir d;
  // A cold pass: every byte came from the origin, and the cache's own writing
  // cost 5% of the time spent waiting on the origin -- inside the cold-pass
  // budget the product already promises.
  writeRun(d.path(), "h", 1, 1000, 1200,
           fillCounters(kGiB) + ",\"buffer_stall_us\":50000000" + originHist(1000.0),
           {filledFile("root://o//a", kGiB, 1200, "aa11")});
  writeRun(d.path(), "h", 2, 2000, 2100, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 2100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  const ucache::Run* warm = nullptr;
  for (const auto& r : runs)
    if (!r.disabled && r.warm())
      warm = &r;
  ASSERT_TRUE(warm);
  const auto g = estimateGain(*warm, runs);
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_NEAR(g.gain, 2.0, 0.05) << "200 s of origin against a 100 s warm pass";
}

TEST(Baseline, ALoudFillIsStillRefused) {
  // Same shape, but the cache spent 60% of the origin wait on its own writes.
  // Its wall is its own cost, so dividing by it would overstate the gain.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1200,
           fillCounters(kGiB) + ",\"buffer_stall_us\":600000000" + originHist(1000.0),
           {filledFile("root://o//a", kGiB, 1200, "aa11")});
  writeRun(d.path(), "h", 2, 2000, 2100, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 2100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  const ucache::Run* warm = nullptr;
  for (const auto& r : runs)
    if (!r.disabled && r.warm())
      warm = &r;
  ASSERT_TRUE(warm);
  EXPECT_FALSE(estimateGain(*warm, runs).valid);
}

TEST(Baseline, OriginShareCountsBytesNotFiles) {
  test::TempDir d;
  // Two files, each delivered half from the origin and half from the byte
  // tier. That is a run the cache did half the work for, and it must read as
  // 50% -- not as 100% because "most of each file" came off the wire.
  //
  // Half and half is written as EQUAL served and wire counts because the two
  // are disjoint: a byte counted in one is never counted in the other, so a
  // file delivered half each way carries the same number in both. An earlier
  // version of this test wrote served=1 GiB against wire=0.5 GiB and called it
  // half -- that record actually describes 1.5 GiB delivered, a third of it
  // from the origin, and asserting 0.50 on it pinned the arithmetic to the
  // defect rather than to the contract.
  writeRun(d.path(), "h", 1, 1000, 1100, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, kGiB / 2, 0, 0, "cached", kGiB},
            {"root://o//b", kGiB / 2, 0, kGiB / 2, 0, 0, "cached", kGiB}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_NEAR(runs[0].originShare(), 0.50, 0.01);
  EXPECT_FALSE(runs[0].baselineQualified()) << "half the work came from the cache";
}

TEST(Baseline, EveryTierCountsTowardsWhatWasDelivered) {
  // The three counters are written in three different places and none of them
  // may be dropped. A file served from the replica tier with a little origin
  // refetch is a third of each here, so any arithmetic that ignores one tier
  // reports a half or a whole instead.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, warmCounters(kGiB),
           {{"root://o//a", kGiB, kGiB, kGiB, 0, 0, "cached", 3 * kGiB}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_NEAR(runs[0].originShare(), 1.0 / 3.0, 0.01)
      << "served, replica and wire are disjoint, so delivered is their sum";
}

TEST(Baseline, ARunTheCacheServedMostOfIsNotABaseline) {
  // The SECOND door into the same failure, and the one the byte-share test
  // exists to shut. originShare weights each file by its size AT THE ORIGIN,
  // so it is a size-weighted count of FILES, not a share of bytes: a file
  // touched for 5 MB counts exactly as much as a file read whole from the
  // cache. Underneath a passing 0.95 the cache's byte contribution is
  // unbounded.
  //
  // Here 380 files of 4 GiB are touched for 4 MiB each from the origin and 20
  // are read whole from the byte tier. That is 0.95 by file size and about 96%
  // of the BYTES from the cache -- and it was accepted as the measure of
  // running without a cache, which made the genuinely warm run over the same
  // files report a loss.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  const uint64_t sz = 4 * kGiB;
  for (size_t i = 0; i < 400; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    if (i < 380)
      base.push_back(FileLine(key, 0, 0, 4 * kMiB, 1200, 0, "fill", sz, "aa11"));
    else
      base.push_back(FileLine(key, sz, 0, 0, 1200, 0, "cached", sz, "aa11"));
    warm.push_back(FileLine(key, sz, 0, 0, 2100, 0, "cached", sz, "aa11"));
  }
  const uint64_t baseOrigin = 380 * 4 * kMiB, baseHit = 20 * sz;
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":" + std::to_string(baseOrigin) +
               ",\"hit_bytes\":" + std::to_string(baseHit) + ",\"served_bytes\":" +
               std::to_string(baseHit) + originHist(1000.0),
           base);
  writeRun(d.path(), "h", 2, 2000, 2100,
           "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":" + std::to_string(400 * sz), warm);
  const auto runs = loadRuns(d.path());
  const ucache::Run* ref = nullptr;
  for (const auto& r : runs)
    if (r.startS == 1000)
      ref = &r;
  ASSERT_TRUE(ref);
  EXPECT_GE(ref->originShare(), 0.95) << "by file size it looks like a no-cache run";
  EXPECT_FALSE(ref->servedLessThanFetched())
      << "but the cache delivered most of the bytes";
  EXPECT_FALSE(ref->baselineQualified())
      << "a run the cache served must never define what no cache costs";
  EXPECT_FALSE(gainOfWarm(d.path()).valid) << "so the warm run gets no number from it";
}

TEST(Baseline, AQuietFillStillQualifiesUnderTheByteTest) {
  // The control for the leg above: the byte test must not disqualify the fills
  // it was never aimed at. A real cold pass fetches far more than it serves --
  // measured at 75x on recorded campaigns -- so this has enormous margin, and
  // a test that only proved the refusal would leave that unstated.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1200,
           fillCounters(kGiB) + ",\"buffer_stall_us\":50000000" + originHist(1000.0),
           {filledFile("root://o//a", kGiB, 1200, "aa11")});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_TRUE(runs[0].servedLessThanFetched());
  EXPECT_TRUE(runs[0].baselineQualified());
}

TEST(Baseline, AWarmReplicaRunIsNotABaseline) {
  // THE defect this arithmetic exists to prevent, in the shape the field
  // produces it: recompressed entries serve nearly everything from the replica
  // tier, and under reclaim=full the byte copy is punched, so the few bytes
  // the replica does not cover come from the ORIGIN rather than from the byte
  // tier. Comparing served against wire scores that file as entirely origin.
  //
  // A run scored that way qualifies as the reference every other run is
  // measured against -- so the cache's own best result becomes the definition
  // of "no cache", and the runs it should be beating report a LOSS. The
  // failure direction is understatement, which is the one direction a reader
  // has no reason to doubt.
  test::TempDir d;
  std::vector<FileLine> f;
  for (size_t i = 0; i < 8; ++i)
    f.push_back(FileLine("root://o//f" + std::to_string(i), 0, kGiB, kGiB / 8192, 0, 0,
                         "cached", kGiB));
  writeRun(d.path(), "h", 1, 1000, 1100,
           warmCounters(8 * kGiB) + originHist(1.0), f);
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_LT(runs[0].originShare(), 0.01) << "the replica tier delivered essentially all of it";
  EXPECT_FALSE(runs[0].baselineQualified())
      << "a run served by the cache can never be the measure of running without one";
}

TEST(Baseline, AMixtureOfRelayedAndFilledFilesQualifies) {
  test::TempDir d;
  // The shape a real first run has: some files the cache declined and relayed
  // straight through, the rest fetched from the origin and kept. Nothing was
  // served from cache, so the origin did all of the work whatever the mix.
  std::vector<FileLine> f;
  for (size_t i = 0; i < 10; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    const bool relayed = i % 2 == 0;
    f.push_back(relayed ? FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, "aa11")
                        : filledFile(key, kGiB, 1200, "aa11"));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           fillCounters(kGiB) + ",\"buffer_stall_us\":50000000" + originHist(1000.0), f);
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  // Both routes are the origin; the small shortfall from 1.00 is the fill's own
  // re-reads of staged pages, which the cache really did deliver.
  EXPECT_GT(runs[0].originShare(), 0.98) << "relayed and filled are both the origin";
  EXPECT_TRUE(runs[0].baselineQualified());
}

TEST(Baseline, ARunTooThinToJudgeDoesNotQualify) {
  // No origin timing in the record, so the cache's write cost cannot be put
  // against anything. Qualifying here would be qualifying on an absence of
  // evidence, which is how the guard was silently passing everything.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1200, fillCounters(kGiB) + ",\"buffer_stall_us\":50000000",
           {filledFile("root://o//a", kGiB, 1200, "aa11")});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_GT(runs[0].originShare(), 0.98);
  EXPECT_FALSE(runs[0].overheadKnown());
  EXPECT_FALSE(runs[0].baselineQualified());
}

TEST(Baseline, ASwitchedOffRunBeatsANearerFill) {
  // Both qualify, and the fill is much nearer in time. The switched-off run
  // must still win: it carries none of the cache's own writing in its wall,
  // and admitting fills must not quietly revise a number that was already
  // reported from a clean baseline.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1200, baselineCounters(kGiB),
           {{"root://o//a", 0, 0, kGiB, 1200, 0, "relay", kGiB, "aa11"}});
  writeRun(d.path(), "h", 2, 5000, 5200,
           fillCounters(kGiB) + ",\"buffer_stall_us\":50000000" + originHist(1000.0),
           {{"root://o//a", kGiB, 0, kGiB, 5200, 0, "fill", kGiB, "aa11"}});
  writeRun(d.path(), "h", 3, 6000, 6100, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 6100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  const ucache::Run* warm = nullptr;
  for (const auto& r : runs)
    if (r.warm())
      warm = &r;
  ASSERT_TRUE(warm);
  const auto g = estimateGain(*warm, runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_EQ(g.referenceStartS, 1000u) << "the switched-off run, not the nearer fill";
}

TEST(RunLog, AMalformedHistogramIsDroppedNotSpun) {
  // One unexpected byte inside a histogram used to consume nothing per pass
  // while appending a bucket every time: the CLI hung and grew without bound.
  // A histogram that cannot be read is worth nothing, so it is dropped and the
  // rest of the line still parses.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100,
           "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824,"
           "\"hist_origin_rt_us\":[1,2,x,4],\"threads_high_water\":32",
           {{"root://o//a", kGiB, 0, 0, 0, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path()); // must return at all
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].originWaitS(), 0.0) << "an unreadable histogram counts as no evidence";
  EXPECT_EQ(runs[0].threadsHighWater, 32u) << "the rest of the line still parses";
}

// ---- the thresholds themselves ------------------------------------------
//
// Each of these pins a CONSTANT at its boundary, not merely the machinery that
// reads it. A test that only exercises values far from the edge lets the
// constant drift: moving the overhead bound from a tenth to a half broke
// nothing in this suite, which meant the number nobody had agreed to was as
// well defended as the one everybody had.

TEST(Thresholds, OverheadBoundSitsAtOneTenth) {
  // Just inside qualifies, just outside does not. Both runs are identical
  // apart from the cache-write time being compared with the origin wait.
  auto qualifies = [](double stallS) {
    test::TempDir d;
    const std::string w = ",\"buffer_stall_us\":" + std::to_string((uint64_t)(stallS * 1e6)) +
                          originHist(1000.0);
    writeRun(d.path(), "h", 1, 1000, 1200, fillCounters(kGiB) + w,
             {filledFile("root://o//a", kGiB, 1200, "aa11")});
    const auto runs = loadRuns(d.path());
    EXPECT_EQ(runs.size(), 1u);
    return !runs.empty() && runs[0].baselineQualified();
  };
  EXPECT_TRUE(qualifies(99.0)) << "9.9% of the origin wait is inside the bound";
  EXPECT_FALSE(qualifies(101.0)) << "10.1% is outside it";
}

TEST(Thresholds, TheOverheadBoundSitsInTheGapTheEvidenceLeftEmpty) {
  // overhead() is a COARSE estimate -- measured error 0.17x to 3.01x against
  // nine fills whose no-cache wall was known -- so the bound cannot be derived
  // from units. It is placed empirically, in the gap between the fills that
  // were truly cheap and those that were truly expensive:
  //
  //   truly <=10% cost : reported 0.011, 0.013, 0.015
  //   truly  >10% cost : reported 0.608, 1.055, 1.295, 1.459, 1.858, 2.227
  //
  // Nothing was measured between 0.015 and 0.608. This test fails if the bound
  // is moved out of that empty band -- below it the cheap fills start being
  // refused, above it the expensive ones start being accepted, and in neither
  // case is there evidence to say what happens. Moving it is not forbidden; it
  // requires new calibration fills, and this is what says so.
  constexpr double kWorstTrulyCheap = 0.015;
  constexpr double kBestTrulyExpensive = 0.608;
  EXPECT_GT(Run::kMaxOverhead, kWorstTrulyCheap)
      << "below the worst fill that was genuinely fine: usable references would be refused";
  EXPECT_LT(Run::kMaxOverhead, kBestTrulyExpensive)
      << "above the best fill that genuinely was not: a contaminated reference would be accepted";
}

TEST(Thresholds, OriginShareBoundSitsAtNineteenTwentieths) {
  // A run is a reference only when the origin did nearly all of the work.
  // Fractions of a file, not whole files: the split is by bytes -- so the file
  // record carries the cached part in the byte-tier counter and the rest in the
  // wire counter, and the two add up to the file. They are disjoint counters;
  // putting the whole file in BOTH describes a file read twice, not a file
  // split between two tiers.
  // Asserted through QUALIFICATION, not through the share itself. Checking
  // the computed fraction against 0.95 pins the arithmetic and leaves the
  // constant free: dropping the bound to 0.80 passed such a test untouched.
  auto qualifies = [](uint64_t cachedBytes) {
    test::TempDir d;
    writeRun(d.path(), "h", 1, 1000, 1200,
             fillCounters(kGiB) + ",\"buffer_stall_us\":1000" + originHist(1000.0),
             {{"root://o//a", cachedBytes, 0, kGiB - cachedBytes, 1200, 0, "fill", kGiB,
               "aa11"}});
    const auto runs = loadRuns(d.path());
    EXPECT_EQ(runs.size(), 1u);
    return !runs.empty() && runs[0].baselineQualified();
  };
  EXPECT_TRUE(qualifies(kGiB / 25)) << "4% from cache still qualifies";
  EXPECT_FALSE(qualifies(kGiB / 15)) << "6.7% from cache does not";
}

TEST(Thresholds, WidthIsCollectedButNeverGatesAMatch) {
  // A standing ruling: thread counts and read concurrency are recorded and
  // displayed, and they must not decide whether two runs are comparable. The
  // two runs here differ by 20x on every width we record.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1200,
           std::string("\"disabled\":1,\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824")
               + ",\"peak_cores\":2,\"threads_high_water\":4"
               + ",\"origin_reads_in_flight_high_water\":2",
           {{"root://o//a", 0, 0, kGiB, 1200, 0, "relay", kGiB, "aa11"}});
  writeRun(d.path(), "h", 2, 2000, 2100,
           std::string("\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":1073741824")
               + ",\"peak_cores\":40,\"threads_high_water\":584"
               + ",\"origin_reads_in_flight_high_water\":40",
           {{"root://o//a", kGiB, 0, 0, 2100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  const ucache::Run* warm = nullptr;
  for (const auto& r : runs)
    if (!r.disabled)
      warm = &r;
  ASSERT_TRUE(warm);
  const auto g = estimateGain(*warm, runs);
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_NEAR(g.gain, 2.0, 0.05) << "200 s of origin against a 100 s warm pass";
}

// --- the refusal reason itself ------------------------------------------
//
// WHY a run has no gain is a decision, not a label: the summary counts each
// class separately and each one sends the reader somewhere different -- record
// a baseline, run for longer, read more data, or nothing at all because this
// run IS the baseline. The reasons were derived twice before, in two places,
// and three classes came out wrong; deciding it once is the fix, and these pin
// that the decision is actually made. Every value was reachable with nothing
// asserting any of it, so deleting an assignment left the suite green.

TEST(RefusalReason, ABaselineSaysSoRatherThanReportingNoGain) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"disabled\":1,\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":1073741824",
           {{"root://o//a", 0, 0, kGiB, 1200, 0, "relay", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kIsBaseline);
}

TEST(RefusalReason, AFillSaysItFilledRatherThanBlamingTheBaseline) {
  // The distinction that was got wrong: a fill and a run under the size floor
  // both used to report "no comparable baseline", which sends the reader off
  // to record a baseline that would not have helped either case.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1200,
           fillCounters(kGiB) + ",\"buffer_stall_us\":600000000" + originHist(1000.0),
           {filledFile("root://o//a", kGiB, 1200, "aa11")});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kFilled);
}

TEST(RefusalReason, TooLittleDataIsItsOwnAnswer) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, warmCounters(1 * kMiB),
           {{"root://o//a", kMiB, 0, 0, 1100, 0, "cached", kMiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kTooSmall);
}

TEST(RefusalReason, TooShortToTimeIsItsOwnAnswer) {
  // Above the size floor so the two cannot be confused: this run moved plenty
  // of data, it just did not run long enough to divide two whole seconds by.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1005, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 1005, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kTooShort);
}

TEST(RefusalReason, NoPerFileRecordsIsIncompleteNotMissingBaseline) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, warmCounters(kGiB), {});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kIncomplete);
}

TEST(RefusalReason, NothingToCompareAgainstSaysExactlyThat) {
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 1100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kNoBaseline);
  EXPECT_NE(g.reason.find("UCACHE_DISABLE=1"), std::string::npos)
      << "the one refusal with an action attached must name it: " << g.reason;
}

TEST(RefusalReason, DifferentWorkIsNotReportedAsDifferentFiles) {
  // Same files, different analysis. Reporting this as "covered different
  // files" would send the reader to fix a file list that is already correct.
  test::TempDir d;
  writeAgreementPair(d.path(), 100, 100);
  const auto g = gainOfWarm(d.path());
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kReadDifferent);
}

TEST(RefusalReason, AMeasuredRunSaysMeasured) {
  // The control: without it every assignment above could be replaced by the
  // one refusal that happens to be checked, and the suite would still pass.
  test::TempDir d;
  writeAgreementPair(d.path(), 100, 0);
  const auto g = gainOfWarm(d.path());
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kMeasured);
}

TEST(Gain, AReplicaServedRunReportsItsOwnWork) {
  // The purpose of counting all three tiers in the run-side totals, which had
  // no test of its own: a run served ENTIRELY from recompressed replicas
  // touches the byte-tier counter in crumbs, so totalling only that reported a
  // thousandth of the run's work -- and the run was then refused for having
  // done none. Every other estimateGain fixture here is byte-tier, so nothing
  // exercised it.
  test::TempDir d;
  std::vector<FileLine> base, warm;
  for (size_t i = 0; i < 20; ++i) {
    const std::string key = "root://o//f" + std::to_string(i);
    base.push_back(FileLine(key, 0, 0, kGiB, 1200, 0, "relay", kGiB, "aa11"));
    // served is a crumb; the replica tier did the work. This is the shape the
    // field produces: recorded warm replica runs put the byte tier between
    // 0.009% and 0.65% of the replica bytes.
    warm.push_back(FileLine(key, kGiB / 8192, kGiB, 0, 2100, 0, "cached", kGiB, "aa11"));
  }
  writeRun(d.path(), "h", 1, 1000, 1200,
           "\"opens\":1,\"origin_bytes\":0,\"relay_bytes\":21474836480,\"disabled\":1", base);
  writeRun(d.path(), "h", 2, 2000, 2100,
           "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":2621440,"
           "\"replica_bytes_served\":21474836480,\"served_bytes\":21477457920",
           warm);
  const auto g = gainOfWarm(d.path());
  EXPECT_TRUE(g.valid) << g.reason;
  EXPECT_NEAR(g.gain, 2.0, 0.05) << "200 s of origin against a 100 s replica pass";
}

TEST(RefusalReason, TheCacheServingNothingIsNamed) {
  // RunLog.cc's "the cache served nothing in this run" branch, which the suite
  // never executed. A run that opened files, went nowhere near the origin and
  // was served nothing has no gain to report and a specific reason why.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100,
           "\"opens\":1,\"origin_bytes\":0,\"hit_bytes\":0,\"replica_bytes_served\":0",
           {{"root://o//a", 0, 0, 0, 1100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kIncomplete);
  EXPECT_NE(g.reason.find("served nothing"), std::string::npos) << g.reason;
}

TEST(RefusalReason, RecordsThatAttributeNoBytesAreNamed) {
  // The other branch the suite never reached, and one this work changed: the
  // run's counters say the cache served a gigabyte, but no per-file record
  // accounts for any of it. Refusing is right -- there is nothing to match a
  // baseline against file by file -- and it must not be reported as "no
  // baseline", which would send the reader to record one that cannot help.
  test::TempDir d;
  writeRun(d.path(), "h", 1, 1000, 1100, warmCounters(kGiB),
           {{"root://o//a", 0, 0, 0, 1100, 0, "cached", kGiB, "aa11"}});
  const auto runs = loadRuns(d.path());
  ASSERT_EQ(runs.size(), 1u);
  const auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_EQ(g.why, ucache::GainEstimate::Why::kIncomplete);
  EXPECT_NE(g.reason.find("no bytes attributed"), std::string::npos) << g.reason;
}

TEST(Gain, TheEstimateSaysWhichKindOfReferenceProducedIt) {
  // The two kinds carry different confidence -- a disabled run is a reference
  // by an unambiguous flag, a fill by inference -- and the output used to say
  // "with the cache disabled" and emit "baseline" unconditionally, both wrong
  // whenever the reference was a fill. The fill is the ordinary case: most
  // people never think to record a disabled run.
  {
    test::TempDir d;
    writeAgreementPair(d.path(), 100, 0); // reference = a disabled run
    const auto g = gainOfWarm(d.path());
    ASSERT_TRUE(g.valid) << g.reason;
    EXPECT_TRUE(g.referenceDisabled);
  }
  {
    test::TempDir d; // reference = a qualifying fill, no disabled run anywhere
    writeRun(d.path(), "h", 1, 1000, 1200,
             fillCounters(kGiB) + ",\"buffer_stall_us\":50000000" + originHist(1000.0),
             {filledFile("root://o//a", kGiB, 1200, "aa11")});
    writeRun(d.path(), "h", 2, 2000, 2100, warmCounters(kGiB),
             {{"root://o//a", kGiB, 0, 0, 2100, 0, "cached", kGiB, "aa11"}});
    const auto runs = loadRuns(d.path());
    const ucache::Run* warm = nullptr;
    for (const auto& r : runs)
      if (!r.disabled && r.warm())
        warm = &r;
    ASSERT_TRUE(warm);
    const auto g = estimateGain(*warm, runs);
    ASSERT_TRUE(g.valid) << g.reason;
    EXPECT_FALSE(g.referenceDisabled) << "the reference was a fill and must say so";
  }
}
