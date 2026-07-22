#include "Stats.h"

#include "TestUtil.h"
#include <fstream>
#include <gtest/gtest.h>
#include <sys/stat.h>

using namespace ucache;

TEST(Histogram, Log2Bucketing) {
  Histogram h;
  h.add(0);
  h.add(1); // both -> bucket 0
  h.add(2);
  h.add(3); // -> bucket 1
  h.add(4); // -> bucket 2
  h.add(1024); // -> bucket 10
  h.add(~0ull); // clamps to last bucket
  EXPECT_EQ(h.b[0].load(), 2u);
  EXPECT_EQ(h.b[1].load(), 2u);
  EXPECT_EQ(h.b[2].load(), 1u);
  EXPECT_EQ(h.b[10].load(), 1u);
  EXPECT_EQ(h.b[Histogram::kBuckets - 1].load(), 1u);
}

TEST(Histogram, JsonTrimsTrailingZeros) {
  Histogram h;
  EXPECT_EQ(h.toJson(), "[]");
  h.add(1);
  h.add(4);
  EXPECT_EQ(h.toJson(), "[1,0,1]");
}

TEST(Stats, JsonBodyHasAllCounters) {
  Stats s;
  s.opens = 3;
  s.hitBytes = 12345;
  s.crcFailures = 1;
  s.hitReadUs.add(7);
  std::string j = "{" + s.toJsonBody() + "}";
  // Spot-check names and values (schema is docs/STATS.md and load-bearing).
  EXPECT_NE(j.find("\"opens\":3"), std::string::npos);
  EXPECT_NE(j.find("\"hit_bytes\":12345"), std::string::npos);
  EXPECT_NE(j.find("\"crc_failures\":1"), std::string::npos);
  EXPECT_NE(j.find("\"origin_bytes\":0"), std::string::npos);
  EXPECT_NE(j.find("\"failopen_events\":0"), std::string::npos);
  EXPECT_NE(j.find("\"disabled_handles\":0"), std::string::npos);
  EXPECT_NE(j.find("\"hist_hit_read_us\":[0,0,1]"), std::string::npos);
  // Braces balanced, no trailing comma before '}'.
  EXPECT_EQ(j.find(",}"), std::string::npos);
}

TEST(StatsAggregate, LastLinePerFileSummedAcrossProcesses) {
  test::TempDir td;
  std::string dir = td.path() + "/stats";
  ::mkdir(dir.c_str(), 0700);
  // Process A: two cumulative snapshots — the LAST supersedes.
  {
    std::ofstream f(dir + "/hostA-100-1.jsonl");
    f << "{\"ts\":1,\"pid\":100,\"opens\":5,\"hit_bytes\":100,\"origin_bytes\":40,"
         "\"origin_reads\":2,\"hist_hit_read_us\":[1,2,3]}\n";
    f << "{\"ts\":2,\"pid\":100,\"opens\":10,\"hit_bytes\":200,\"origin_bytes\":50,"
         "\"origin_reads\":4,\"hist_hit_read_us\":[1,2,3,4]}\n"; // final for A
  }
  // Process B: single line.
  {
    std::ofstream f(dir + "/hostB-200-1.jsonl");
    f << "{\"ts\":9,\"pid\":200,\"opens\":3,\"hit_bytes\":30,\"served_bytes\":7,"
         "\"failopen_events\":1}\n";
  }
  // Noise that must be ignored: wrong suffix, and an empty file.
  { std::ofstream(dir + "/notes.txt") << "ignore me\n"; }
  { std::ofstream(dir + "/hostC-300-1.jsonl"); }

  StatsTotals t = aggregateStats(dir);
  EXPECT_EQ(t.files, 2);          // empty + .txt skipped
  EXPECT_EQ(t.opens, 13u);        // A's last (10) + B (3)
  EXPECT_EQ(t.hitBytes, 230u);    // 200 + 30
  EXPECT_EQ(t.originBytes, 50u);  // A's last only (not 40+50)
  EXPECT_EQ(t.originReads, 4u);   // A's last
  EXPECT_EQ(t.servedBytes, 7u);   // B
  EXPECT_EQ(t.failopenEvents, 1u);
}

TEST(StatsAggregate, MissingDirIsZero) {
  StatsTotals t = aggregateStats("/nonexistent/ucache/stats");
  EXPECT_EQ(t.files, 0);
  EXPECT_EQ(t.opens, 0u);
}
