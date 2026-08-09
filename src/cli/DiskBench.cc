#include "DiskBench.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <thread>
#include <unistd.h>

#ifndef UCACHE_VERSION
#define UCACHE_VERSION "unknown"
#endif

namespace ucache {
namespace {

constexpr size_t kAlign = 4096;
constexpr size_t kSmall = 4096;            // "page/basket-ish" IO
constexpr uint64_t kMinFile = 16ull << 20; // give up below this
// Buffers for the large-block phases are per stream; at 64 streams x a big
// --block they would dwarf the machine. Shrink the block instead of the
// stream count — the stream count is what the caller asked to measure.
constexpr uint64_t kMaxStreamBufTotal = 1ull << 30;
// A quarter-window shorter than this cannot carry an honest rate, so the
// burst/sustained split is only attempted on windows of 4x it or more.
constexpr double kMinQuarterS = 0.5;

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

// printf-into-a-string: the human table is printed AND logged, and one
// formatter for both is the only way they cannot drift apart.
void appendf(std::string& s, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));
void appendf(std::string& s, const char* fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = std::vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t base = s.size();
    s.resize(base + static_cast<size_t>(n));
    std::vsnprintf(&s[base], static_cast<size_t>(n) + 1, fmt, ap2);
  }
  va_end(ap2);
}

std::string jesc(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '"': o += "\\\""; break;
    case '\\': o += "\\\\"; break;
    case '\n': o += "\\n"; break;
    case '\r': o += "\\r"; break;
    case '\t': o += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char b[8];
        std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(c) & 0xff);
        o += b;
      } else {
        o.push_back(c);
      }
    }
  }
  return o;
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

// ---------------------------------------------------------------------------
// Run context: what the numbers mean depends on which device answered them and
// how busy the machine was, so both are captured and recorded next to them.
// ---------------------------------------------------------------------------

std::string readFileTrim(const std::string& p) {
  std::ifstream f(p);
  std::string s;
  if (!std::getline(f, s))
    return "";
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  size_t b = s.find_first_not_of(' ');
  return b == std::string::npos ? "" : s.substr(b);
}

// mountinfo escapes space/tab/newline/backslash as \0NN.
std::string unescapeOctal(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
      o.push_back(static_cast<char>((s[i + 1] - '0') * 64 + (s[i + 2] - '0') * 8 + (s[i + 3] - '0')));
      i += 3;
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

bool pathUnder(const std::string& mp, const std::string& rp) {
  if (mp == "/")
    return true;
  if (rp.size() < mp.size() || rp.compare(0, mp.size(), mp) != 0)
    return false;
  return rp.size() == mp.size() || rp[mp.size()] == '/';
}

struct MountInfo {
  bool found = false;
  std::string mountPoint, fsType, source, opts, superOpts;
  unsigned maj = 0, min = 0;
  bool haveBlock = false;
  std::string partName, diskName, model, sched;
  int rotational = -1;
  double sizeGb = 0;
};

// The scheduler file reads "none [mq-deadline] kyber bfq" — only the selected
// one is a fact about this device.
std::string selectedSched(const std::string& s) {
  size_t a = s.find('[');
  size_t b = s.find(']');
  if (a != std::string::npos && b != std::string::npos && b > a)
    return s.substr(a + 1, b - a - 1);
  return s;
}

MountInfo mountFor(const std::string& path) {
  MountInfo m;
  char rbuf[4096];
  std::string rp = ::realpath(path.c_str(), rbuf) ? std::string(rbuf) : path;

  struct ::stat st;
  bool haveSt = ::stat(rp.c_str(), &st) == 0;
  unsigned wantMaj = haveSt ? ::major(st.st_dev) : 0;
  unsigned wantMin = haveSt ? ::minor(st.st_dev) : 0;

  std::ifstream f("/proc/self/mountinfo");
  std::string line;
  size_t bestLen = 0;
  bool bestDev = false;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::vector<std::string> tok;
    std::string t;
    while (is >> t)
      tok.push_back(t);
    if (tok.size() < 7)
      continue;
    size_t dash = 0;
    while (dash < tok.size() && tok[dash] != "-")
      ++dash;
    if (dash + 2 >= tok.size())
      continue;
    unsigned mj = 0, mn = 0;
    if (std::sscanf(tok[2].c_str(), "%u:%u", &mj, &mn) != 2)
      continue;
    std::string mp = unescapeOctal(tok[4]);
    if (!pathUnder(mp, rp))
      continue;
    // Prefer the line whose device matches stat()'s; among equals the longest
    // mount point, and the last such line (later mounts shadow earlier ones).
    bool devMatch = haveSt && mj == wantMaj && mn == wantMin;
    if (bestDev && !devMatch)
      continue;
    if (devMatch == bestDev && mp.size() < bestLen)
      continue;
    bestDev = devMatch;
    bestLen = mp.size();
    m.found = true;
    m.mountPoint = mp;
    m.opts = tok[5];
    m.maj = mj;
    m.min = mn;
    m.fsType = tok[dash + 1];
    m.source = unescapeOctal(tok[dash + 2]);
    // Superblock options carry the ones that change the numbers (discard,
    // noquota, data=, nobarrier); the per-mount ones rarely do.
    m.superOpts = dash + 3 < tok.size() ? tok[dash + 3] : "";
  }
  if (!m.found)
    return m;

  char sysdev[128];
  std::snprintf(sysdev, sizeof sysdev, "/sys/dev/block/%u:%u", m.maj, m.min);
  char lnk[4096];
  ssize_t n = ::readlink(sysdev, lnk, sizeof lnk - 1);
  if (n <= 0)
    return m; // tmpfs, nfs, fuse, ... — no block device behind it
  lnk[n] = '\0';
  std::string p(lnk);
  size_t slash = p.find_last_of('/');
  m.partName = slash == std::string::npos ? p : p.substr(slash + 1);
  m.haveBlock = true;
  struct ::stat pst;
  if (::stat((std::string(sysdev) + "/partition").c_str(), &pst) == 0 &&
      slash != std::string::npos) {
    std::string parent = p.substr(0, slash);
    size_t s2 = parent.find_last_of('/');
    m.diskName = s2 == std::string::npos ? parent : parent.substr(s2 + 1);
  } else {
    m.diskName = m.partName;
  }
  const std::string base = "/sys/block/" + m.diskName;
  std::string rot = readFileTrim(base + "/queue/rotational");
  m.rotational = rot.empty() ? -1 : std::atoi(rot.c_str());
  m.sched = selectedSched(readFileTrim(base + "/queue/scheduler"));
  m.model = readFileTrim(base + "/device/model");
  if (m.model.empty())
    m.model = readFileTrim(base + "/dm/name"); // device-mapper: the friendly name
  std::string sz = readFileTrim(base + "/size");
  if (!sz.empty())
    m.sizeGb = static_cast<double>(std::strtoull(sz.c_str(), nullptr, 10)) * 512.0 / (1ull << 30);
  return m;
}

struct DiskCounters {
  bool valid = false;
  uint64_t reads = 0, rdSect = 0, writes = 0, wrSect = 0, ticksMs = 0;
};

DiskCounters diskCounters(const std::string& disk) {
  DiskCounters d;
  if (disk.empty())
    return d;
  std::ifstream f("/proc/diskstats");
  std::string line;
  while (std::getline(f, line)) {
    std::istringstream is(line);
    std::vector<std::string> tok;
    std::string t;
    while (is >> t)
      tok.push_back(t);
    if (tok.size() < 14 || tok[2] != disk)
      continue;
    d.valid = true;
    d.reads = std::strtoull(tok[3].c_str(), nullptr, 10);
    d.rdSect = std::strtoull(tok[5].c_str(), nullptr, 10);
    d.writes = std::strtoull(tok[7].c_str(), nullptr, 10);
    d.wrSect = std::strtoull(tok[9].c_str(), nullptr, 10);
    d.ticksMs = std::strtoull(tok[12].c_str(), nullptr, 10);
    break;
  }
  return d;
}

struct CpuCounters {
  bool valid = false;
  uint64_t total = 0, idle = 0, iowait = 0;
};

CpuCounters cpuCounters() {
  CpuCounters c;
  std::ifstream f("/proc/stat");
  std::string line;
  if (!std::getline(f, line) || line.compare(0, 4, "cpu ") != 0)
    return c;
  std::istringstream is(line.substr(4));
  uint64_t v = 0;
  for (int i = 0; is >> v; ++i) {
    c.total += v;
    if (i == 3)
      c.idle = v;
    if (i == 4)
      c.iowait = v;
  }
  c.valid = c.total > 0;
  return c;
}

struct LoadAvg {
  double l1 = 0, l5 = 0, l15 = 0;
};
LoadAvg loadAvg() {
  LoadAvg l;
  std::ifstream f("/proc/loadavg");
  f >> l.l1 >> l.l5 >> l.l15;
  return l;
}

struct Machine {
  std::string host, kernel, arch, cpuModel;
  long ncpu = 0;
  double memGb = 0;
};

Machine machineInfo() {
  Machine m;
  char host[256] = "?";
  ::gethostname(host, sizeof host - 1);
  m.host = host;
  struct ::utsname u;
  if (::uname(&u) == 0) {
    m.kernel = u.release;
    m.arch = u.machine;
  }
  m.ncpu = ::sysconf(_SC_NPROCESSORS_ONLN);
  std::ifstream ci("/proc/cpuinfo");
  std::string line;
  while (std::getline(ci, line)) {
    if (line.compare(0, 10, "model name") == 0 || line.compare(0, 8, "Hardware") == 0) {
      size_t c = line.find(':');
      if (c != std::string::npos) {
        m.cpuModel = line.substr(c + 1);
        size_t b = m.cpuModel.find_first_not_of(' ');
        m.cpuModel = b == std::string::npos ? "" : m.cpuModel.substr(b);
      }
      break;
    }
  }
  std::ifstream mi("/proc/meminfo");
  while (std::getline(mi, line)) {
    if (line.compare(0, 9, "MemTotal:") == 0) {
      m.memGb = static_cast<double>(std::strtoull(line.c_str() + 9, nullptr, 10)) / (1024.0 * 1024.0);
      break;
    }
  }
  return m;
}

std::string stamp(const char* fmt) {
  std::time_t t = std::time(nullptr);
  struct std::tm tmv;
  ::localtime_r(&t, &tmv);
  char b[64];
  if (std::strftime(b, sizeof b, fmt, &tmv) == 0)
    return "?";
  return b;
}

// Everything about the machine and the device, sampled around one path's run.
struct RunEnv {
  Machine mach;
  MountInfo mnt;
  std::string startLocal, startIso;
  LoadAvg load0, load1;
  CpuCounters cpu0, cpu1;
  DiskCounters ds0, ds1;
  double wallS = 0;

  double cpuBusyPct() const {
    if (!cpu0.valid || !cpu1.valid || cpu1.total <= cpu0.total)
      return -1;
    double dt = static_cast<double>(cpu1.total - cpu0.total);
    double idle = static_cast<double>((cpu1.idle + cpu1.iowait) - (cpu0.idle + cpu0.iowait));
    return 100.0 * (dt - idle) / dt;
  }
  double iowaitPct() const {
    if (!cpu0.valid || !cpu1.valid || cpu1.total <= cpu0.total)
      return -1;
    return 100.0 * static_cast<double>(cpu1.iowait - cpu0.iowait) /
           static_cast<double>(cpu1.total - cpu0.total);
  }
  bool haveDev() const { return ds0.valid && ds1.valid; }
  double devReadMb() const { return (ds1.rdSect - ds0.rdSect) * 512.0 / 1e6; }
  double devWriteMb() const { return (ds1.wrSect - ds0.wrSect) * 512.0 / 1e6; }
  double devReads() const { return static_cast<double>(ds1.reads - ds0.reads); }
  double devWrites() const { return static_cast<double>(ds1.writes - ds0.writes); }
  double devUtilPct() const {
    if (!haveDev() || wallS <= 0)
      return -1;
    return 100.0 * static_cast<double>(ds1.ticksMs - ds0.ticksMs) / (wallS * 1000.0);
  }
};

// ---------------------------------------------------------------------------
// Measurement loops
// ---------------------------------------------------------------------------

// Latency-sampling random-IO loop: runs until deadline, records per-op µs.
// Returns ops done. `sample` may be null (count only).
uint64_t randLoop(int fd, uint64_t fsz, bool write, char* buf, double deadline,
                  std::vector<uint32_t>* sample, uint64_t seed, size_t maxSamples) {
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
    if (sample && sample->size() < maxSamples)
      sample->push_back(static_cast<uint32_t>(std::min(4.0e9, (nowS() - t0) * 1e6)));
    ++ops;
  }
  return ops;
}

// One stream-count result. `nEff` is what actually ran: a file too small to
// slice N ways is measured with fewer streams and says so rather than
// reporting a number the caller would read as N.
struct StreamStat {
  int n = 0, nEff = 0;
  double iops = 0, mbps = 0;
  Pct lat;
};

// Thread-safety: each worker opens its own fd and owns its own buffer and
// sample vector; only the totals (atomics) and the merged sample list (mutex)
// are shared.
StreamStat randReadStage(const std::string& tf, int rflags, uint64_t fsz, int n,
                         double phase) {
  StreamStat s;
  s.n = s.nEff = n;
  const size_t cap = std::max<size_t>(20000, 500000 / static_cast<size_t>(n));
  std::atomic<uint64_t> total{0};
  std::mutex mu;
  std::vector<uint32_t> us;
  double t0 = nowS(), deadline = t0 + phase;
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(n));
  for (int t = 0; t < n; ++t)
    pool.emplace_back([&, t] {
      int fd = ::open(tf.c_str(), rflags);
      char* buf = alignedBuf(kSmall);
      std::vector<uint32_t> mine;
      if (fd >= 0 && buf)
        total += randLoop(fd, fsz, false, buf, deadline, &mine, 100 + static_cast<uint64_t>(t), cap);
      if (fd >= 0)
        ::close(fd);
      ::free(buf);
      std::lock_guard<std::mutex> g(mu);
      us.insert(us.end(), mine.begin(), mine.end());
    });
  for (auto& t : pool)
    t.join();
  double el = std::max(1e-9, nowS() - t0);
  s.iops = static_cast<double>(total.load()) / el;
  s.lat = percentiles(us);
  return s;
}

// Large-block read or write at N streams. Each stream owns a slice of the
// existing test file and CYCLES INSIDE IT, so a long window on a fast device
// moves terabytes without the file — or the disk usage — growing past --size.
StreamStat bigStage(const std::string& tf, uint64_t fsz, int n, uint64_t block,
                    double phase, bool write, bool direct) {
  StreamStat s;
  s.n = n;
  int nEff = n;
  uint64_t per = 0;
  for (;;) {
    per = (fsz / static_cast<uint64_t>(nEff)) / kAlign * kAlign;
    if (per >= kAlign || nEff == 1)
      break;
    nEff /= 2;
  }
  if (per < kAlign)
    return s; // nEff stays 0: the file cannot be sliced at all
  uint64_t blk = std::min<uint64_t>(block, per) / kAlign * kAlign;
  if (blk == 0)
    blk = kAlign;
  if (blk * static_cast<uint64_t>(nEff) > kMaxStreamBufTotal) {
    uint64_t fair = (kMaxStreamBufTotal / static_cast<uint64_t>(nEff)) / kAlign * kAlign;
    blk = std::max<uint64_t>(kAlign, fair);
  }
  s.nEff = nEff;

  const int flags = (write ? O_WRONLY : O_RDONLY) | (direct ? O_DIRECT : 0);
  std::atomic<uint64_t> bytes{0};
  double t0 = nowS(), deadline = t0 + phase;
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(nEff));
  for (int t = 0; t < nEff; ++t)
    pool.emplace_back([&, t] {
      int fd = ::open(tf.c_str(), flags);
      char* buf = alignedBuf(blk);
      const uint64_t base = static_cast<uint64_t>(t) * per;
      uint64_t off = base, local = 0;
      while (fd >= 0 && buf && nowS() < deadline) {
        ssize_t r = write ? ::pwrite(fd, buf, blk, static_cast<off_t>(off))
                          : ::pread(fd, buf, blk, static_cast<off_t>(off));
        if (r != static_cast<ssize_t>(blk))
          break;
        local += blk;
        off += blk;
        if (off + blk > base + per)
          off = base; // cycle in place — never extend the file
      }
      if (fd >= 0) {
        if (write && !direct)
          ::fdatasync(fd); // buffered: bill the writeback inside the phase
        ::close(fd);
      }
      ::free(buf);
      bytes += local;
    });
  for (auto& t : pool)
    t.join();
  double el = std::max(1e-9, nowS() - t0);
  s.mbps = static_cast<double>(bytes.load()) / 1e6 / el;
  return s;
}

// ---------------------------------------------------------------------------

struct Result {
  std::string path, fs, mode, error;
  double totalGb = 0, freeGb = 0, fileMb = 0, wantMb = 0;
  bool sizeCapped = false;
  double seqWriteMbps = 0;      // sustained — the quotable figure
  double seqWriteBurstMbps = 0; // first quarter of the build window
  double seqWriteWindowS = 0;   // how long the build actually ran
  bool writeSplit = false;      // window long enough to separate the two
  double seqReadMbps = 0;
  double fsyncP50Ms = 0;
  double randwIops = 0;
  std::vector<StreamStat> randr, bigread, bigwrite;
  double mixedReadIops = 0, mixedWriteMbps = 0;
  Pct mixed;
  double createPs = 0, unlinkPs = 0;
  RunEnv env;
  bool ok = false;
};

// Which way the build window moved. FALLING is the write cache emptying into a
// slower device — the last quarter is then a real sustained rate. RISING means
// the window never got there (or allocation paced its start), so the figure is
// a ceiling and a longer write would report less. Measured on one SATA SSD:
// the SAME 64 GiB build reports FALLING 437->268 MB/s when it follows heavy
// write activity and RISING 342->452 after the device has been idle, so the
// shape is a fact about the run, not about the device alone, and the record
// has to carry it.
const char* writeShape(const Result& r) {
  double b = r.seqWriteBurstMbps, s = r.seqWriteMbps;
  double hi = std::max(b, s);
  if (hi <= 0 || std::fabs(b - s) / hi <= 0.10)
    return "flat";
  return b > s ? "FALLING" : "RISING";
}

std::string humanBlock(const Result& r) {
  std::string s;
  appendf(s, "=== ucache bench: %s (%s, %.1f GiB total, %.1f GiB free, %s) ===\n",
          r.path.c_str(), r.fs.c_str(), r.totalGb, r.freeGb, r.mode.c_str());
  if (!r.error.empty()) {
    appendf(s, "  FAILED: %s\n\n", r.error.c_str());
    return s;
  }
  if (r.sizeCapped)
    appendf(s, "  test file               %.0f MiB   (stopped at time cap; --size asked %.0f MiB)\n",
            r.fileMb, r.wantMb);
  else
    appendf(s, "  test file               %.0f MiB\n", r.fileMb);
  // Always show the shape of the build window, never just the endpoint. A bare
  // rate here is what made a write-cache burst readable as a device's sustained
  // rate, and a threshold that prints the pair only past some divergence has a
  // cliff edge: a 24% rise printed one bare number on a device sustaining half
  // of it.
  if (r.writeSplit) {
    const char* shape = writeShape(r);
    const char* gloss =
        std::strcmp(shape, "FALLING") == 0
            ? "the device's write cache absorbed the start; the last quarter is the sustained rate"
        : std::strcmp(shape, "RISING") == 0
            ? "the window never fell out of the write cache (or allocation paced the start), so "
              "this is a ceiling, not a sustained floor"
            : "steady across the window";
    appendf(s, "  sequential write        %8.1f MB/s   (build %.0f s: %.1f -> %.1f MB/s, %s — %s)\n",
            r.seqWriteMbps, r.seqWriteWindowS, r.seqWriteBurstMbps, r.seqWriteMbps, shape, gloss);
  } else {
    appendf(s, "  sequential write        %8.1f MB/s   (build %.0f s, too short to split)\n",
            r.seqWriteMbps, r.seqWriteWindowS);
  }
  appendf(s, "  fdatasync               %8.2f ms (p50)\n", r.fsyncP50Ms);
  appendf(s, "  scattered 4KiB write    %8.0f IOPS\n", r.randwIops);
  for (size_t i = 0; i < r.randr.size(); ++i) {
    const StreamStat& t = r.randr[i];
    char lbl[64];
    std::snprintf(lbl, sizeof lbl, "random 4KiB read (%d)", t.nEff);
    appendf(s, "  %-24s%8.0f IOPS   p50 %.2f ms   p95 %.2f ms   p99 %.2f ms", lbl, t.iops,
            t.lat.p50 / 1e3, t.lat.p95 / 1e3, t.lat.p99 / 1e3);
    if (i > 0 && r.randr.front().iops > 0)
      appendf(s, "   (scaling %.1fx)", t.iops / r.randr.front().iops);
    appendf(s, "\n");
  }
  for (size_t i = 0; i < r.bigread.size(); ++i) {
    const StreamStat& t = r.bigread[i];
    char lbl[64];
    std::snprintf(lbl, sizeof lbl, "large-block read (%d)", t.nEff);
    appendf(s, "  %-24s%8.1f MB/s", lbl, t.mbps);
    if (i > 0 && r.bigread.front().mbps > 0)
      appendf(s, "   (scaling %.1fx)", t.mbps / r.bigread.front().mbps);
    appendf(s, "\n");
  }
  appendf(s, "  sequential read         %8.1f MB/s\n", r.seqReadMbps);
  for (size_t i = 0; i < r.bigwrite.size(); ++i) {
    const StreamStat& t = r.bigwrite[i];
    char lbl[64];
    std::snprintf(lbl, sizeof lbl, "large-block write (%d)", t.nEff);
    appendf(s, "  %-24s%8.1f MB/s", lbl, t.mbps);
    if (i > 0 && r.bigwrite.front().mbps > 0)
      appendf(s, "   (scaling %.1fx)", t.mbps / r.bigwrite.front().mbps);
    appendf(s, "\n");
  }
  appendf(s, "  read under writeback    %8.0f IOPS   p50 %.2f ms   p99 %.2f ms   (write %.1f MB/s)\n",
          r.mixedReadIops, r.mixed.p50 / 1e3, r.mixed.p99 / 1e3, r.mixedWriteMbps);
  appendf(s, "  create / unlink         %8.0f /s   %8.0f /s\n", r.createPs, r.unlinkPs);
  return s;
}

// The context block: without it a number in a log file is unreadable a month
// later — which device answered, and how loaded was the machine.
std::string contextBlock(const Result& r, const DiskBenchOpts& o) {
  const RunEnv& e = r.env;
  std::string s;
  appendf(s, "--- run context ---\n");
  appendf(s, "  when      %s   (ucache %s, %.1f s wall)\n", e.startLocal.c_str(), UCACHE_VERSION,
          e.wallS);
  appendf(s, "  command   %s\n", o.cmdline.c_str());
  appendf(s, "  machine   %s   %s %s   %ld cpu   %.1f GiB RAM\n", e.mach.host.c_str(),
          e.mach.kernel.c_str(), e.mach.arch.c_str(), e.mach.ncpu, e.mach.memGb);
  if (!e.mach.cpuModel.empty())
    appendf(s, "  cpu       %s\n", e.mach.cpuModel.c_str());
  appendf(s, "  load      %.2f %.2f %.2f at start -> %.2f %.2f %.2f at end",
          e.load0.l1, e.load0.l5, e.load0.l15, e.load1.l1, e.load1.l5, e.load1.l15);
  if (e.cpuBusyPct() >= 0)
    appendf(s, "   (cpu %.1f%% busy, %.1f%% iowait during the run)", e.cpuBusyPct(), e.iowaitPct());
  appendf(s, "\n");
  appendf(s, "  target    %s\n", r.path.c_str());
  if (e.mnt.found) {
    appendf(s, "  mount     %s   type %s   source %s   dev %u:%u\n", e.mnt.mountPoint.c_str(),
            e.mnt.fsType.c_str(), e.mnt.source.c_str(), e.mnt.maj, e.mnt.min);
    appendf(s, "  mountopts %s%s%s\n", e.mnt.opts.c_str(), e.mnt.superOpts.empty() ? "" : " / ",
            e.mnt.superOpts.c_str());
  } else {
    appendf(s, "  mount     (not resolved)\n");
  }
  if (e.mnt.haveBlock) {
    appendf(s, "  device    %s", e.mnt.diskName.c_str());
    if (e.mnt.partName != e.mnt.diskName)
      appendf(s, " (partition %s)", e.mnt.partName.c_str());
    if (!e.mnt.model.empty())
      appendf(s, "   %s", e.mnt.model.c_str());
    if (e.mnt.rotational >= 0)
      appendf(s, "   rotational=%d", e.mnt.rotational);
    if (!e.mnt.sched.empty())
      appendf(s, "   sched=%s", e.mnt.sched.c_str());
    if (e.mnt.sizeGb > 0)
      appendf(s, "   %.1f GiB", e.mnt.sizeGb);
    appendf(s, "\n");
  } else {
    appendf(s, "  device    (none — %s is not backed by a block device)\n",
            e.mnt.found ? e.mnt.fsType.c_str() : r.fs.c_str());
  }
  if (e.haveDev())
    appendf(s, "  device IO during the run (this benchmark + everything else): "
               "read %.1f GiB in %.0f ops, wrote %.1f GiB in %.0f ops, %.0f%% util\n",
            e.devReadMb() / 1024.0, e.devReads(), e.devWriteMb() / 1024.0, e.devWrites(),
            e.devUtilPct());
  return s;
}

std::string jsonLine(const Result& r, const DiskBenchOpts& o) {
  const RunEnv& e = r.env;
  std::string s;
  appendf(s, "ucache-bench-json: {\"schema\":1,\"host\":\"%s\",\"path\":\"%s\",\"fs\":\"%s\","
             "\"mode\":\"%s\",\"total_gb\":%.1f,\"free_gb\":%.1f,\"file_mb\":%.0f,"
             "\"size_capped\":%s,\"phase_s\":%.1f,\"block_kb\":%llu,\"error\":\"%s\","
             "\"seq_write_mbps\":%.1f,\"seq_write_burst_mbps\":%.1f,"
             "\"seq_write_window_s\":%.1f,\"seq_write_shape\":\"%s\","
             "\"fsync_p50_ms\":%.2f,\"randw_iops\":%.0f",
          jesc(e.mach.host).c_str(), jesc(r.path).c_str(), jesc(r.fs).c_str(),
          jesc(r.mode).c_str(), r.totalGb, r.freeGb, r.fileMb, r.sizeCapped ? "true" : "false",
          o.phaseSeconds, static_cast<unsigned long long>(o.blockBytes / 1024),
          jesc(r.error).c_str(), r.seqWriteMbps, r.seqWriteBurstMbps, r.seqWriteWindowS,
          r.writeSplit ? writeShape(r) : "unsplit", r.fsyncP50Ms, r.randwIops);
  appendf(s, ",\"streams\":[");
  for (size_t i = 0; i < r.randr.size(); ++i)
    appendf(s, "%s%d", i ? "," : "", r.randr[i].nEff);
  appendf(s, "]");
  for (const auto& t : r.randr)
    appendf(s, ",\"randr%d_iops\":%.0f,\"randr%d_us_p50\":%llu,\"randr%d_us_p95\":%llu,"
               "\"randr%d_us_p99\":%llu",
            t.nEff, t.iops, t.nEff, static_cast<unsigned long long>(t.lat.p50), t.nEff,
            static_cast<unsigned long long>(t.lat.p95), t.nEff,
            static_cast<unsigned long long>(t.lat.p99));
  for (const auto& t : r.bigread)
    appendf(s, ",\"bigread_mbps_%d\":%.1f", t.nEff, t.mbps);
  for (const auto& t : r.bigwrite)
    appendf(s, ",\"bigwrite_mbps_%d\":%.1f", t.nEff, t.mbps);
  appendf(s, ",\"seq_read_mbps\":%.1f,\"mixed_read_iops\":%.0f,\"mixed_us_p50\":%llu,"
             "\"mixed_us_p99\":%llu,\"mixed_write_mbps\":%.1f,\"create_ps\":%.0f,"
             "\"unlink_ps\":%.0f",
          r.seqReadMbps, r.mixedReadIops, static_cast<unsigned long long>(r.mixed.p50),
          static_cast<unsigned long long>(r.mixed.p99), r.mixedWriteMbps, r.createPs, r.unlinkPs);
  // run context
  appendf(s, ",\"time\":\"%s\",\"version\":\"%s\",\"cmd\":\"%s\",\"wall_s\":%.1f,"
             "\"kernel\":\"%s\",\"arch\":\"%s\",\"ncpu\":%ld,\"mem_gb\":%.1f,\"cpu_model\":\"%s\"",
          jesc(e.startIso).c_str(), UCACHE_VERSION, jesc(o.cmdline).c_str(), e.wallS,
          jesc(e.mach.kernel).c_str(), jesc(e.mach.arch).c_str(), e.mach.ncpu, e.mach.memGb,
          jesc(e.mach.cpuModel).c_str());
  appendf(s, ",\"mount\":\"%s\",\"mount_fstype\":\"%s\",\"mount_source\":\"%s\","
             "\"mount_opts\":\"%s\",\"mount_super_opts\":\"%s\",\"dev\":\"%u:%u\"",
          jesc(e.mnt.mountPoint).c_str(), jesc(e.mnt.fsType).c_str(), jesc(e.mnt.source).c_str(),
          jesc(e.mnt.opts).c_str(), jesc(e.mnt.superOpts).c_str(), e.mnt.maj, e.mnt.min);
  if (e.mnt.haveBlock)
    appendf(s, ",\"dev_name\":\"%s\",\"dev_model\":\"%s\",\"dev_rotational\":%d,"
               "\"dev_sched\":\"%s\",\"dev_size_gb\":%.1f",
            jesc(e.mnt.diskName).c_str(), jesc(e.mnt.model).c_str(), e.mnt.rotational,
            jesc(e.mnt.sched).c_str(), e.mnt.sizeGb);
  appendf(s, ",\"load1_start\":%.2f,\"load1_end\":%.2f,\"cpu_busy_pct\":%.1f,"
             "\"cpu_iowait_pct\":%.1f",
          e.load0.l1, e.load1.l1, e.cpuBusyPct(), e.iowaitPct());
  if (e.haveDev())
    appendf(s, ",\"dev_read_mb\":%.0f,\"dev_write_mb\":%.0f,\"dev_read_ops\":%.0f,"
               "\"dev_write_ops\":%.0f,\"dev_util_pct\":%.0f",
            e.devReadMb(), e.devWriteMb(), e.devReads(), e.devWrites(), e.devUtilPct());
  appendf(s, "}\n");
  return s;
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
  r.env.mach = machineInfo();
  r.env.mnt = mountFor(path);
  r.env.startLocal = stamp("%Y-%m-%d %H:%M:%S %z");
  r.env.startIso = stamp("%Y-%m-%dT%H:%M:%S%z");
  r.env.load0 = loadAvg();
  r.env.cpu0 = cpuCounters();
  r.env.ds0 = diskCounters(r.env.mnt.diskName);
  double wall0 = nowS();
  // Every exit path below records how long the run took and what the machine
  // and device did meanwhile — a failed run's context is worth as much as a
  // successful one's.
  struct EnvClose {
    Result* r;
    double t0;
    ~EnvClose() {
      r->env.wallS = nowS() - t0;
      r->env.load1 = loadAvg();
      r->env.cpu1 = cpuCounters();
      r->env.ds1 = diskCounters(r->env.mnt.diskName);
    }
  } envClose{&r, wall0};

  struct ::statvfs vfs;
  if (::statvfs(path.c_str(), &vfs) != 0) {
    r.error = std::string("statvfs failed: ") + std::strerror(errno);
    return r;
  }
  r.totalGb = static_cast<double>(vfs.f_blocks) * vfs.f_frsize / (1ull << 30);
  r.freeGb = static_cast<double>(vfs.f_bavail) * vfs.f_frsize / (1ull << 30);
  uint64_t want = std::max<uint64_t>(o.fileBytes, kMinFile);
  r.wantMb = static_cast<double>(want) / (1 << 20);
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

  char* big = alignedBuf(o.blockBytes);
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

  // --- build the test file: sequential write, burst vs sustained -----------
  uint64_t fsz = 0;
  {
    std::vector<std::pair<double, uint64_t>> prog; // (elapsed, cumulative bytes)
    double t0 = nowS(), cap = t0 + std::max(o.phaseSeconds * 3, 10.0);
    while (fsz < want && nowS() < cap) {
      uint64_t n = std::min<uint64_t>(o.blockBytes, want - fsz);
      n = n / kAlign * kAlign;
      if (n == 0)
        break;
      if (::pwrite(wfd, big, n, static_cast<off_t>(fsz)) != static_cast<ssize_t>(n)) {
        r.error = std::string("write failed: ") + std::strerror(errno);
        ::close(wfd);
        return r;
      }
      fsz += n;
      prog.emplace_back(nowS() - t0, fsz);
    }
    double writeS = prog.empty() ? 0 : prog.back().first;
    ::fdatasync(wfd);
    double totalS = nowS() - t0; // the flush is part of the cost of the write
    double overall = totalS > 0 ? static_cast<double>(fsz) / 1e6 / totalS : 0;
    r.seqWriteMbps = overall;
    r.seqWriteBurstMbps = overall;
    r.seqWriteWindowS = totalS;
    // A device write cache makes the head of the window fast and the tail
    // honest. Split only when each quarter is long enough to mean anything.
    if (writeS >= 4 * kMinQuarterS && prog.size() >= 8) {
      auto bytesAt = [&prog](double t) -> uint64_t {
        uint64_t b = 0;
        for (const auto& p : prog) {
          if (p.first > t)
            break;
          b = p.second;
        }
        return b;
      };
      double q1 = writeS * 0.25, q3 = writeS * 0.75;
      uint64_t b1 = bytesAt(q1), b3 = bytesAt(q3);
      if (q1 > 0 && totalS > q3 && fsz > b3) {
        r.seqWriteBurstMbps = static_cast<double>(b1) / 1e6 / q1;
        r.seqWriteMbps = static_cast<double>(fsz - b3) / 1e6 / (totalS - q3);
        r.writeSplit = true;
      }
    }
    r.sizeCapped = fsz < want;
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
      if (::pwrite(bfd, small, kSmall, static_cast<off_t>((i * 977ull * kAlign) % fsz)) !=
          static_cast<ssize_t>(kSmall))
        break; // nothing dirty to sync — the fdatasync sample would be a lie
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
    uint64_t ops = randLoop(wfd, fsz, /*write=*/true, small, deadline, nullptr, 1, 0);
    if (!direct)
      ::fdatasync(wfd); // buffered: bill the writeback inside the phase
    r.randwIops = static_cast<double>(ops) / std::max(1e-9, nowS() - t0);
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

  // --- random 4 KiB reads at each stream count (hit serving; flat scaling
  // --- exposes a QoS quota) -----------------------------------------------
  for (int n : o.streams) {
    if (!direct)
      evict(rfd);
    r.randr.push_back(randReadStage(tf, rflags, fsz, n, o.phaseSeconds));
  }

  // --- large-block reads at each stream count (the replica-serving pattern
  // --- at the concurrency an analysis job actually brings) -----------------
  for (int n : o.streams) {
    if (!direct)
      evict(rfd);
    r.bigread.push_back(bigStage(tf, fsz, n, o.blockBytes, o.phaseSeconds, false, direct));
  }

  // --- sequential read (single stream, whole file) ------------------------
  {
    if (!direct)
      evict(rfd);
    double t0 = nowS(), deadline = t0 + o.phaseSeconds;
    uint64_t off = 0, bytes = 0;
    while (nowS() < deadline) {
      uint64_t n = std::min<uint64_t>(o.blockBytes, fsz - off) / kAlign * kAlign;
      if (n == 0 || ::pread(rfd, big, n, static_cast<off_t>(off)) != static_cast<ssize_t>(n))
        break;
      bytes += n;
      off += n;
      if (off + kAlign >= fsz)
        off = 0; // wrap: keep streaming for the full phase
    }
    r.seqReadMbps = static_cast<double>(bytes) / 1e6 / std::max(1e-9, nowS() - t0);
  }

  // --- large-block writes at each stream count (can the cache absorb a fast
  // --- source?) — in place, so disk usage never exceeds --size -------------
  for (int n : o.streams)
    r.bigwrite.push_back(bigStage(tf, fsz, n, o.blockBytes, o.phaseSeconds, true, direct));

  // --- random reads UNDER writeback (the production killer mode) ----------
  {
    if (!direct)
      evict(rfd);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> wbytes{0};
    std::thread writer([&] {
      int fd = ::open(tf.c_str(), O_WRONLY | (direct ? O_DIRECT : 0));
      char* buf = alignedBuf(o.blockBytes);
      uint64_t off = 0;
      while (fd >= 0 && buf && !stop.load(std::memory_order_relaxed)) {
        uint64_t n = std::min<uint64_t>(o.blockBytes, fsz - off) / kAlign * kAlign;
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
          rops += randLoop(fd, fsz, false, buf, deadline, &mine, 200 + static_cast<uint64_t>(t),
                           500000);
        if (fd >= 0)
          ::close(fd);
        ::free(buf);
        std::lock_guard<std::mutex> g(mu);
        us.insert(us.end(), mine.begin(), mine.end());
      });
    for (auto& t : readers)
      t.join();
    double elapsed = std::max(1e-9, nowS() - t0);
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
      if (::write(fd, small, kSmall) < 0) {
      } // payload is optional — this leg times create/unlink metadata
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

// The plan, printed before anything runs: `--phase-seconds` is a PER-STAGE
// window and the build stage gets 3x it, so the total is many times the number
// the caller typed. Saying so up front is cheaper than a confused campaign.
std::string planBlock(const std::vector<std::string>& paths, const DiskBenchOpts& o) {
  const double W = o.phaseSeconds;
  const size_t n = o.streams.size();
  std::string sl;
  for (size_t i = 0; i < o.streams.size(); ++i)
    appendf(sl, "%s%d", i ? "," : "", o.streams[i]);
  const double perPath = 3 * W + W + static_cast<double>(n) * W * 3 + W + W;
  std::string s;
  appendf(s, "run plan: 9 stages, %.1f s per timed window, streams %s, block %llu KiB\n", W,
          sl.c_str(), static_cast<unsigned long long>(o.blockBytes / 1024));
  appendf(s, "  build test file        up to 3 x %.1f s   (target %.1f GiB; stops at whichever "
             "comes first)\n",
          W, static_cast<double>(std::max<uint64_t>(o.fileBytes, kMinFile)) / (1ull << 30));
  appendf(s, "  fdatasync              untimed (5 samples)\n");
  appendf(s, "  scattered 4KiB write   %.1f s\n", W);
  appendf(s, "  random 4KiB read       %zu x %.1f s   (streams %s)\n", n, W, sl.c_str());
  appendf(s, "  large-block read       %zu x %.1f s   (streams %s)\n", n, W, sl.c_str());
  appendf(s, "  sequential read        %.1f s\n", W);
  appendf(s, "  large-block write      %zu x %.1f s   (streams %s, in place — the file never "
             "grows)\n",
          n, W, sl.c_str());
  appendf(s, "  read under writeback   %.1f s\n", W);
  appendf(s, "  create / unlink        untimed\n");
  const double total = perPath * static_cast<double>(paths.size());
  if (total >= 120)
    appendf(s, "  estimated total        ~%.0f s (%.0f min)", total, total / 60.0);
  else
    appendf(s, "  estimated total        ~%.0f s", total);
  if (paths.size() > 1)
    appendf(s, " for %zu paths", paths.size());
  appendf(s, ", plus untimed stages\n\n");
  return s;
}

std::string comparisonBlock(const std::vector<Result>& results) {
  std::string s;
  appendf(s, "\n=== comparison ===\n%-24s", "metric");
  for (const auto& r : results)
    appendf(s, "  %16.16s", r.path.c_str());
  appendf(s, "\n");
  auto row = [&](const char* name, const std::function<double(const Result&)>& get,
                 const char* fmt) {
    appendf(s, "%-24s", name);
    for (const auto& r : results) {
      if (r.ok)
        appendf(s, fmt, get(r));
      else
        appendf(s, "  %16s", "-");
    }
    appendf(s, "\n");
  };
  auto lastRead = [](const Result& r) { return r.bigread.empty() ? 0.0 : r.bigread.back().mbps; };
  auto lastWrite = [](const Result& r) { return r.bigwrite.empty() ? 0.0 : r.bigwrite.back().mbps; };
  auto firstRand = [](const Result& r) { return r.randr.empty() ? 0.0 : r.randr.front().iops; };
  auto lastRand = [](const Result& r) { return r.randr.empty() ? 0.0 : r.randr.back().iops; };
  auto firstLat = [](const Result& r) { return r.randr.empty() ? 0.0 : r.randr.front().lat.p50 / 1e3; };
  row("seq write MB/s", [](const Result& r) { return r.seqWriteMbps; }, "  %16.1f");
  row("seq read MB/s", [](const Result& r) { return r.seqReadMbps; }, "  %16.1f");
  row("large read MB/s (N)", lastRead, "  %16.1f");
  row("large write MB/s (N)", lastWrite, "  %16.1f");
  row("rand read IOPS (1)", firstRand, "  %16.0f");
  row("rand read IOPS (N)", lastRand, "  %16.0f");
  row("rand read p50 ms", firstLat, "  %16.2f");
  row("scattered write IOPS", [](const Result& r) { return r.randwIops; }, "  %16.0f");
  row("read-under-wb p99 ms", [](const Result& r) { return r.mixed.p99 / 1e3; }, "  %16.2f");
  row("fdatasync p50 ms", [](const Result& r) { return r.fsyncP50Ms; }, "  %16.2f");
  row("unlink /s", [](const Result& r) { return r.unlinkPs; }, "  %16.0f");
  return s;
}

// The log is the point of the tool on a fleet: one file per machine that
// accumulates every run with the context needed to read it. A log that cannot
// be written is a warning, never a failed measurement.
void appendLog(const std::string& path, const std::string& text) {
  if (path.empty())
    return;
  std::ofstream f(path, std::ios::app);
  if (!f) {
    std::fprintf(stderr, "bench: could not append to %s — results are on stdout only\n",
                 path.c_str());
    return;
  }
  f << text;
  if (!f) {
    std::fprintf(stderr, "bench: write to %s failed\n", path.c_str());
    return;
  }
  f.close();
  char abs[4096];
  const char* shown = ::realpath(path.c_str(), abs) ? abs : path.c_str();
  std::printf("appended to %s\n", shown);
}

} // namespace

int runDiskBench(const std::vector<std::string>& paths, const DiskBenchOpts& opts) {
  std::string log;
  const std::string plan = planBlock(paths, opts);
  std::fputs(plan.c_str(), stdout);
  std::fflush(stdout);
  appendf(log, "########################################################################\n");
  log += plan;

  std::vector<Result> results;
  for (const auto& p : paths) {
    Result r = benchOne(p, opts);
    std::string block = contextBlock(r, opts) + humanBlock(r) + "\n" + jsonLine(r, opts) + "\n";
    std::fputs(block.c_str(), stdout);
    std::fflush(stdout);
    log += block;
    results.push_back(std::move(r));
  }
  if (results.size() > 1) {
    std::string cmp = comparisonBlock(results);
    std::fputs(cmp.c_str(), stdout);
    log += cmp;
  }
  appendLog(opts.logPath, log);

  for (const auto& r : results)
    if (!r.ok)
      return 1;
  return 0;
}

} // namespace ucache
