// Concurrency: many threads hammering one FileEntry and one CacheStore.
// Primarily meaningful under TSan (the TSan CI job); asserts
// correctness invariants either way.
#include "CacheStore.h"

#include "TestUtil.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace ucache;
using test::TempDir;

TEST(Concurrency, SharedEntryReadersAndWriters) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.pageSize = 4096;
  cfg.metaFlushSeconds = 0; // aggressive sidecar traffic
  Stats stats;
  auto key = *UrlKey::parse("root://h//f");
  auto src = test::randomBytes(256 * 4096, 11);
  auto e = FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  ASSERT_TRUE(e);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> mismatches{0};
  auto writer = [&](uint64_t seed) {
    std::mt19937_64 rng(seed);
    while (!stop) {
      uint64_t len = 4096 * (1 + rng() % 8);
      uint64_t off = (rng() % (src.size() - len + 1)) / 4096 * 4096;
      e->writePages(off, len, src.data() + off);
    }
  };
  auto reader = [&](uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint8_t> buf(64 * 1024);
    while (!stop) {
      uint64_t len = 1 + rng() % buf.size();
      len = std::min<uint64_t>(len, src.size());
      uint64_t off = rng() % (src.size() - len + 1);
      if (e->hasRange(off, len) && e->readCached(off, len, buf.data())) {
        if (std::memcmp(buf.data(), src.data() + off, len) != 0)
          ++mismatches;
      }
    }
  };
  auto flusher = [&] {
    while (!stop)
      e->flushMeta(true);
  };

  std::vector<std::thread> ts;
  for (int i = 0; i < 3; ++i)
    ts.emplace_back(writer, 100 + i);
  for (int i = 0; i < 4; ++i)
    ts.emplace_back(reader, 200 + i);
  ts.emplace_back(flusher);
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  stop = true;
  for (auto& t : ts)
    t.join();
  EXPECT_EQ(mismatches.load(), 0u);
  auto r = e->verifyAll();
  EXPECT_EQ(r.bad, 0u);
}

// Coalesced reads plan a whole request under one lock take and then pread with
// the lock released, so the presence snapshot they act on can go stale — a
// concurrent punch (reclaim) can remove pages mid-run. The invariant: a read
// either returns the right bytes or returns false, never wrong bytes. Storming
// releaseRanges against readers is the case the plan-then-read window creates.
TEST(Concurrency, PunchStormAgainstCoalescedReaders) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  cfg.pageSize = 4096;
  Stats stats;
  auto key = *UrlKey::parse("root://h//punch");
  auto src = test::randomBytes(512 * 4096, 77);
  auto e = FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  ASSERT_TRUE(e);
  e->writePages(0, src.size(), src.data());
  e->flushAll();

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> wrong{0}, served{0}, missed{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&, t] {
      std::mt19937_64 rng(1000 + t);
      std::vector<uint8_t> buf(65 * 4096); // >= the widest span below
      while (!stop) {
        // Spans wide enough to coalesce, unaligned at both ends.
        uint64_t len = 4096 * (1 + rng() % 64) + rng() % 4096;
        ASSERT_LE(len, buf.size());
        uint64_t off = rng() % (src.size() - len);
        if (e->readCached(off, len, buf.data())) {
          if (memcmp(buf.data(), src.data() + off, len) != 0)
            ++wrong;
          else
            ++served;
        } else {
          ++missed;
        }
      }
    });
  ts.emplace_back([&] { // the puncher: removes pages under the readers
    std::mt19937_64 rng(99);
    while (!stop) {
      uint64_t page = rng() % 500;
      e->releaseRanges({{page * 4096, 8 * 4096}});
      e->writePages(page * 4096, 8 * 4096, src.data() + page * 4096); // refill
      e->flushBuffer(true);
    }
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  stop = true;
  for (auto& th : ts)
    th.join();
  EXPECT_EQ(wrong.load(), 0u) << "a coalesced read returned wrong bytes";
  EXPECT_GT(served.load(), 0u); // the test must actually have served reads
}

TEST(Concurrency, StoreConcurrentOpensShareEntries) {
  TempDir td;
  RealIO io;
  Config cfg;
  cfg.cacheDir = td.path();
  CacheStore store(io, cfg);
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> failures{0};
  auto opener = [&](int tid) {
    std::mt19937_64 rng(tid);
    while (!stop) {
      int n = rng() % 4;
      auto k = *UrlKey::parse("root://h//f" + std::to_string(n));
      auto e = store.open(k, 100000);
      if (!e || e->fileSize() != 100000)
        ++failures;
    }
  };
  std::vector<std::thread> ts;
  for (int i = 0; i < 8; ++i)
    ts.emplace_back(opener, i);
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  stop = true;
  for (auto& t : ts)
    t.join();
  EXPECT_EQ(failures.load(), 0u);
}
