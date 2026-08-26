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

TEST(Gain, ReducesToFillOverWarmWhenFileSetsMatch) {
  test::TempDir td;
  twoFileFill(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_NEAR(g.gain, 2.0, 0.01); // 100 s from the origin against 50 s from the cache
  EXPECT_NEAR(g.savedS, 50.0, 0.01);
  EXPECT_EQ(g.matchedFiles, 2u);
  EXPECT_EQ(g.originEquivBytes, kGiB);
}

// NEGATIVE CONTROL for the correction the campaign data forced. The replica
// tier serves DECOMPRESSED bytes, so a rate taken over bytes-served flatters
// it; here it serves twice the bytes in the same time, and the answer must not
// move. Using served bytes would report 4.0x.
TEST(Gain, UsesOriginEquivalentBytesNotBytesServed) {
  test::TempDir td;
  twoFileFill(td.path(), 100);
  writeRun(td.path(), "host", 300, 2000, 2050, warmCounters(0, 2 * kGiB),
           {{"root://o//a", kGiB, kGiB, 0, 0}, {"root://o//b", kGiB, kGiB, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_NEAR(g.gain, 2.0, 0.01);
  EXPECT_LT(g.gain, 3.0); // the naive form would say 4.0
  EXPECT_EQ(g.originEquivBytes, kGiB); // what the ORIGIN would have shipped
}

// A cache slower than the origin is a real and important answer. It must be
// reported below 1, not suppressed and not rounded up.
TEST(Gain, ReportsBelowOneWhenTheCacheIsSlower) {
  test::TempDir td;
  twoFileFill(td.path(), 50);
  writeRun(td.path(), "host", 400, 2000, 2200, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  ASSERT_TRUE(g.valid) << g.reason;
  EXPECT_LT(g.gain, 1.0);
  EXPECT_NEAR(g.gain, 0.25, 0.01);
}

TEST(Gain, SuppressedWithoutAReference) {
  test::TempDir td;
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("no earlier run"), std::string::npos);
}

TEST(Gain, SuppressedOnDisjointFileSets) {
  test::TempDir td;
  twoFileFill(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//x", kGiB / 2, 0, 0, 0}, {"root://o//y", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
}

TEST(Gain, SuppressedOnPartialOverlap) {
  test::TempDir td;
  // The fill covered four files; this run touched one of them. 25% is not a
  // comparison, and partial coverage is exactly what has misled us before.
  writeRun(td.path(), "host", 100, 1000, 1100, fillCounters(4 * kGiB),
           {{"root://o//a", kGiB, 0, kGiB, 0},
            {"root://o//b", kGiB, 0, kGiB, 0},
            {"root://o//c", kGiB, 0, kGiB, 0},
            {"root://o//d", kGiB, 0, kGiB, 0}});
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("different files"), std::string::npos);
}

TEST(Gain, SuppressedWhenTheFillHitFaults) {
  test::TempDir td;
  writeRun(td.path(), "host", 100, 1000, 1100, fillCounters(kGiB, /*faults=*/3),
           {{"root://o//a", kGiB / 2, 0, kGiB / 2, 0},
            {"root://o//b", kGiB / 2, 0, kGiB / 2, 0}});
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid); // an unhealthy fill is not a baseline
}

TEST(Gain, SuppressedBelowTheSizeFloor) {
  test::TempDir td;
  twoFileFill(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(8 * kMiB),
           {{"root://o//a", 4 * kMiB, 0, 0, 0}, {"root://o//b", 4 * kMiB, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("too little data"), std::string::npos);
}

TEST(Gain, SuppressedWhenTheReferenceRateIsAnOutlier) {
  test::TempDir td;
  // Three ordinary fills of the same two files, and one that claims to have
  // fetched twice as much in a fifth of the time.
  for (uint64_t i = 0; i < 3; ++i)
    writeRun(td.path(), "host", 10 + i, 100 + i * 10, 200 + i * 10, fillCounters(kGiB / 2),
             {{"root://o//a", kGiB / 4, 0, kGiB / 4, 0},
              {"root://o//b", kGiB / 4, 0, kGiB / 4, 0}});
  // Same span class as the others (the clock-resolution floor applies to
  // references too), but four times the bytes in less time -- an outlier rate.
  writeRun(td.path(), "host", 90, 900, 940, fillCounters(4 * kGiB),
           {{"root://o//a", 2 * kGiB, 0, 2 * kGiB, 0},
            {"root://o//b", 2 * kGiB, 0, 2 * kGiB, 0}});
  writeRun(td.path(), "host", 200, 2000, 2050, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("outlier"), std::string::npos);
}

// Whole-second records cannot time a short run well enough to divide by. The
// smoke-scale campaigns are exactly where the arithmetic drifted, so this is a
// refusal, not a rounding.
TEST(Gain, SuppressedWhenEitherRunIsTooShortToTime) {
  test::TempDir td;
  twoFileFill(td.path(), 100);
  writeRun(td.path(), "host", 200, 2000, 2020, warmCounters(kGiB), // 20 s
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs = loadRuns(td.path());
  auto g = estimateGain(runs[0], runs);
  EXPECT_FALSE(g.valid);
  EXPECT_NE(g.reason.find("too short"), std::string::npos);

  // ... and a short FILL is not a usable reference either.
  test::TempDir td2;
  twoFileFill(td2.path(), 10);
  writeRun(td2.path(), "host", 200, 2000, 2100, warmCounters(kGiB),
           {{"root://o//a", kGiB / 2, 0, 0, 0}, {"root://o//b", kGiB / 2, 0, 0, 0}});
  auto runs2 = loadRuns(td2.path());
  EXPECT_FALSE(estimateGain(runs2[0], runs2).valid);
}
