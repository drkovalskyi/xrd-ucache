#include "DiskBench.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <random>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>
#include <thread>
#include <unistd.h>

namespace ucache {
namespace {

constexpr size_t kAlign = 4096;
constexpr size_t kSmall = 4096;            // "page/basket-ish" IO
constexpr size_t kBig = 4ull << 20;        // "replica/batched-fill" IO
constexpr uint64_t kMinFile = 16ull << 20; // give up below this

double nowS() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

char* alignedBuf(size_t n) {
  void* p = nullptr;
  if (posix_memalign(&p, kAlign, n) != 0)
    return nullptr;
  std::memset(p, 0xA5, n); // non-zero: defeats potential zero-detection tiers
  return static_cast<char*>(p);
}

struct Pct {
  uint64_t p50 = 0, p95 = 0, p99 = 0;
};
Pct percentiles(std::vector<uint32_t>& us) {
  Pct r;
  if (us.empty())
    return r;
  std::sort(us.begin(), us.end());
  r.p50 = us[us.size() / 2];
  r.p95 = us[std::min(us.size() - 1, us.size() * 95 / 100)];
  r.p99 = us[std::min(us.size() - 1, us.size() * 99 / 100)];
  return r;
}

std::string fsName(const std::string& path) {
  struct ::statfs sf;
  if (::statfs(path.c_str(), &sf) != 0)
    return "?";
  switch (static_cast<unsigned>(sf.f_type)) {
  case 0x58465342: return "xfs";
  case 0xEF53: return "ext4";
  case 0x01021994: return "tmpfs";
  case 0x9123683E: return "btrfs";
  case 0x6969: return "nfs";
  case 0x00C36400: return "ceph";
  case 0x65735546: return "fuse";
  case 0x65735543: return "fuse.ctl";
  case 0x5346414F: return "afs";
  case 0x794C7630: return "overlay";
  case 0x2FC12FC1: return "zfs";
  default: {
    char b[24];
    std::snprintf(b, sizeof b, "0x%lx", static_cast<unsigned long>(sf.f_type));
    return b;
  }
  }
}

// Latency-sampling random-IO loop: runs until deadline, records per-op µs.
// Returns ops done. `sample` may be null (count only — the N-stream phase).
uint64_t randLoop(int fd, uint64_t fsz, bool write, char* buf, double deadline,
                  std::vector<uint32_t>* sample, uint64_t seed) {
  std::mt19937_64 rng(seed);
  const uint64_t slots = (fsz - kSmall) / kAlign;
  uint64_t ops = 0;
  while (nowS() < deadline) {
    uint64_t off = (rng() % slots) * kAlign;
    double t0 = nowS();
    ssize_t r = write ? ::pwrite(fd, buf, kSmall, static_cast<off_t>(off))
                      : ::pread(fd, buf, kSmall, static_cast<off_t>(off));
    if (r != static_cast<ssize_t>(kSmall))
      break; // IO error: stop the phase, keep what we measured
    if (sample && sample->size() < 500000)
      sample->push_back(static_cast<uint32_t>(
          std::min(4.0e9, (nowS() - t0) * 1e6)));
    ++ops;
  }
  return ops;
}

struct Result {
  std::string path, fs, mode, error;
  double totalGb = 0, freeGb = 0, fileMb = 0;
  double seqWriteMbps = 0, seqReadMbps = 0;
  double fsyncP50Ms = 0;
  double randwIops = 0, randr1Iops = 0, randrNIops = 0;
  Pct randr1;
  double mixedReadIops = 0, mixedWriteMbps = 0;
  Pct mixed;
  double createPs = 0, unlinkPs = 0;
  bool ok = false;
};

void printHuman(const Result& r, int wideStreams) {
  std::printf("=== ucache bench: %s (%s, %.1f GiB total, %.1f GiB free, %s) ===\n",
              r.path.c_str(), r.fs.c_str(), r.totalGb, r.freeGb, r.mode.c_str());
  if (!r.error.empty()) {
    std::printf("  FAILED: %s\n\n", r.error.c_str());
    return;
  }
  std::printf("  test file               %.0f MiB\n", r.fileMb);
  std::printf("  sequential write        %8.1f MB/s\n", r.seqWriteMbps);
  std::printf("  fdatasync               %8.2f ms (p50)\n", r.fsyncP50Ms);
  std::printf("  scattered 4KiB write    %8.0f IOPS\n", r.randwIops);
  std::printf("  random 4KiB read (1)    %8.0f IOPS   p50 %.2f ms   p95 %.2f ms   p99 %.2f ms\n",
              r.randr1Iops, r.randr1.p50 / 1e3, r.randr1.p95 / 1e3, r.randr1.p99 / 1e3);
  std::printf("  random 4KiB read (%d)   %8.0f IOPS   (scaling %.1fx)\n", wideStreams,
              r.randrNIops, r.randr1Iops > 0 ? r.randrNIops / r.randr1Iops : 0.0);
  std::printf("  sequential read         %8.1f MB/s\n", r.seqReadMbps);
  std::printf("  read under writeback    %8.0f IOPS   p50 %.2f ms   p99 %.2f ms   (write %.1f MB/s)\n",
              r.mixedReadIops, r.mixed.p50 / 1e3, r.mixed.p99 / 1e3, r.mixedWriteMbps);
  std::printf("  create / unlink         %8.0f /s   %8.0f /s\n\n", r.createPs, r.unlinkPs);
}

void printJson(const Result& r, const DiskBenchOpts& o) {
  char host[256] = "?";
  ::gethostname(host, sizeof host - 1);
  std::printf(
      "ucache-bench-json: {\"schema\":1,\"host\":\"%s\",\"path\":\"%s\",\"fs\":\"%s\","
      "\"mode\":\"%s\",\"total_gb\":%.1f,\"free_gb\":%.1f,\"file_mb\":%.0f,"
      "\"phase_s\":%.1f,\"error\":\"%s\",\"seq_write_mbps\":%.1f,"
      "\"fsync_p50_ms\":%.2f,\"randw_iops\":%.0f,\"randr1_iops\":%.0f,"
      "\"randr1_us_p50\":%llu,\"randr1_us_p95\":%llu,\"randr1_us_p99\":%llu,"
      "\"randr%d_iops\":%.0f,\"seq_read_mbps\":%.1f,\"mixed_read_iops\":%.0f,"
      "\"mixed_us_p50\":%llu,\"mixed_us_p99\":%llu,\"mixed_write_mbps\":%.1f,"
      "\"create_ps\":%.0f,\"unlink_ps\":%.0f}\n",
      host, r.path.c_str(), r.fs.c_str(), r.mode.c_str(), r.totalGb, r.freeGb,
      r.fileMb, o.phaseSeconds, r.error.c_str(), r.seqWriteMbps, r.fsyncP50Ms,
      r.randwIops, r.randr1Iops, static_cast<unsigned long long>(r.randr1.p50),
      static_cast<unsigned long long>(r.randr1.p95),
      static_cast<unsigned long long>(r.randr1.p99), o.wideStreams, r.randrNIops,
      r.seqReadMbps, r.mixedReadIops, static_cast<unsigned long long>(r.mixed.p50),
      static_cast<unsigned long long>(r.mixed.p99), r.mixedWriteMbps, r.createPs,
      r.unlinkPs);
}

// Evict the file's pages so buffered-mode reads see the device, not RAM.
void evict(int fd) {
  ::fdatasync(fd);
  ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
}

Result benchOne(const std::string& path, const DiskBenchOpts& o) {
  Result r;
  r.path = path;
  r.fs = fsName(path);

  struct ::statvfs vfs;
  if (::statvfs(path.c_str(), &vfs) != 0) {
    r.error = std::string("statvfs failed: ") + std::strerror(errno);
    return r;
  }
  r.totalGb = static_cast<double>(vfs.f_blocks) * vfs.f_frsize / (1ull << 30);
  r.freeGb = static_cast<double>(vfs.f_bavail) * vfs.f_frsize / (1ull << 30);
  uint64_t want = std::max<uint64_t>(o.fileBytes, kMinFile);
  if (r.freeGb * (1ull << 30) < static_cast<double>(want) + (64ull << 20)) {
    r.error = "not enough free space for the test file (need --size + 64 MiB)";
    return r;
  }

  char dir[4096];
  std::snprintf(dir, sizeof dir, "%s/.ucache-bench.%d", path.c_str(),
                static_cast<int>(::getpid()));
  if (::mkdir(dir, 0700) != 0) {
    r.error = std::string("mkdir failed: ") + std::strerror(errno);
    return r;
  }
  std::string tf = std::string(dir) + "/testfile";

  // Cleanup runs on every exit path of this function.
  struct Cleanup {
    std::string tf, dir;
    ~Cleanup() {
      ::unlink(tf.c_str());
      ::rmdir(dir.c_str());
    }
  } cleanup{tf, dir};

  // O_DIRECT when the fs takes it; else buffered + eviction (mode reported).
  bool direct = true;
  int wfd = ::open(tf.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0600);
  if (wfd < 0) {
    direct = false;
    wfd = ::open(tf.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  }
  if (wfd < 0) {
    r.error = std::string("open failed: ") + std::strerror(errno);
    return r;
  }
  r.mode = direct ? "O_DIRECT" : "buffered";

  char* big = alignedBuf(kBig);
  char* small = alignedBuf(kSmall);
  if (!big || !small) {
    ::close(wfd);
    r.error = "alloc failed";
    return r;
  }
  struct Bufs {
    char *a, *b;
    ~Bufs() {
      ::free(a);
      ::free(b);
    }
  } bufs{big, small};

  // --- sequential write (the batched-fill / replica-build pattern) --------
  uint64_t fsz = 0;
  {
    double t0 = nowS(), cap = t0 + std::max(o.phaseSeconds * 3, 10.0);
    while (fsz < want && nowS() < cap) {
      uint64_t n = std::min<uint64_t>(kBig, want - fsz);
      n = n / kAlign * kAlign;
      if (::pwrite(wfd, big, n, static_cast<off_t>(fsz)) != static_cast<ssize_t>(n)) {
        r.error = std::string("write failed: ") + std::strerror(errno);
        ::close(wfd);
        return r;
      }
      fsz += n;
    }
    ::fdatasync(wfd);
    r.seqWriteMbps = static_cast<double>(fsz) / 1e6 / (nowS() - t0);
  }
  if (fsz < kMinFile) {
    r.error = "could not write a 16 MiB test file in time (device too slow?)";
    ::close(wfd);
    return r;
  }
  r.fileMb = static_cast<double>(fsz) / (1 << 20);

  // --- fdatasync latency (sidecar-flush pattern); buffered fd on purpose --
  {
    int bfd = ::open(tf.c_str(), O_WRONLY, 0600);
    std::vector<uint32_t> us;
    for (int i = 0; i < 5 && bfd >= 0; ++i) {
      ::pwrite(bfd, small, kSmall, static_cast<off_t>((i * 977ull * kAlign) % fsz));
      double t0 = nowS();
      ::fdatasync(bfd);
      us.push_back(static_cast<uint32_t>((nowS() - t0) * 1e6));
    }
    if (bfd >= 0)
      ::close(bfd);
    r.fsyncP50Ms = percentiles(us).p50 / 1e3;
  }

  // --- scattered 4 KiB writes (the CURRENT fill pattern) ------------------
  {
    double deadline = nowS() + o.phaseSeconds;
    double t0 = nowS();
    uint64_t ops = randLoop(wfd, fsz, /*write=*/true, small, deadline, nullptr, 1);
    if (!direct)
      ::fdatasync(wfd); // buffered: bill the writeback inside the phase
    r.randwIops = static_cast<double>(ops) / (nowS() - t0);
  }
  ::close(wfd);

  int rflags = O_RDONLY | (direct ? O_DIRECT : 0);
  int rfd = ::open(tf.c_str(), rflags);
  if (rfd < 0) {
    r.error = std::string("reopen failed: ") + std::strerror(errno);
    return r;
  }
  if (!direct)
    evict(rfd);

  // --- random 4 KiB read, 1 stream (hit-serving latency) ------------------
  {
    std::vector<uint32_t> us;
    double t0 = nowS();
    uint64_t ops = randLoop(rfd, fsz, false, small, t0 + o.phaseSeconds, &us, 2);
    r.randr1Iops = static_cast<double>(ops) / (nowS() - t0);
    r.randr1 = percentiles(us);
  }

  // --- random 4 KiB read, N streams (quota / capacity exposure) -----------
  {
    if (!direct)
      evict(rfd);
    std::atomic<uint64_t> total{0};
    double t0 = nowS(), deadline = t0 + o.phaseSeconds;
    std::vector<std::thread> pool;
    for (int t = 0; t < o.wideStreams; ++t)
      pool.emplace_back([&, t] {
        int fd = ::open(tf.c_str(), rflags);
        char* buf = alignedBuf(kSmall);
        if (fd >= 0 && buf)
          total += randLoop(fd, fsz, false, buf, deadline, nullptr, 100 + t);
        if (fd >= 0)
          ::close(fd);
        ::free(buf);
      });
    for (auto& t : pool)
      t.join();
    r.randrNIops = static_cast<double>(total.load()) / (nowS() - t0);
  }

  // --- sequential read (the replica-serving pattern) ----------------------
  {
    if (!direct)
      evict(rfd);
    double t0 = nowS(), deadline = t0 + o.phaseSeconds;
    uint64_t off = 0, bytes = 0;
    while (nowS() < deadline) {
      uint64_t n = std::min<uint64_t>(kBig, fsz - off) / kAlign * kAlign;
      if (n == 0 || ::pread(rfd, big, n, static_cast<off_t>(off)) != static_cast<ssize_t>(n))
        break;
      bytes += n;
      off += n;
      if (off + kAlign >= fsz)
        off = 0; // wrap: keep streaming for the full phase
    }
    r.seqReadMbps = static_cast<double>(bytes) / 1e6 / (nowS() - t0);
  }

  // --- random reads UNDER writeback (the production killer mode) ----------
  {
    if (!direct)
      evict(rfd);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> wbytes{0};
    std::thread writer([&] {
      int fd = ::open(tf.c_str(), O_WRONLY | (direct ? O_DIRECT : 0));
      char* buf = alignedBuf(kBig);
      uint64_t off = 0;
      while (fd >= 0 && buf && !stop.load(std::memory_order_relaxed)) {
        uint64_t n = std::min<uint64_t>(kBig, fsz - off) / kAlign * kAlign;
        if (n == 0 || ::pwrite(fd, buf, n, static_cast<off_t>(off)) != static_cast<ssize_t>(n))
          break;
        wbytes += n;
        off += n;
        if (off + kAlign >= fsz)
          off = 0;
      }
      if (fd >= 0) {
        if (!direct)
          ::fdatasync(fd);
        ::close(fd);
      }
      ::free(buf);
    });
    std::mutex mu;
    std::vector<uint32_t> us;
    std::atomic<uint64_t> rops{0};
    double t0 = nowS(), deadline = t0 + o.phaseSeconds;
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
      readers.emplace_back([&, t] {
        int fd = ::open(tf.c_str(), rflags);
        char* buf = alignedBuf(kSmall);
        std::vector<uint32_t> mine;
        if (fd >= 0 && buf)
          rops += randLoop(fd, fsz, false, buf, deadline, &mine, 200 + t);
        if (fd >= 0)
          ::close(fd);
        ::free(buf);
        std::lock_guard<std::mutex> g(mu);
        us.insert(us.end(), mine.begin(), mine.end());
      });
    for (auto& t : readers)
      t.join();
    double elapsed = nowS() - t0;
    stop = true;
    writer.join();
    r.mixedReadIops = static_cast<double>(rops.load()) / elapsed;
    r.mixed = percentiles(us);
    r.mixedWriteMbps = static_cast<double>(wbytes.load()) / 1e6 / elapsed;
  }
  ::close(rfd);

  // --- create / unlink (the cleanup / eviction pattern) -------------------
  {
    const int n = 200;
    std::vector<std::string> names;
    names.reserve(n);
    double t0 = nowS();
    for (int i = 0; i < n; ++i) {
      char nm[4200];
      std::snprintf(nm, sizeof nm, "%s/m%04d", dir, i);
      int fd = ::open(nm, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0)
        break;
      ::write(fd, small, kSmall);
      ::close(fd);
      names.push_back(nm);
    }
    double t1 = nowS();
    for (const auto& nm : names)
      ::unlink(nm.c_str());
    double t2 = nowS();
    if (!names.empty()) {
      r.createPs = names.size() / std::max(1e-9, t1 - t0);
      r.unlinkPs = names.size() / std::max(1e-9, t2 - t1);
    }
  }

  r.ok = true;
  return r;
}

} // namespace

int runDiskBench(const std::vector<std::string>& paths, const DiskBenchOpts& opts) {
  std::vector<Result> results;
  for (const auto& p : paths) {
    Result r = benchOne(p, opts);
    printHuman(r, opts.wideStreams);
    printJson(r, opts);
    results.push_back(std::move(r));
  }
  if (results.size() > 1) {
    std::printf("\n=== comparison ===\n%-24s", "metric");
    for (const auto& r : results)
      std::printf("  %16.16s", r.path.c_str());
    std::printf("\n");
    auto row = [&](const char* name, auto get, const char* fmt) {
      std::printf("%-24s", name);
      for (const auto& r : results) {
        if (r.ok)
          std::printf(fmt, get(r));
        else
          std::printf("  %16s", "-");
      }
      std::printf("\n");
    };
    row("seq write MB/s", [](const Result& r) { return r.seqWriteMbps; }, "  %16.1f");
    row("seq read MB/s", [](const Result& r) { return r.seqReadMbps; }, "  %16.1f");
    row("rand read IOPS (1)", [](const Result& r) { return r.randr1Iops; }, "  %16.0f");
    row("rand read IOPS (N)", [](const Result& r) { return r.randrNIops; }, "  %16.0f");
    row("rand read p50 ms", [](const Result& r) { return r.randr1.p50 / 1e3; }, "  %16.2f");
    row("scattered write IOPS", [](const Result& r) { return r.randwIops; }, "  %16.0f");
    row("read-under-wb p99 ms", [](const Result& r) { return r.mixed.p99 / 1e3; }, "  %16.2f");
    row("fdatasync p50 ms", [](const Result& r) { return r.fsyncP50Ms; }, "  %16.2f");
    row("unlink /s", [](const Result& r) { return r.unlinkPs; }, "  %16.0f");
  }
  for (const auto& r : results)
    if (!r.ok)
      return 1;
  return 0;
}

} // namespace ucache
