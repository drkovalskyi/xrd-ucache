// Crash suite: fork a populator child that caches pages from a
// source buffer as fast as it can (with periodic sidecar flushes), SIGKILL
// it at a random point, then reopen the entry in the parent and prove ZERO
// servable-but-wrong pages: every page the cache claims to have must be
// (a) CRC-clean and (b) byte-identical to the source. Repeats
// UCACHE_CRASH_ITERS times (default 200; set lower for quick runs).
//
// Exit code 0 = all iterations clean; 1 = violation (seed and iter printed).
#include "CacheStore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ftw.h>
#include <random>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace ucache;

namespace {

int rmCb(const char* p, const struct stat*, int, struct FTW*) { return ::remove(p); }
void rmTree(const std::string& p) { ::nftw(p.c_str(), rmCb, 16, FTW_DEPTH | FTW_PHYS); }

std::vector<uint8_t> randomBytes(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<uint8_t> out(n);
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<uint8_t>(rng());
  return out;
}

uint64_t envU64(const char* name, uint64_t dflt) {
  const char* v = ::getenv(name);
  return v ? ::strtoull(v, nullptr, 10) : dflt;
}

// The child populates forever until killed.
[[noreturn]] void childLoop(const std::string& dir, const std::vector<uint8_t>& src,
                            uint64_t seed) {
  Config cfg;
  cfg.cacheDir = dir;
  cfg.pageSize = 4096;
  cfg.metaFlushSeconds = 0; // flush aggressively: maximize meta/data races
  RealIO io;
  Stats stats;
  UrlKey key = *UrlKey::parse("root://crash//f");
  auto e = FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  if (!e)
    ::_exit(2);
  std::mt19937_64 rng(seed);
  for (;;) {
    uint64_t len = 1 + rng() % (128 * 1024);
    len = std::min<uint64_t>(len, src.size());
    uint64_t off = rng() % (src.size() - len + 1);
    e->writePages(off, len, src.data() + off);
    if (rng() % 16 == 0)
      e->flushMeta(true);
  }
}

// Returns number of wrong pages (0 = clean).
uint64_t verifyAfterCrash(const std::string& dir, const std::vector<uint8_t>& src) {
  Config cfg;
  cfg.cacheDir = dir;
  cfg.pageSize = 4096;
  RealIO io;
  Stats stats;
  UrlKey key = *UrlKey::parse("root://crash//f");
  auto e = FileEntry::open(io, cfg, stats, key, src.size(), 0, MetaData::kCksumNone, 0);
  if (!e) {
    std::fprintf(stderr, "verify: reopen failed\n");
    return 1;
  }
  // (a) CRC scrub.
  auto scrub = e->verifyAll();
  // (b) byte-truth: every page still claimed present must equal the source.
  uint64_t wrong = 0;
  const uint32_t P = e->pageSize();
  std::vector<uint8_t> buf(P);
  uint64_t npages = (src.size() + P - 1) / P;
  uint64_t present = 0;
  for (uint64_t i = 0; i < npages; ++i) {
    uint64_t off = i * P;
    uint64_t len = std::min<uint64_t>(P, src.size() - off);
    if (!e->hasRange(off, len))
      continue;
    ++present;
    if (!e->readCached(off, len, buf.data())) {
      ++wrong; // claimed present but unreadable-clean => counts as wrong
      continue;
    }
    if (std::memcmp(buf.data(), src.data() + off, len) != 0)
      ++wrong;
  }
  std::printf("  scrubbed=%llu bad_crc=%llu present=%llu wrong=%llu\n",
              static_cast<unsigned long long>(scrub.checked),
              static_cast<unsigned long long>(scrub.bad),
              static_cast<unsigned long long>(present),
              static_cast<unsigned long long>(wrong));
  // scrub.bad pages were quarantined (lost cache, acceptable); `wrong` pages
  // would have been SERVED wrong — the invariant that must never break.
  return wrong;
}

} // namespace

int main() {
  const uint64_t iters = envU64("UCACHE_CRASH_ITERS", 200);
  const uint64_t seed0 = envU64("UCACHE_CRASH_SEED", std::random_device{}());
  std::printf("crash suite: %llu iterations, seed=%llu\n",
              static_cast<unsigned long long>(iters),
              static_cast<unsigned long long>(seed0));
  std::mt19937_64 rng(seed0);

  const char* tbase = ::getenv("TMPDIR");
  std::string base = std::string(tbase ? tbase : "/tmp");

  for (uint64_t it = 0; it < iters; ++it) {
    uint64_t seed = rng();
    std::string tpl = base + "/ucache-crash-XXXXXX";
    char* dir = ::mkdtemp(tpl.data());
    if (!dir) {
      std::perror("mkdtemp");
      return 1;
    }
    size_t fileSize = 512 * 1024 + seed % (3 * 1024 * 1024);
    auto src = randomBytes(fileSize, seed);

    pid_t pid = ::fork();
    if (pid == 0)
      childLoop(dir, src, seed); // never returns
    if (pid < 0) {
      std::perror("fork");
      return 1;
    }
    // Kill at a random point: 200us .. 60ms of populating.
    ::usleep(200 + seed % 60000);
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);

    std::printf("iter %llu seed=%llu size=%zu\n", static_cast<unsigned long long>(it),
                static_cast<unsigned long long>(seed), fileSize);
    uint64_t wrong = verifyAfterCrash(dir, src);
    rmTree(dir);
    if (wrong != 0) {
      std::fprintf(stderr, "FAIL: iter=%llu seed=%llu wrong_pages=%llu\n",
                   static_cast<unsigned long long>(it),
                   static_cast<unsigned long long>(seed),
                   static_cast<unsigned long long>(wrong));
      return 1;
    }
  }
  std::printf("crash suite: all %llu iterations clean\n",
              static_cast<unsigned long long>(iters));
  return 0;
}
