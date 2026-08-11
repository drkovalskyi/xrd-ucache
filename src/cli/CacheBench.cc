#include "CacheBench.h"

#include "CacheStore.h"
#include "Config.h"
#include "FileEntry.h"
#include "IOBackend.h"
#include "Stats.h"
#include "UrlKey.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ucache {
namespace {

double nowS() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Same four fields DiskBench samples, kept local so this file does not depend on
// it: completed writes/reads, sectors, and the weighted in-flight time whose
// delta over elapsed is the average queue depth.
struct Dev {
  bool valid = false;
  uint64_t rd = 0, rdSect = 0, wr = 0, wrSect = 0, weightedMs = 0;
};
Dev devRead(const std::string& disk) {
  Dev d;
  if (disk.empty())
    return d;
  std::ifstream f("/proc/diskstats");
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::vector<std::string> t;
    std::string w;
    while (is >> w)
      t.push_back(w);
    if (t.size() < 14 || t[2] != disk)
      continue;
    d.valid = true;
    d.rd = std::strtoull(t[3].c_str(), nullptr, 10);
    d.rdSect = std::strtoull(t[5].c_str(), nullptr, 10);
    d.wr = std::strtoull(t[7].c_str(), nullptr, 10);
    d.wrSect = std::strtoull(t[9].c_str(), nullptr, 10);
    d.weightedMs = std::strtoull(t[13].c_str(), nullptr, 10);
    break;
  }
  return d;
}

// Dirty pages right now, in GiB. Sampled through the fill so the record can say
// whether the kernel's writeback threshold was actually approached, rather than
// only whether the volume was large enough that it might have been.
double dirtyMib() {
  std::ifstream f("/proc/meminfo");
  std::string k;
  unsigned long long v = 0;
  while (f >> k) {
    if (k == "Dirty:") {
      f >> v;
      return static_cast<double>(v) / 1024.0;
    }
    std::getline(f, k);
  }
  return 0;
}

void pct(std::vector<uint32_t>& us, CachePhase& p) {
  if (us.empty())
    return;
  std::sort(us.begin(), us.end());
  auto at = [&us](double q) { return us[std::min(us.size() - 1, size_t(us.size() * q))]; };
  p.p50Us = at(0.50);
  p.p95Us = at(0.95);
  p.p99Us = at(0.99);
}

// A deterministic scattered order: every block of the entry exactly once, in a
// shuffled sequence. That makes each fill write a first touch into a hole and no
// two consecutive writes adjacent, and it makes the volume exact. It is the one
// thing here that is a MODEL — a real analysis reads in basket layout order, not
// uniformly at random — and it is why the plan wants trace replay eventually.
std::vector<uint64_t> scatterOrder(uint64_t blocks, uint64_t seed) {
  std::vector<uint64_t> o(static_cast<size_t>(blocks));
  for (uint64_t i = 0; i < blocks; ++i)
    o[static_cast<size_t>(i)] = i;
  std::mt19937_64 rng(seed);
  for (uint64_t i = blocks; i > 1; --i)
    std::swap(o[static_cast<size_t>(i - 1)], o[static_cast<size_t>(rng() % i)]);
  return o;
}

// Drop the page cache over the entry data files. POSIX_FADV_DONTNEED only
// discards CLEAN pages, so the caller must have synced first — otherwise the
// read phase quietly measures RAM.
void dropCache(const std::vector<std::string>& paths) {
  for (const auto& p : paths) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0)
      continue;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    ::close(fd);
  }
}

// Remove the cache tree this stage built. DiskBench's own cleanup uses rmdir(),
// which fails silently on a non-empty directory — so without this the objects,
// sidecars and stats files stay on the target disk forever. Guarded on the
// benchmark's own temp-directory marker so a recursive delete can only ever run
// inside a path this process created.
bool removeTree(const std::string& path) {
  if (path.find("/.ucache-bench.") == std::string::npos)
    return false; // refuse to recurse anywhere we did not make
  DIR* d = ::opendir(path.c_str());
  if (!d)
    return false;
  while (struct dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (n == "." || n == "..")
      continue;
    const std::string child = path + "/" + n;
    struct ::stat st;
    if (::lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      removeTree(child);
    else
      ::unlink(child.c_str());
  }
  ::closedir(d);
  return ::rmdir(path.c_str()) == 0;
}

void syncPaths(const std::vector<std::string>& paths) {
  // One thread per file: a serial loop runs the tail at a fraction of the
  // concurrency the writes used, which is enough on its own to distort a
  // comparison (measured: 34-80% of a fill's wall clock).
  std::vector<std::thread> t;
  t.reserve(paths.size());
  for (const auto& p : paths)
    t.emplace_back([p] {
      int fd = ::open(p.c_str(), O_WRONLY);
      if (fd < 0)
        return;
      ::fdatasync(fd);
      ::close(fd);
    });
  for (auto& x : t)
    x.join();
}

// Buckets a phase's rate by DEVICE bytes into eighths of the volume, in order.
// Both phases get one: an aggregate cannot show a rate that drifts through the
// pass, and the fill had this while the reads did not.
struct Curve {
  CachePhase& p;
  const std::string& disk;
  uint64_t target;
  bool write;
  Dev d0, bucket0;
  double bucketT0;
  int filled = 0;
  std::atomic<bool> running{true};
  std::thread th;
  Curve(CachePhase& ph, const std::string& dk, uint64_t tgt, bool wr, const Dev& start, double t0)
      : p(ph), disk(dk), target(tgt), write(wr), d0(start), bucket0(start), bucketT0(t0) {
    th = std::thread([this] {
      const struct timespec tick { 0, 50 * 1000 * 1000 };
      while (running.load(std::memory_order_relaxed) && filled < kCacheCurveBuckets) {
        const Dev dn = devRead(disk);
        if (dn.valid && d0.valid) {
          const uint64_t s0 = write ? d0.wrSect : d0.rdSect;
          const uint64_t sn = write ? dn.wrSect : dn.rdSect;
          const uint64_t sb = write ? bucket0.wrSect : bucket0.rdSect;
          const uint64_t done = (sn - s0) * 512ull;
          while (filled < kCacheCurveBuckets &&
                 done >= target / kCacheCurveBuckets * static_cast<uint64_t>(filled + 1)) {
            const double dt = nowS() - bucketT0;
            if (dt > 0)
              p.curve[filled] = static_cast<double>(sn - sb) * 512.0 / 1e6 / dt;
            bucket0 = dn;
            bucketT0 = nowS();
            ++filled;
          }
        }
        ::nanosleep(&tick, nullptr);
      }
    });
  }
  void stop() {
    running.store(false);
    if (th.joinable())
      th.join();
    p.curveN = filled;
  }
};

void finish(CachePhase& p, const Dev& d0, const Dev& d1, double elapsed, bool write) {
  p.seconds = elapsed;
  p.payloadMbps = elapsed > 0 ? static_cast<double>(p.bytes) / 1e6 / elapsed : 0;
  if (!d0.valid || !d1.valid || elapsed <= 0)
    return;
  const double mb = write ? static_cast<double>(d1.wrSect - d0.wrSect) * 512.0 / 1e6
                          : static_cast<double>(d1.rdSect - d0.rdSect) * 512.0 / 1e6;
  const double ops = write ? static_cast<double>(d1.wr - d0.wr) : static_cast<double>(d1.rd - d0.rd);
  p.devMbps = mb / elapsed;
  p.devOpKib = ops > 0 ? mb * 1e6 / ops / 1024.0 : 0;
  p.devQueueDepth = static_cast<double>(d1.weightedMs - d0.weightedMs) / (elapsed * 1000.0);
}

} // namespace

CacheBenchResult runCacheBench(const CacheBenchOpts& o) {
  CacheBenchResult r;
  const int nEntries = std::max(1, o.entries);
  const uint64_t blk = std::max<uint64_t>(4096, o.fillBlock / 4096 * 4096);
  // Every entry the same size, and a whole number of blocks, so "write each
  // block once" gives an exact volume.
  uint64_t per = o.sampleBytes / static_cast<uint64_t>(nEntries) / blk * blk;
  if (per < blk)
    per = blk;
  r.sampleBytes = per * static_cast<uint64_t>(nEntries);
  r.entries = nEntries;
  r.writers = std::max(1, o.writers);
  r.threads = std::max(1, o.threads);
  r.fillBlockKib = blk / 1024;
  r.byteReadKib = std::max<uint64_t>(4096, o.byteReadBlock) / 1024;
  r.replicaReadKib = std::max<uint64_t>(4096, o.replicaReadBlock) / 1024;
  r.dirtyLimitMib = o.dirtyLimitGb * 1024.0;
  r.volumeExceedsDirtyLimit =
      o.dirtyLimitGb > 0 &&
      static_cast<double>(r.sampleBytes) > o.dirtyLimitGb * static_cast<double>(1ull << 30);

  const std::string cacheDir = o.dir + "/ucache-path";
  if (::mkdir(cacheDir.c_str(), 0700) != 0) {
    r.error = std::string("mkdir ") + cacheDir + ": " + std::strerror(errno);
    return r;
  }

  // Every return below frees the tree; the store must be destroyed first so its
  // final stats dump does not recreate files underneath us.
  struct TreeGuard {
    std::string dir;
    ~TreeGuard() { removeTree(dir); }
  } guard{cacheDir};

  Config cfg;
  cfg.cacheDir = cacheDir;
  // Eviction OFF, explicitly: no byte cap, no free-space floor, and budgetAuto
  // false so CacheStore does not resolve one of its own. A store that evicts
  // mid-measurement is measuring eviction.
  cfg.maxBytes = 0;
  cfg.minFreeBytes = 0;
  cfg.budgetAuto = false;
  // The drain trigger is a race between a size cap and a wall clock. Pin the
  // clock far away so only the size cap fires and the run is deterministic.
  cfg.metaFlushSeconds = 24 * 3600;
  cfg.fsync = FsyncMode::kOff; // the product default: nothing syncs during a fill
  if (o.fillBufferMb >= 0)
    cfg.fillBufferMb = o.fillBufferMb; // 0 = unstaged, for the variant leg

  r.fillBufferMb = cfg.fillBufferMb;
  RealIO io;
  // Scoped so the store (and its exit-time stats dump) is gone before TreeGuard
  // removes the directory.
  CacheStore store(io, cfg);

  std::vector<UrlKey> keys;
  std::vector<std::string> dataPaths;
  for (int i = 0; i < nEntries; ++i) {
    char url[128];
    std::snprintf(url, sizeof url, "root://bench.invalid:1094/ucache-path/entry%04d", i);
    auto k = UrlKey::parse(url);
    if (!k) {
      r.error = "internal: benchmark key did not parse";
      return r;
    }
    keys.push_back(*k);
    dataPaths.push_back(k->dataPath(cacheDir));
  }

  // ---------------- FILL ----------------------------------------------------
  {
    std::vector<uint8_t> payload(static_cast<size_t>(blk), 0xA5);
    std::atomic<uint64_t> bytes{0}, ops{0};
    std::atomic<int> next{0};
    std::vector<uint32_t> us;
    std::mutex mu;
    const Dev d0 = devRead(o.diskName);
    const double t0 = nowS();

    // Bucket the curve on DEVICE bytes: below the dirty limit the writer runs
    // far ahead of the disk, so an application-bucketed curve fires every
    // boundary before the device has moved.
    std::atomic<bool> filling{true};
    Dev bucket0 = d0;
    double bucketT0 = t0;
    int filled = 0;
    std::thread sampler([&] {
      const struct timespec tick { 0, 50 * 1000 * 1000 };
      while (filling.load(std::memory_order_relaxed) && filled < kCacheCurveBuckets) {
        r.peakDirtyMib = std::max(r.peakDirtyMib, dirtyMib());
        const Dev dn = devRead(o.diskName);
        if (dn.valid && d0.valid) {
          const uint64_t done = (dn.wrSect - d0.wrSect) * 512ull;
          while (filled < kCacheCurveBuckets &&
                 done >= r.sampleBytes / kCacheCurveBuckets * uint64_t(filled + 1)) {
            const double dt = nowS() - bucketT0;
            if (dt > 0)
              r.fill.curve[filled] =
                  static_cast<double>(dn.wrSect - bucket0.wrSect) * 512.0 / 1e6 / dt;
            bucket0 = dn;
            bucketT0 = nowS();
            ++filled;
          }
        }
        ::nanosleep(&tick, nullptr);
      }
      // The buckets can fill before the writing does; dirty keeps moving until
      // the last drain, so keep watching it until the phase ends.
      while (filling.load(std::memory_order_relaxed)) {
        r.peakDirtyMib = std::max(r.peakDirtyMib, dirtyMib());
        ::nanosleep(&tick, nullptr);
      }
    });

    std::vector<std::thread> pool;
    for (int w = 0; w < std::max(1, o.writers); ++w)
      pool.emplace_back([&] {
        for (;;) {
          const int i = next.fetch_add(1);
          if (i >= nEntries)
            return;
          auto e = store.open(keys[static_cast<size_t>(i)], per);
          if (!e)
            continue;
          const auto order = scatterOrder(per / blk, 1000 + static_cast<uint64_t>(i));
          std::vector<uint32_t> mine;
          for (uint64_t s : order) {
            const double a = nowS();
            e->writePages(s * blk, blk, payload.data());
            mine.push_back(static_cast<uint32_t>((nowS() - a) * 1e6));
            bytes += blk;
            ++ops;
          }
          // Releasing the entry flushes whatever is still staged in RAM.
          e.reset();
          std::lock_guard<std::mutex> g(mu);
          us.insert(us.end(), mine.begin(), mine.end());
        }
      });
    for (auto& t : pool)
      t.join();
    const double writeEnd = nowS();
    // Durability is timed apart: the product never syncs, but the bytes must be
    // on the device for the device totals to mean anything and for the read
    // phase's page-cache drop to work at all.
    syncPaths(dataPaths);
    const double t1 = nowS();
    filling.store(false);
    sampler.join();
    const Dev d1 = devRead(o.diskName);

    r.fill.bytes = bytes.load();
    r.fill.ops = ops.load();
    r.fill.syncS = t1 - writeEnd;
    pct(us, r.fill);
    finish(r.fill, d0, d1, t1 - t0, /*write=*/true);
    r.fill.curveN = filled;
    r.fill.ok = r.fill.bytes > 0;
  }

  // The product's own view of what it just did.
  r.stalls = store.stats().bufferStalls.load();
  r.stallMs = static_cast<double>(store.stats().bufferStallUs.load()) / 1000.0;
  // Against total writer thread-time, not wall time: with N writers there are N
  // seconds of thread-time per second, and it is the fraction of that spent
  // waiting for the disk that says the disk was the constraint.
  const double threadS = r.fill.seconds * static_cast<double>(r.writers);
  r.stallShare = threadS > 0 ? (r.stallMs / 1000.0) / threadS : 0;
  r.flushRuns = store.stats().flushRuns.load();
  r.flushRunBytes = store.stats().flushRunBytes.load();

  // ---------------- READ, both tier shapes ---------------------------------
  auto readPhase = [&](CachePhase& p, uint64_t rblk, bool sequential) {
    dropCache(dataPaths); // clean pages only, hence the sync above
    std::vector<uint8_t> buf(static_cast<size_t>(rblk));
    std::atomic<uint64_t> bytes{0}, ops{0};
    std::atomic<int> next{0};
    std::vector<uint32_t> us;
    std::mutex mu;
    const Dev d0 = devRead(o.diskName);
    const double t0 = nowS();
    Curve curve(p, o.diskName, r.sampleBytes, /*write=*/false, d0, t0);
    std::vector<std::thread> pool;
    for (int t = 0; t < std::max(1, o.threads); ++t)
      pool.emplace_back([&] {
        std::vector<uint8_t> mybuf(static_cast<size_t>(rblk));
        std::vector<uint32_t> mine;
        for (;;) {
          const int i = next.fetch_add(1);
          if (i >= nEntries)
            break;
          auto e = store.open(keys[static_cast<size_t>(i)], per);
          if (!e)
            continue;
          const uint64_t nblk = per / rblk;
          if (nblk == 0)
            continue;
          // Each byte exactly once: a second pass would answer from RAM.
          std::vector<uint64_t> order;
          if (sequential) {
            order.resize(static_cast<size_t>(nblk));
            for (uint64_t j = 0; j < nblk; ++j)
              order[static_cast<size_t>(j)] = j;
          } else {
            order = scatterOrder(nblk, 2000 + static_cast<uint64_t>(i));
          }
          for (uint64_t s : order) {
            const double a = nowS();
            if (!e->readCached(s * rblk, rblk, mybuf.data()))
              continue; // a miss here would be a bug; counted by omission
            mine.push_back(static_cast<uint32_t>((nowS() - a) * 1e6));
            bytes += rblk;
            ++ops;
          }
        }
        std::lock_guard<std::mutex> g(mu);
        us.insert(us.end(), mine.begin(), mine.end());
      });
    for (auto& t : pool)
      t.join();
    curve.stop();
    const double t1 = nowS();
    const Dev d1 = devRead(o.diskName);
    p.bytes = bytes.load();
    p.ops = ops.load();
    pct(us, p);
    finish(p, d0, d1, t1 - t0, /*write=*/false);
    p.ok = p.bytes > 0;
  };
  readPhase(r.readByte, std::max<uint64_t>(4096, o.byteReadBlock), /*sequential=*/false);
  readPhase(r.readReplica, std::max<uint64_t>(4096, o.replicaReadBlock), /*sequential=*/true);

  r.hitDiskReads = store.stats().hitDiskReads.load();
  r.hitDiskBytes = store.stats().hitDiskBytes.load();
  return r;
}

} // namespace ucache
