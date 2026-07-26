// Randomized differential test: a cache-backed reader that
// simulates the §5.2 plugin algorithm — atomic-chunk classification, HIT
// from cache, MISS fetched from the "origin" (a plain byte buffer) with
// outward page rounding and full-page persistence — compared byte-for-byte
// against the origin on EVERY operation. Random reopens exercise sidecar
// adoption; random on-disk corruption exercises the CRC fail-open path.
//
// Seed is logged; override with UCACHE_TEST_SEED. Op count via
// UCACHE_TEST_OPS (default 100000; CI runs >= 1e5).
#include "CacheStore.h"

#include "TestUtil.h"
#include <algorithm>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <random>

using namespace ucache;
using test::TempDir;

namespace {

class DifferentialHarness {
 public:
  DifferentialHarness(uint64_t seed, uint64_t fileSize, uint32_t pageSize)
      : rng_(seed), src_(test::randomBytes(fileSize, seed ^ 0x9E3779B97F4A7C15ull)) {
    cfg_.cacheDir = td_.path();
    cfg_.pageSize = pageSize;
    store_ = std::make_unique<CacheStore>(io_, cfg_);
    key_ = *UrlKey::parse("root://origin//data/f.root");
    entry_ = store_->open(key_, src_.size());
  }

  // One client read [off, len): classify per chunk; HIT via readCached,
  // MISS via origin + writePages of the page-rounded span. Returns bytes
  // exactly as the plugin would deliver them.
  void clientRead(uint64_t off, uint64_t len, uint8_t* out) {
    if (entry_ && entry_->hasRange(off, len) && entry_->readCached(off, len, out))
      return; // HIT
    // MISS (or CRC fail-open): fetch rounded span from origin, persist,
    // serve the client from the origin bytes (as the plugin does).
    const uint32_t P = entry_ ? entry_->pageSize() : 4096;
    uint64_t start = off / P * P;
    uint64_t end = std::min<uint64_t>((off + len + P - 1) / P * P, src_.size());
    if (entry_)
      entry_->writePages(start, end - start, src_.data() + start);
    std::memcpy(out, src_.data() + off, len);
  }

  void step() {
    std::uniform_int_distribution<int> what(0, 99);
    int w = what(rng_);
    if (w < 2) { // reopen: drop and re-adopt through the store
      entry_.reset();
      entry_ = store_->open(key_, src_.size());
      ASSERT_TRUE(entry_);
      return;
    }
    if (w < 4) { // corrupt one random cached byte on disk
      corruptRandomByte();
      return;
    }
    if (w < 10) { // vector-read: several chunks, each classified atomically
      int nchunks = 1 + static_cast<int>(rng_() % 8);
      for (int c = 0; c < nchunks; ++c)
        randomRead(1, 32 * 1024);
      return;
    }
    randomRead(1, 256 * 1024);
  }

  void randomRead(uint64_t minLen, uint64_t maxLen) {
    std::uniform_int_distribution<uint64_t> lenD(minLen, maxLen);
    uint64_t len = std::min<uint64_t>(lenD(rng_), src_.size());
    std::uniform_int_distribution<uint64_t> offD(0, src_.size() - len);
    uint64_t off = offD(rng_);
    // EXACTLY len bytes, freshly allocated: a reused vector keeps its capacity,
    // and a read that overruns into that capacity is invisible to a sanitizer.
    std::unique_ptr<uint8_t[]> buf(new uint8_t[len]);
    clientRead(off, len, buf.get());
    ASSERT_EQ(0, std::memcmp(buf.get(), src_.data() + off, len))
        << "MISMATCH off=" << off << " len=" << len;
  }

  void corruptRandomByte() {
    int fd = ::open(key_.dataPath(cfg_.cacheDir).c_str(), O_RDWR);
    if (fd < 0)
      return;
    uint64_t off = rng_() % src_.size();
    uint8_t b;
    if (::pread(fd, &b, 1, off) == 1) {
      b ^= 0x1 << (rng_() % 8);
      [[maybe_unused]] ssize_t r = ::pwrite(fd, &b, 1, off);
    }
    ::close(fd);
  }

  void finalScrub() {
    ASSERT_TRUE(entry_);
    // First scrub quarantines any not-yet-noticed corruption...
    entry_->verifyAll();
    // ...second scrub must then be perfectly clean: zero servable-but-wrong.
    auto r = entry_->verifyAll();
    EXPECT_EQ(r.bad, 0u);
    // And a full sequential read must equal the origin byte-for-byte.
    buf_.resize(src_.size());
    clientRead(0, src_.size(), buf_.data());
    EXPECT_EQ(0, std::memcmp(buf_.data(), src_.data(), src_.size()));
  }

  Stats& stats() { return store_->stats(); }

 private:
  TempDir td_;
  RealIO io_;
  Config cfg_;
  std::mt19937_64 rng_;
  std::vector<uint8_t> src_;
  std::unique_ptr<CacheStore> store_;
  UrlKey key_;
  std::shared_ptr<FileEntry> entry_;
  std::vector<uint8_t> buf_;
};

uint64_t envU64(const char* name, uint64_t dflt) {
  const char* v = ::getenv(name);
  return v ? ::strtoull(v, nullptr, 10) : dflt;
}

} // namespace

TEST(Differential, RandomOpsVsPlainFile) {
  uint64_t seed = envU64("UCACHE_TEST_SEED", std::random_device{}());
  uint64_t ops = envU64("UCACHE_TEST_OPS", 100000);
  // The full-scale campaign runs >= 1e6 ops across >= 100 randomized files; the
  // spec-scale campaign runs UCACHE_TEST_OPS=1000000 UCACHE_TEST_FILES=100
  // under ASan/UBSan. Default (1 file) keeps the historical per-CI behavior.
  uint64_t files = envU64("UCACHE_TEST_FILES", 1);
  if (files < 1)
    files = 1;
  printf("[ differential ] seed=%llu ops=%llu files=%llu "
         "(override: UCACHE_TEST_SEED/UCACHE_TEST_OPS/UCACHE_TEST_FILES)\n",
         static_cast<unsigned long long>(seed), static_cast<unsigned long long>(ops),
         static_cast<unsigned long long>(files));

  uint64_t crcs = 0, writes = 0;
  for (uint64_t f = 0; f < files && !::testing::Test::HasFatalFailure(); ++f) {
    uint64_t fseed = seed + f * 0x9E3779B97F4A7C15ull;
    // File 0 keeps the historical geometry; the rest randomize size and page.
    uint64_t size = 8 * 1024 * 1024 + 12345 /*odd tail*/;
    uint32_t page = 4096;
    if (f > 0) {
      std::mt19937_64 grng(fseed);
      static constexpr uint32_t kPages[] = {4096, 16384, 65536};
      size = 512 * 1024 + grng() % (12 * 1024 * 1024) + grng() % 8192 /*odd tail*/;
      page = kPages[grng() % 3];
    }
    DifferentialHarness h(fseed, size, page);
    uint64_t perFile = std::max<uint64_t>(ops / files, 1);
    for (uint64_t i = 0; i < perFile && !::testing::Test::HasFatalFailure(); ++i)
      h.step();
    h.finalScrub();
    crcs += h.stats().crcFailures.load();
    writes += h.stats().pageWrites.load();
  }
  printf("[ differential ] crc_failures=%llu (corruptions caught) page_writes=%llu\n",
         static_cast<unsigned long long>(crcs), static_cast<unsigned long long>(writes));
}

TEST(Differential, LargePagesAndTinyFile) {
  uint64_t seed = envU64("UCACHE_TEST_SEED", 424242);
  // 64 KiB pages over a file smaller than one page: tail-only entry.
  DifferentialHarness h(seed, 30000, 65536);
  for (int i = 0; i < 2000 && !::testing::Test::HasFatalFailure(); ++i)
    h.step();
  h.finalScrub();
}
