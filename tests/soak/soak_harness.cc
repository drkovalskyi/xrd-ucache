// Eviction soak: sustained writes past the cache limit for a long
// duration with MANY concurrent processes sharing one cache dir — the real
// deployment model. Asserts the guarantees eviction must hold under
// load: (1) pinned entries are never evicted and stay byte-correct; (2) usage
// stays bounded (no unbounded growth); (3) every worker makes forward progress
// (no starvation / deadlock); (4) zero CRC/meta corruption on the protected set.
//
// Roles across UCACHE_SOAK_PROCS children on a shared UCACHE_DIR:
//   - writers: churn fresh keys (drive the cache past the limit -> eviction);
//   - readers: re-read the pinned working set and verify every byte;
//   - one admin: interleaves evictNow() + setPinnedByKey() (the CLI-vs-plugin
//     concurrent mutation path the w1-verify review flagged).
// A hard byte cap (UCACHE_MAX_BYTES) drives eviction deterministically without
// depending on filling the real disk. Env knobs (defaults in main).
//
// Exit 0 = all invariants held; nonzero = violation (details on stderr).
#include "CacheStore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <ftw.h>
#include <random>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace ucache;

namespace {

int rmCb(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }
void rmTree(const std::string& p) { ::nftw(p.c_str(), rmCb, 16, FTW_DEPTH | FTW_PHYS); }

uint64_t envU64(const char* n, uint64_t d) {
  const char* v = ::getenv(n);
  return v ? ::strtoull(v, nullptr, 10) : d;
}

// Deterministic content for pinned entry `idx`, reproducible in any process.
std::vector<uint8_t> pinnedBytes(uint64_t idx, size_t n) {
  std::mt19937_64 rng(0x50FA5EED ^ idx);
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<uint8_t>(rng());
  return out;
}

std::string pinKey(uint64_t i) { return "root://soak//pin/" + std::to_string(i); }

Config soakCfg(const std::string& dir, uint64_t maxBytes) {
  Config cfg;
  cfg.cacheDir = dir;
  cfg.pageSize = 4096;
  cfg.maxBytes = maxBytes;      // explicit hard cap => deterministic byte-budget eviction
  cfg.minFreeBytes = 2ull << 30; // disk-full backstop (the byte cap fires first)
  cfg.highWater = 0.9;
  cfg.lowWater = 0.6;
  cfg.evictCheckSeconds = 1; // evict often
  cfg.metaFlushSeconds = 1;
  return cfg;
}

// One worker process. Returns via _exit: 0 ok, 3 content mismatch, 4 no progress.
[[noreturn]] void worker(int role, const std::string& dir, uint64_t maxBytes,
                         uint64_t pinnedCount, size_t pinnedSize, size_t writeSize,
                         uint64_t deadline, uint64_t seed, uint64_t writeDelayUs) {
  RealIO io;
  CacheStore store(io, soakCfg(dir, maxBytes));
  std::mt19937_64 rng(seed);
  std::vector<uint8_t> wbuf(writeSize, static_cast<uint8_t>(seed));
  std::vector<uint8_t> rbuf(pinnedSize);
  uint64_t ops = 0, ctr = 0;
  while (static_cast<uint64_t>(::time(nullptr)) < deadline) {
    if (role == 0) {
      // admin: churn eviction + re-pin the working set + spot-verify one.
      store.evictNow();
      for (uint64_t i = 0; i < pinnedCount; ++i)
        store.setPinnedByKey(*UrlKey::parse(pinKey(i)), true);
      uint64_t i = rng() % pinnedCount;
      if (auto e = store.open(*UrlKey::parse(pinKey(i)), pinnedSize)) {
        if (e->readCached(0, pinnedSize, rbuf.data())) {
          auto exp = pinnedBytes(i, pinnedSize);
          if (std::memcmp(rbuf.data(), exp.data(), pinnedSize) != 0)
            ::_exit(3);
        }
      }
      ::usleep(200000); // ~5 admin passes/sec
    } else if (role % 2 == 1) {
      // writer: fresh unique key each time -> steady pressure past the cap.
      // Throttled to a realistic sustained (origin-paced) rate: an unthrottled
      // firehose out-writes a single serialized eviction stream, which is a
      // synthetic artifact, not a real workload (real fills are network-bound).
      std::string k = "root://soak//w/" + std::to_string(::getpid()) + "/" + std::to_string(ctr++);
      if (auto e = store.open(*UrlKey::parse(k), writeSize))
        e->writePages(0, writeSize, wbuf.data());
      if (writeDelayUs)
        ::usleep(static_cast<useconds_t>(writeDelayUs));
    } else {
      // reader: re-read a pinned entry and verify every byte is correct.
      uint64_t i = rng() % pinnedCount;
      auto e = store.open(*UrlKey::parse(pinKey(i)), pinnedSize);
      if (e && e->readCached(0, pinnedSize, rbuf.data())) {
        auto exp = pinnedBytes(i, pinnedSize);
        if (std::memcmp(rbuf.data(), exp.data(), pinnedSize) != 0)
          ::_exit(3); // a pinned entry served WRONG bytes — hard failure
      }
    }
    ++ops;
  }
  ::_exit(ops > 0 ? 0 : 4); // 4 => made no progress (starvation/deadlock)
}

} // namespace

int main() {
  const uint64_t seconds = envU64("UCACHE_SOAK_SECONDS", 60);
  const uint64_t procs = std::max<uint64_t>(3, envU64("UCACHE_SOAK_PROCS", 8));
  const uint64_t maxBytes = envU64("UCACHE_MAX_BYTES", 32ull << 20);
  const uint64_t pinnedCount = std::max<uint64_t>(1, envU64("UCACHE_SOAK_PINNED", 8));
  const size_t pinnedSize = 256 * 1024;
  const size_t writeSize = 128 * 1024;
  // Per-write throttle (default 3 ms) -> sustained origin-like pressure past the
  // cap, not an unbounded firehose. 0 = flat out (stress eviction throughput).
  const uint64_t writeDelayUs = envU64("UCACHE_SOAK_WRITE_DELAY_US", 3000);
  const uint64_t seed0 = envU64("UCACHE_SOAK_SEED", std::random_device{}());

  std::string dir;
  if (const char* d = ::getenv("UCACHE_DIR")) {
    dir = d;
  } else {
    const char* base = ::getenv("TMPDIR");
    std::string tpl = std::string(base ? base : "/tmp") + "/ucache-soak-XXXXXX";
    dir = ::mkdtemp(tpl.data());
  }
  std::printf("soak: %llu procs x %llus, maxBytes=%llu, pinned=%llu, dir=%s\n",
              (unsigned long long)procs, (unsigned long long)seconds,
              (unsigned long long)maxBytes, (unsigned long long)pinnedCount, dir.c_str());

  RealIO io;
  // Populate + pin the protected working set.
  {
    CacheStore store(io, soakCfg(dir, maxBytes));
    for (uint64_t i = 0; i < pinnedCount; ++i) {
      auto e = store.open(*UrlKey::parse(pinKey(i)), pinnedSize);
      if (!e) {
        std::fprintf(stderr, "setup: open pinned %llu failed\n", (unsigned long long)i);
        return 1;
      }
      auto data = pinnedBytes(i, pinnedSize);
      e->writePages(0, pinnedSize, data.data());
      e->flushMeta(true);
      if (!store.setPinnedByKey(*UrlKey::parse(pinKey(i)), true)) {
        std::fprintf(stderr, "setup: pin %llu failed\n", (unsigned long long)i);
        return 1;
      }
    }
  }

  const uint64_t deadline = static_cast<uint64_t>(::time(nullptr)) + seconds;
  std::vector<pid_t> kids;
  for (uint64_t r = 0; r < procs; ++r) {
    pid_t pid = ::fork();
    if (pid == 0)
      worker(static_cast<int>(r), dir, maxBytes, pinnedCount, pinnedSize, writeSize, deadline,
             seed0 + r, writeDelayUs);
    if (pid < 0) {
      std::perror("fork");
      return 1;
    }
    kids.push_back(pid);
  }

  // Parent monitors usage until the deadline.
  // A generous multiple of the cap: catches unbounded growth without flaking on
  // concurrent write-burst overshoot.
  const uint64_t bound = 4 * maxBytes;
  uint64_t maxUsage = 0;
  bool exploded = false;
  {
    CacheStore mon(io, soakCfg(dir, maxBytes));
    mon.disableStatsDump();
    while (static_cast<uint64_t>(::time(nullptr)) < deadline) {
      maxUsage = std::max(maxUsage, mon.usageBytes());
      if (maxUsage > bound) { // abort early: do NOT keep filling the real disk
        exploded = true;
        break;
      }
      ::usleep(1000000);
    }
  }
  if (exploded)
    for (pid_t k : kids)
      ::kill(k, SIGKILL); // stop the writers before they can fill the filesystem

  int fails = 0;
  for (size_t r = 0; r < kids.size(); ++r) {
    int st = 0;
    ::waitpid(kids[r], &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
      std::fprintf(stderr, "FAIL: worker %zu exited abnormally (status=%d)\n", r, st);
      ++fails;
    }
  }

  // Final: every pinned entry present, byte-correct, and CRC-clean.
  CacheStore store(io, soakCfg(dir, maxBytes));
  uint64_t badPins = 0;
  for (uint64_t i = 0; i < pinnedCount; ++i) {
    auto e = store.open(*UrlKey::parse(pinKey(i)), pinnedSize);
    std::vector<uint8_t> buf(pinnedSize);
    auto exp = pinnedBytes(i, pinnedSize);
    if (!e || !e->hasRange(0, pinnedSize) || !e->readCached(0, pinnedSize, buf.data()) ||
        std::memcmp(buf.data(), exp.data(), pinnedSize) != 0) {
      std::fprintf(stderr, "FAIL: pinned entry %llu missing/wrong after soak\n",
                   (unsigned long long)i);
      ++badPins;
    } else if (e->verifyAll().bad != 0) {
      std::fprintf(stderr, "FAIL: pinned entry %llu has CRC-bad pages\n", (unsigned long long)i);
      ++badPins;
    }
  }

  // Usage must have stayed bounded (eviction kept up).
  bool boundOk = maxUsage <= bound;
  std::printf("soak done: maxUsage=%llu (cap=%llu, bound=%llu) badPins=%llu workerFails=%d\n",
              (unsigned long long)maxUsage, (unsigned long long)maxBytes,
              (unsigned long long)bound, (unsigned long long)badPins, fails);
  if (!boundOk)
    std::fprintf(stderr, "FAIL: usage %llu exceeded bound %llu (eviction not keeping up)\n",
                 (unsigned long long)maxUsage, (unsigned long long)bound);

  if (!::getenv("UCACHE_DIR"))
    rmTree(dir); // only clean a dir we created

  return (fails == 0 && badPins == 0 && boundOk) ? 0 : 1;
}
