// Concurrency: many threads hammering one FileEntry and one CacheStore.
// Primarily meaningful under TSan (the TSan CI job); asserts
// correctness invariants either way.
#include "CacheStore.h"

#include "TestUtil.h"
#include <atomic>
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
