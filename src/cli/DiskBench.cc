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
#ifndef UCACHE_BUILD_ID
#define UCACHE_BUILD_ID UCACHE_VERSION
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
// Datasheets quote random IOPS at queue depth 32; pin it so the number is
// comparable rather than a function of whatever --streams was passed.
constexpr int kStdQd[] = {1, 16, 32};
// The depth datasheets quote for random IOPS, and SATA's full NCQ.
constexpr int kStdWriteQd = 32;

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

// Everything else device-side in this record is a delta across the whole run,
// so it describes the run — including our own traffic — and says nothing about
// the state the run began in. loadavg is no substitute: it mixes runnable with
// IO-blocked tasks, it is a decaying average that mostly reflects what just
// FINISHED, and it is machine-wide rather than device-specific. So sample the
// device directly for a moment before touching anything.
struct PreRun {
  bool valid = false;
  double seconds = 0, readMbps = 0, writeMbps = 0, busyPct = 0;
};

PreRun sampleIdle(const std::string& disk, double seconds) {
  PreRun p;
  DiskCounters a = diskCounters(disk);
  if (!a.valid)
    return p;
  double t0 = nowS();
  std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(seconds * 1000)));
  double dt = nowS() - t0;
  DiskCounters b = diskCounters(disk);
  if (!b.valid || dt <= 0)
    return p;
  p.valid = true;
  p.seconds = dt;
  p.readMbps = (b.rdSect - a.rdSect) * 512.0 / 1e6 / dt;
  p.writeMbps = (b.wrSect - a.wrSect) * 512.0 / 1e6 / dt;
  p.busyPct = 100.0 * static_cast<double>(b.ticksMs - a.ticksMs) / (dt * 1000.0);
  return p;
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
  // Buffered writes are free until the kernel's dirty limit, so a buffered
  // measurement smaller than this measures RAM. Recorded because it differs
  // across the fleet and explains an otherwise impossible write rate.
  int dirtyRatio = -1, dirtyBgRatio = -1;
  double dirtyLimitGb = 0;
  bool dirtyAbsolute = false; // vm.dirty_bytes is set, so the ratio is ignored
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
  std::string dr = readFileTrim("/proc/sys/vm/dirty_ratio");
  std::string db = readFileTrim("/proc/sys/vm/dirty_background_ratio");
  if (!dr.empty())
    m.dirtyRatio = std::atoi(dr.c_str());
  if (!db.empty())
    m.dirtyBgRatio = std::atoi(db.c_str());
  std::string dbytes = readFileTrim("/proc/sys/vm/dirty_bytes");
  uint64_t db_abs = dbytes.empty() ? 0 : std::strtoull(dbytes.c_str(), nullptr, 10);
  m.dirtyAbsolute = db_abs != 0;
  m.dirtyLimitGb = db_abs ? static_cast<double>(db_abs) / (1ull << 30)
                          : (m.dirtyRatio > 0 ? m.memGb * m.dirtyRatio / 100.0 : 0);
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
  PreRun pre;         // device state BEFORE the run
  uint64_t ownReadBytes = 0, ownWriteBytes = 0; // what this benchmark issued
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

// The cold-fill result. Two rates, and the difference between them is the point:
// `appMbps` is what the writer saw, `devMbps` is what the disk did. With nothing
// synced until the end they diverge by however much is still dirty, so only the
// device figure is a storage measurement — a buffered leg measured 319 MB/s at
// the application against 240 at the device, a 33% overstatement.
constexpr int kFillBuckets = 8;
struct FillStat {
  double appMbps = 0, devMbps = 0, meanWriteKib = 0;
  double devWriteKib = 0; // what the DEVICE saw per write: the merging question
  uint64_t writes = 0, bytes = 0, target = 0;
  double seconds = 0, fsyncS = 0; // the closing flush, timed on its own
  // Device MB/s over each eighth of the volume, in order. A throttled fill is
  // fast until the dirty limit and slower after it, so the knee is at a byte
  // count and this is the shape that shows it; a window-based quarter split
  // cannot, because the two regimes have different durations.
  double curve[kFillBuckets] = {};
  int curveN = 0;
  bool volumeReached = false; // false => the time cap stopped it
  bool crossesDirtyLimit = false; // did the volume exceed the writeback threshold
  double dirtyLimitGb = 0;    // where the knee is predicted, for the reader
};

// One stream-count result. `nEff` is what actually ran: a file too small to
// slice N ways is measured with fewer streams and says so rather than
// reporting a number the caller would read as N.
struct StreamStat {
  int n = 0, nEff = 0;
  double iops = 0, mbps = 0;
  uint64_t bytes = 0; // exact, so "what did WE issue" is measured not inferred
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
  s.bytes = total.load() * kSmall;
  s.mbps = static_cast<double>(s.bytes) / 1e6 / el;
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
  s.bytes = bytes.load();
  s.mbps = static_cast<double>(s.bytes) / 1e6 / el;
  s.iops = static_cast<double>(s.bytes) / static_cast<double>(blk) / el;
  return s;
}

// What the ordered device-rate curve says. A fill that crosses the kernel's
// dirty limit is fast until it does and throttled afterwards, so the shape is a
// step in cumulative BYTES, not a drift in time — which is why the curve is
// bucketed by volume and why a window-based quarter split could not find it.
const char* fillShape(const FillStat& f) {
  if (f.curveN < 4)
    return "";
  double head = 0, tail = 0;
  const int h = f.curveN / 4 > 0 ? f.curveN / 4 : 1;
  for (int i = 0; i < h; ++i) {
    head += f.curve[i];
    tail += f.curve[f.curveN - 1 - i];
  }
  head /= h;
  tail /= h;
  if (head <= 0)
    return "";
  const double drop = (head - tail) / head;
  if (!f.crossesDirtyLimit)
    return drop > 0.15 ? "   <- SLOWS, but the volume never reached the dirty limit, so this is "
                         "the device, not the writeback throttle"
                       : "   (steady; volume below the dirty limit, so no throttle expected)";
  if (drop > 0.15)
    return "   <- THROTTLED: the tail is the sustained rate a long fill gets";
  return "   (steady across the dirty limit: writeback kept up)";
}

// The write pattern a cold fill actually generates. Every element is here
// because the previous shape (one writer APPENDING to a dense file, syncing and
// unlinking each one) was measured against a real cold fill on the same disk and
// overstated it by ~1.7x:
//
//   - SPARSE files, SCATTERED offsets. Each writer ftruncates its file to full
//     size and then writes its blocks in a random permutation, so every write is
//     a first touch that allocates, and no two consecutive writes are adjacent.
//     That last part is what the old shape got wrong: appending let the kernel
//     merge 48 KiB writes into ~510 KiB device writes (measured), and a cache
//     never gets that, because its writes land wherever the analysis happened to
//     read. Writing each block exactly once also makes the total exact.
//   - RETAINED. Nothing is unlinked until the stage ends, so free space falls
//     and garbage-collection pressure rises through the run. Recycling one
//     file's blocks measures a disk that never fills up.
//   - NOT SYNCED until the end. A cache does not sync either, and it is the only
//     way to reach the writeback throttle: dirty pages accumulate to the kernel's
//     limit and every writer is throttled from there on. That regime is where a
//     fill larger than the dirty limit spends most of its life, and the old
//     per-file sync bounded the dirty set at one file, so it could never be seen.
//
// BUFFERED is what a cache does; the O_DIRECT leg is the control that says what
// the page cache was worth at this shape.
//
// Thread-safety: each writer owns its file, fd, buffer and permutation; the byte
// counters are atomics, and the caller thread samples them plus the device.
FillStat fillStage(const std::string& dir, const DiskBenchOpts& o, bool direct,
                   uint64_t target, const std::string& disk, double dirtyLimitGb,
                   double timeCapS) {
  FillStat s;
  const int nw = std::max(1, o.fillWriters);
  const uint64_t blk = std::max<uint64_t>(kAlign, o.fillBlock / kAlign * kAlign);
  s.dirtyLimitGb = dirtyLimitGb;
  // Per-file size follows from the volume: writing every block of every file
  // exactly once, in random order, is what makes each write a first touch.
  uint64_t per = target / static_cast<uint64_t>(nw) / blk * blk;
  if (per < blk)
    per = blk;
  s.target = per * static_cast<uint64_t>(nw);
  s.crossesDirtyLimit = dirtyLimitGb > 0 &&
                        static_cast<double>(s.target) > dirtyLimitGb * (1ull << 30);

  std::atomic<uint64_t> bytes{0}, writes{0};
  // Counts writers still going, so the sampling loop below ends when the work
  // does. Without it a writer that cannot even open its file would leave the
  // caller spinning until the time cap with nothing being measured.
  std::atomic<int> live{nw};
  std::vector<int> fds(static_cast<size_t>(nw), -1);
  std::vector<std::string> names(static_cast<size_t>(nw));
  const double t0 = nowS(), deadline = t0 + timeCapS;
  DiskCounters d0 = diskCounters(disk);

  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(nw));
  for (int t = 0; t < nw; ++t)
    pool.emplace_back([&, t] {
      struct Done {
        std::atomic<int>* n;
        ~Done() { n->fetch_sub(1); }
      } done{&live}; // every exit path counts, including the early returns below
      char nm[4300];
      std::snprintf(nm, sizeof nm, "%s/fill%02d", dir.c_str(), t);
      names[static_cast<size_t>(t)] = nm;
      int fd = ::open(nm, O_WRONLY | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0), 0600);
      if (fd < 0)
        return;
      fds[static_cast<size_t>(t)] = fd;
      // Sparse: the size is declared, no blocks are allocated yet.
      if (::ftruncate(fd, static_cast<off_t>(per)) != 0)
        return;
      char* buf = alignedBuf(blk);
      if (!buf)
        return;
      const uint64_t slots = per / blk;
      std::vector<uint64_t> order(static_cast<size_t>(slots));
      for (uint64_t i = 0; i < slots; ++i)
        order[static_cast<size_t>(i)] = i;
      std::mt19937_64 rng(700 + static_cast<uint64_t>(t));
      for (uint64_t i = slots; i > 1; --i)
        std::swap(order[static_cast<size_t>(i - 1)],
                  order[static_cast<size_t>(rng() % i)]);
      uint64_t lb = 0, lw = 0;
      for (uint64_t i = 0; i < slots; ++i) {
        if (nowS() >= deadline)
          break;
        const off_t off = static_cast<off_t>(order[static_cast<size_t>(i)] * blk);
        if (::pwrite(fd, buf, blk, off) != static_cast<ssize_t>(blk))
          break;
        lb += blk;
        ++lw;
        // Publish often enough that the caller's bucket boundaries are sharp.
        if ((lw & 0x3f) == 0) {
          bytes += lb;
          writes += lw;
          lb = 0;
          lw = 0;
        }
      }
      bytes += lb;
      writes += lw;
      ::free(buf);
    });

  // The curve buckets on DEVICE bytes, not on the byte counter, and it covers
  // the closing flush as well as the write loop. Both parts are necessary: below
  // the dirty limit the application finishes into RAM at several GB/s and the
  // disk does nearly all of its work during the flush, so a curve bucketed on
  // application bytes fires all eight boundaries before the device has moved
  // (measured: `199 185 0 0 0 0 0` for a 2 GiB volume against a 37.4 GiB limit).
  DiskCounters bucket0 = d0;
  double bucketT0 = t0;
  int filled = 0;
  auto sample = [&] {
    const DiskCounters dn = diskCounters(disk);
    if (!dn.valid || !d0.valid)
      return;
    const uint64_t done = (dn.wrSect - d0.wrSect) * 512ull;
    while (filled < kFillBuckets &&
           done >= s.target / kFillBuckets * static_cast<uint64_t>(filled + 1)) {
      const double dt = nowS() - bucketT0;
      if (dt > 0)
        s.curve[filled] = static_cast<double>(dn.wrSect - bucket0.wrSect) * 512.0 / 1e6 / dt;
      bucket0 = dn;
      bucketT0 = nowS();
      ++filled;
    }
  };
  const struct timespec kTick { 0, 20 * 1000 * 1000 };
  while (live.load(std::memory_order_relaxed) > 0 && nowS() < deadline) {
    sample();
    ::nanosleep(&kTick, nullptr);
  }
  for (auto& th : pool)
    th.join();
  const double writeS = std::max(1e-9, nowS() - t0);

  // The closing flush, timed separately: it is a real cost of the fill, but
  // folding it into the rate would hide how much of the run was never on disk.
  // It runs on its own thread so the curve keeps being sampled through it, and
  // it is NOT deadline-bounded — abandoning it would leave dirty pages that the
  // unlink below then discards, which would silently shrink the measurement.
  const double f0 = nowS();
  std::atomic<bool> flushed{false};
  std::thread closer([&] {
    for (size_t i = 0; i < fds.size(); ++i)
      if (fds[i] >= 0)
        ::fdatasync(fds[i]);
    flushed.store(true);
  });
  while (!flushed.load(std::memory_order_relaxed)) {
    sample();
    ::nanosleep(&kTick, nullptr);
  }
  closer.join();
  s.fsyncS = nowS() - f0;
  sample();
  s.curveN = filled;
  DiskCounters d1 = diskCounters(disk);
  for (size_t i = 0; i < fds.size(); ++i)
    if (fds[i] >= 0)
      ::close(fds[i]);
  // Retained for the whole stage, removed only now.
  for (const auto& n : names)
    if (!n.empty())
      ::unlink(n.c_str());

  s.bytes = bytes.load();
  s.writes = writes.load();
  s.seconds = writeS;
  s.volumeReached = s.bytes >= s.target;
  s.appMbps = static_cast<double>(s.bytes) / 1e6 / writeS;
  s.meanWriteKib = s.writes ? static_cast<double>(s.bytes) / s.writes / 1024.0 : 0;
  const double total = writeS + s.fsyncS;
  if (d0.valid && d1.valid && total > 0) {
    const double devMb = static_cast<double>(d1.wrSect - d0.wrSect) * 512.0 / 1e6;
    const double devOps = static_cast<double>(d1.writes - d0.writes);
    s.devMbps = devMb / total;
    s.devWriteKib = devOps > 0 ? devMb * 1e6 / devOps / 1024.0 : 0;
  }
  return s;
}

// Random reads of an arbitrary block size at a given queue depth. 4 KiB is the
// datasheet number; the tiers uCache actually serves from read at ~42 KiB
// (byte cache) and ~599 KiB (replica), which sit between the standard 4 KiB
// and 4 MiB points — and that interval is where a device stops being
// op-bound and becomes bandwidth-bound, so it cannot be interpolated.
StreamStat randSizeStage(const std::string& tf, int rflags, uint64_t fsz, int qd,
                         uint64_t blk, double phase, bool write) {
  StreamStat s;
  s.n = s.nEff = qd;
  blk = std::max<uint64_t>(kAlign, blk / kAlign * kAlign);
  if (fsz <= blk)
    return s;
  const uint64_t slots = (fsz - blk) / blk;
  std::atomic<uint64_t> ops{0};
  std::mutex mu;
  std::vector<uint32_t> us;
  const size_t cap = std::max<size_t>(20000, 500000 / static_cast<size_t>(qd));
  double t0 = nowS(), deadline = t0 + phase;
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(qd));
  for (int t = 0; t < qd; ++t)
    pool.emplace_back([&, t] {
      int fd = ::open(tf.c_str(), write ? (rflags & ~O_RDONLY) | O_WRONLY : rflags);
      char* buf = alignedBuf(blk);
      std::vector<uint32_t> mine;
      std::mt19937_64 rng(900 + static_cast<uint64_t>(t));
      uint64_t n = 0;
      while (fd >= 0 && buf && nowS() < deadline) {
        uint64_t off = (rng() % slots) * blk;
        double a = nowS();
        ssize_t r = write ? ::pwrite(fd, buf, blk, static_cast<off_t>(off))
                          : ::pread(fd, buf, blk, static_cast<off_t>(off));
        if (r != static_cast<ssize_t>(blk))
          break;
        if (mine.size() < cap)
          mine.push_back(static_cast<uint32_t>(std::min(4.0e9, (nowS() - a) * 1e6)));
        ++n;
      }
      if (fd >= 0)
        ::close(fd);
      ::free(buf);
      ops += n;
      std::lock_guard<std::mutex> g(mu);
      us.insert(us.end(), mine.begin(), mine.end());
    });
  for (auto& th : pool)
    th.join();
  double el = std::max(1e-9, nowS() - t0);
  s.iops = static_cast<double>(ops.load()) / el;
  s.bytes = ops.load() * blk;
  s.mbps = static_cast<double>(s.bytes) / 1e6 / el;
  s.lat = percentiles(us);
  return s;
}

// ---------------------------------------------------------------------------

struct Result {
  std::string path, fs, mode, error;
  double totalGb = 0, freeGb = 0, fileMb = 0, wantMb = 0;
  bool sizeCapped = false;
  // Test-file creation. NOT a device spec: it pays allocation, runs cold, and
  // its window length varies with --size, which is why three runs of one
  // command on one idle SSD reported 268.5 / 451.9 / 371.9 MB/s. Kept for its
  // SHAPE and for the allocation cost; the quotable sequential write is
  // stdSeqWriteMbps.
  double buildWriteMbps = 0;
  double buildBurstMbps = 0; // first quarter of the build window
  double buildWindowS = 0;   // how long the build actually ran
  bool buildSplit = false;   // window long enough to separate the two
  double stdSeqWriteMbps = 0; // STANDARD: 1 stream, large block, O_DIRECT, in place
  uint64_t blockKib = 0;      // printed: a rate is not relatable without it
  double fsyncP50Ms = 0;
  // STANDARD — pinned block sizes and queue depths, identical on every machine
  std::vector<StreamStat> randr;     // random 4 KiB read at kStdQd[]
  StreamStat randw;                  // random 4 KiB write at QD32
  double stdSeqReadMbps = 0;         // sequential, QD1
  // PATTERN — at this machine's job concurrency
  StreamStat patSeqRead;             // sequential, QD threads
  std::vector<StreamStat> patRead;   // random reads at patReadKib[], QD threads
  std::vector<uint64_t> patReadKib;
  StreamStat patReplicaRead;         // SEQUENTIAL 512 KiB — the replica tier
  FillStat fillBuf, fillDir;         // the cold fill, both caching modes
  int fillWriters = 0;               // resolved, so the table identifies itself
  int threads = 0;                   // resolved job concurrency
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
  double b = r.buildBurstMbps, s = r.buildWriteMbps;
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
  if (r.buildSplit) {
    const char* shape = writeShape(r);
    // Report the shape, never a cause. Four runs of one command on one idle
    // SATA SSD produced FALLING, RISING, FALLING and flat, and that drive is
    // spec'd at the same rate sustained as burst — so a write-cache story does
    // not fit, and allocation and contention are at least as likely.
    const char* gloss =
        std::strcmp(shape, "FALLING") == 0
            ? "slowed through the window; the last quarter is the lower figure"
        : std::strcmp(shape, "RISING") == 0
            ? "sped up through the window, so the last quarter is a ceiling rather than a floor"
            : "steady across the window";
    appendf(s, "  test-file creation      %8.1f MB/s   (build %.0f s: %.1f -> %.1f MB/s, %s — %s)\n",
            r.buildWriteMbps, r.buildWindowS, r.buildBurstMbps, r.buildWriteMbps, shape, gloss);
  } else {
    appendf(s, "  test-file creation      %8.1f MB/s   (build %.0f s, too short to split)\n",
            r.buildWriteMbps, r.buildWindowS);
  }
  appendf(s, "  fdatasync               %8.2f ms (p50)\n", r.fsyncP50Ms);
  appendf(s, "\n  Standard measurements (same configuration on every machine)\n");
  char lb[80];
  std::snprintf(lb, sizeof lb, "sequential %llu KiB read (QD1)",
                static_cast<unsigned long long>(r.blockKib));
  appendf(s, "    %-46s%8.1f MB/s\n", lb, r.stdSeqReadMbps);
  std::snprintf(lb, sizeof lb, "sequential %llu KiB write (QD1, in place)",
                static_cast<unsigned long long>(r.blockKib));
  appendf(s, "    %-46s%8.1f MB/s\n", lb, r.stdSeqWriteMbps);
  for (size_t i = 0; i < r.randr.size(); ++i) {
    const StreamStat& t = r.randr[i];
    std::snprintf(lb, sizeof lb, "random 4 KiB read (QD%d)", t.nEff);
    appendf(s, "    %-46s%8.0f IOPS   p50 %.2f ms   p95 %.2f ms   p99 %.2f ms", lb, t.iops,
            t.lat.p50 / 1e3, t.lat.p95 / 1e3, t.lat.p99 / 1e3);
    if (i > 0 && r.randr.front().iops > 0)
      appendf(s, "   (%.1fx QD1)", t.iops / r.randr.front().iops);
    appendf(s, "\n");
  }
  std::snprintf(lb, sizeof lb, "random 4 KiB write (QD%d)", kStdWriteQd);
  appendf(s, "    %-46s%8.0f IOPS   p50 %.2f ms   p99 %.2f ms\n", lb, r.randw.iops,
          r.randw.lat.p50 / 1e3, r.randw.lat.p99 / 1e3);

  if (r.threads > 0)
    appendf(s, "\n  Pattern measurements (job concurrency %d threads)\n", r.threads);
  else
    appendf(s, "\n  Pattern measurements (concurrency-dependent ones SKIPPED — no --threads)\n");
  if (r.threads > 0) {
    std::snprintf(lb, sizeof lb, "sequential %llu KiB read (QD%d)",
                  static_cast<unsigned long long>(r.blockKib), r.patSeqRead.nEff);
    appendf(s, "    %-46s%8.1f MB/s", lb, r.patSeqRead.mbps);
    if (r.stdSeqReadMbps > 0)
      appendf(s, "   (%.2fx QD1)", r.patSeqRead.mbps / r.stdSeqReadMbps);
    appendf(s, "\n");
  }
  for (size_t i = 0; i < r.patRead.size(); ++i) {
    const StreamStat& t = r.patRead[i];
    const uint64_t kib = i < r.patReadKib.size() ? r.patReadKib[i] : 0;
    const char* tier = kib == 48 ? "  [byte tier]" : kib == 512 ? "  [replica tier]" : "";
    std::snprintf(lb, sizeof lb, "random %llu KiB read (QD%d)%s",
                  static_cast<unsigned long long>(kib), t.nEff, tier);
    appendf(s, "    %-46s%8.1f MB/s   %8.0f IOPS   p99 %.2f ms\n", lb, t.mbps, t.iops,
            t.lat.p99 / 1e3);
  }
  if (r.threads > 0) {
    std::snprintf(lb, sizeof lb, "sequential 512 KiB read (QD%d)  [replica tier]",
                  r.patReplicaRead.nEff);
    appendf(s, "    %-46s%8.1f MB/s   %8.0f IOPS\n", lb, r.patReplicaRead.mbps,
            r.patReplicaRead.iops);
  }
  // The fill is reported DEVICE-side. With nothing synced until the end, the
  // application is ahead of the disk by whatever is still dirty, so the
  // application rate is shown only as the size of that gap.
  auto fillLine = [&s](const char* label, const FillStat& f, int writers) {
    char lb2[80];
    std::snprintf(lb2, sizeof lb2, "%s (%d writer%s, sparse, retained)", label, writers,
                  writers == 1 ? "" : "s");
    appendf(s, "    %-46s%8.1f MB/s   %.1f KiB device writes\n", lb2, f.devMbps, f.devWriteKib);
    // A quick check writes megabytes and the convention run writes tens of
    // gigabytes; one fixed unit renders one of them as 0.0.
    const bool gib = f.target >= (1ull << 30);
    const double div = static_cast<double>(gib ? (1ull << 30) : (1ull << 20));
    appendf(s, "      wrote %.1f of %.1f %s in %.0f s = %.0f s writing + %.0f s closing "
               "flush%s\n",
            static_cast<double>(f.bytes) / div, static_cast<double>(f.target) / div,
            gib ? "GiB" : "MiB", f.seconds + f.fsyncS, f.seconds, f.fsyncS,
            f.volumeReached ? "" : " — STOPPED AT TIME CAP");
    // The gap between the two rates is how far ahead of the disk the writer got,
    // i.e. how much of this was RAM. It is the reason the device figure is the
    // one reported: a buffered leg once read 319 MB/s at the application against
    // 240 at the device.
    appendf(s, "      application saw %.1f MB/s, %+.0f%% vs the device%s\n", f.appMbps,
            f.devMbps > 0 ? 100.0 * (f.appMbps - f.devMbps) / f.devMbps : 0.0,
            f.appMbps > 2 * f.devMbps ? "  <- mostly RAM: the writer never waited for the disk"
                                      : "");
    if (f.curveN >= 2) {
      appendf(s, "      by eighth of volume:");
      for (int i = 0; i < f.curveN; ++i)
        appendf(s, " %.0f", f.curve[i]);
      appendf(s, " MB/s%s\n", fillShape(f));
    }
    if (f.dirtyLimitGb > 0)
      appendf(s, "      writeback threshold at %.1f GiB — %s\n", f.dirtyLimitGb,
              f.crossesDirtyLimit ? "crossed inside this run, so the tail is the throttled rate"
                                  : "NOT reached: this run cannot show the throttle");
  };
  fillLine("cold fill, buffered", r.fillBuf, r.fillWriters);
  fillLine("cold fill, O_DIRECT", r.fillDir, r.fillWriters);
  appendf(s, "    %-46s%8.0f IOPS   p50 %.2f ms   p99 %.2f ms   (write %.1f MB/s)\n",
          "random 4 KiB read under writeback (QD4)", r.mixedReadIops, r.mixed.p50 / 1e3, r.mixed.p99 / 1e3, r.mixedWriteMbps);
  appendf(s, "\n  Untimed\n    %-46s%8.0f /s   %8.0f /s\n", "create / unlink", r.createPs,
          r.unlinkPs);
  return s;
}

// The context block: without it a number in a log file is unreadable a month
// later — which device answered, and how loaded was the machine.
std::string contextBlock(const Result& r, const DiskBenchOpts& o) {
  const RunEnv& e = r.env;
  std::string s;
  appendf(s, "--- run context ---\n");
  appendf(s, "  when      %s   (ucache %s, build %s, %.1f s wall)\n", e.startLocal.c_str(),
          UCACHE_VERSION, UCACHE_BUILD_ID, e.wallS);
  appendf(s, "  command   %s\n", o.cmdline.c_str());
  appendf(s, "  machine   %s   %s %s   %ld cpu   %.1f GiB RAM\n", e.mach.host.c_str(),
          e.mach.kernel.c_str(), e.mach.arch.c_str(), e.mach.ncpu, e.mach.memGb);
  if (!e.mach.cpuModel.empty())
    appendf(s, "  cpu       %s\n", e.mach.cpuModel.c_str());
  // Say WHICH knob governs. The kernel reports dirty_ratio as 0 when
  // vm.dirty_bytes is set, so printing both flat reads as a contradiction: a
  // 0% ratio beside a real limit.
  if (e.mach.dirtyAbsolute)
    appendf(s, "  writeback vm.dirty_bytes is set (the ratio is ignored)  -> buffered writes "
               "are free up to ~%.1f GiB\n",
            e.mach.dirtyLimitGb);
  else if (e.mach.dirtyRatio >= 0)
    appendf(s, "  writeback dirty_ratio %d%% of RAM / background %d%%  -> buffered writes are "
               "free up to ~%.1f GiB\n",
            e.mach.dirtyRatio, e.mach.dirtyBgRatio, e.mach.dirtyLimitGb);
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
  if (e.pre.valid)
    appendf(s, "  device BEFORE the run (%.1f s sample): read %.1f MB/s, wrote %.1f MB/s, "
               "busy %.0f%%%s\n",
            e.pre.seconds, e.pre.readMbps, e.pre.writeMbps, e.pre.busyPct,
            e.pre.busyPct < 5 ? "   (idle)" : "   <- NOT IDLE: another workload is on this device");
  if (e.haveDev())
    appendf(s, "  device IO during the run (this benchmark + everything else): "
               "read %.1f GiB in %.0f ops, wrote %.1f GiB in %.0f ops, %.0f%% util\n",
            e.devReadMb() / 1024.0, e.devReads(), e.devWriteMb() / 1024.0, e.devWrites(),
            e.devUtilPct());
  if (e.haveDev() && (e.ownReadBytes || e.ownWriteBytes)) {
    const double orb = static_cast<double>(e.ownReadBytes) / 1e6;
    const double owb = static_cast<double>(e.ownWriteBytes) / 1e6;
    appendf(s, "    of which this benchmark issued: read %.1f GiB, wrote %.1f GiB\n",
            orb / 1024.0, owb / 1024.0);
    // A RESIDUAL, not a measurement, and an upper bound on interference: our
    // own filesystem metadata (journal, extent maps, ~200 create/unlink pairs)
    // and any traffic on OTHER partitions of the same disk land here too. Shown
    // in MB with a share, because at GiB-with-one-decimal the metadata we would
    // want to notice rounds to nothing.
    const double ur = e.devReadMb() - orb, uw = e.devWriteMb() - owb;
    appendf(s, "    unattributed remainder:         read %.0f MB (%.1f%%), wrote %.0f MB (%.1f%%)"
               "   [our metadata + any other user of this disk]\n",
            ur, e.devReadMb() > 0 ? 100.0 * ur / e.devReadMb() : 0.0, uw,
            e.devWriteMb() > 0 ? 100.0 * uw / e.devWriteMb() : 0.0);
  }
  return s;
}

std::string jsonLine(const Result& r, const DiskBenchOpts& o) {
  const RunEnv& e = r.env;
  std::string s;
  appendf(s, "ucache-bench-json: {\"schema\":1,\"host\":\"%s\",\"path\":\"%s\",\"fs\":\"%s\","
             "\"mode\":\"%s\",\"total_gb\":%.1f,\"free_gb\":%.1f,\"file_mb\":%.0f,"
             "\"size_capped\":%s,\"measurement_s\":%.1f,\"block_kb\":%llu,\"error\":\"%s\","
             "\"build_write_mbps\":%.1f,\"build_write_burst_mbps\":%.1f,"
             "\"build_write_window_s\":%.1f,\"build_write_shape\":\"%s\","
             "\"fsync_p50_ms\":%.2f",
          jesc(e.mach.host).c_str(), jesc(r.path).c_str(), jesc(r.fs).c_str(),
          jesc(r.mode).c_str(), r.totalGb, r.freeGb, r.fileMb, r.sizeCapped ? "true" : "false",
          o.measurementSeconds, static_cast<unsigned long long>(o.blockBytes / 1024),
          jesc(r.error).c_str(), r.buildWriteMbps, r.buildBurstMbps, r.buildWindowS,
          r.buildSplit ? writeShape(r) : "unsplit", r.fsyncP50Ms);
  // STANDARD measures, at pinned block sizes and queue depths so they can be
  // read against a datasheet. seq_write_mbps keeps its name and now carries
  // the in-place single-stream figure: the build-stage number it used to hold
  // moved 68% across three runs of one command and is in build_write_mbps.
  appendf(s, ",\"seq_write_mbps\":%.1f,\"std_block_kib\":%llu,\"std_qd\":%d,"
             "\"randw_iops\":%.0f,\"randw_us_p50\":%llu,\"randw_us_p99\":%llu",
          r.stdSeqWriteMbps, static_cast<unsigned long long>(r.blockKib), kStdWriteQd,
          r.randw.iops,
          static_cast<unsigned long long>(r.randw.lat.p50),
          static_cast<unsigned long long>(r.randw.lat.p99));
  // PREDICTIVE: tier-sized random reads at job concurrency, and the fill.
  appendf(s, ",\"threads\":%d", r.threads);
  if (r.threads > 0)
    appendf(s, ",\"pat_seq_read_mbps\":%.1f,\"pat_replica_read_mbps\":%.1f,"
               "\"pat_replica_read_iops\":%.0f",
            r.patSeqRead.mbps, r.patReplicaRead.mbps, r.patReplicaRead.iops);
  for (size_t i = 0; i < r.patRead.size(); ++i)
  {
    const unsigned long long kib =
        static_cast<unsigned long long>(i < r.patReadKib.size() ? r.patReadKib[i] : 0);
    appendf(s, ",\"pat_rand%lluk_read_mbps\":%.1f,\"pat_rand%lluk_read_iops\":%.0f,"
               "\"pat_rand%lluk_read_us_p99\":%llu",
            kib, r.patRead[i].mbps, kib, r.patRead[i].iops, kib,
            static_cast<unsigned long long>(r.patRead[i].lat.p99));
  }
  // The cold fill. New key names because this is NOT the quantity the old
  // fill_* keys held: those measured one writer APPENDING to a dense file and
  // syncing every 256 MiB, which merged into ~510 KiB device writes and never
  // reached the writeback throttle. Reusing the names would have silently
  // compared two different measurements across the fleet's records.
  appendf(s, ",\"fill_writers\":%d,\"fill_block_kib\":%llu,\"fill_volume_mib\":%.0f",
          r.fillWriters, static_cast<unsigned long long>(o.fillBlock / 1024),
          static_cast<double>(r.fillBuf.target) / (1ull << 20));
  auto fillJson = [&s](const char* tag, const FillStat& f) {
    appendf(s, ",\"coldfill_%s_dev_mbps\":%.1f,\"coldfill_%s_app_mbps\":%.1f,"
               "\"coldfill_%s_dev_write_kib\":%.1f,\"coldfill_%s_app_write_kib\":%.1f,"
               "\"coldfill_%s_mib\":%.0f,\"coldfill_%s_seconds\":%.1f,"
               "\"coldfill_%s_fsync_s\":%.1f,\"coldfill_%s_volume_reached\":%s,"
               "\"coldfill_%s_crosses_dirty_limit\":%s,\"coldfill_%s_curve\":[",
            tag, f.devMbps, tag, f.appMbps, tag, f.devWriteKib, tag, f.meanWriteKib, tag,
            static_cast<double>(f.bytes) / (1ull << 20), tag, f.seconds, tag, f.fsyncS, tag,
            f.volumeReached ? "true" : "false", tag,
            f.crossesDirtyLimit ? "true" : "false", tag);
    for (int i = 0; i < f.curveN; ++i)
      appendf(s, "%s%.1f", i ? "," : "", f.curve[i]);
    appendf(s, "]");
  };
  fillJson("buffered", r.fillBuf);
  fillJson("direct", r.fillDir);
  appendf(s, ",\"std_qds\":[");
  for (size_t i = 0; i < r.randr.size(); ++i)
    appendf(s, "%s%d", i ? "," : "", r.randr[i].nEff);
  appendf(s, "]");
  for (const auto& t : r.randr)
    appendf(s, ",\"randr%d_iops\":%.0f,\"randr%d_us_p50\":%llu,\"randr%d_us_p95\":%llu,"
               "\"randr%d_us_p99\":%llu",
            t.nEff, t.iops, t.nEff, static_cast<unsigned long long>(t.lat.p50), t.nEff,
            static_cast<unsigned long long>(t.lat.p95), t.nEff,
            static_cast<unsigned long long>(t.lat.p99));

  appendf(s, ",\"seq_read_mbps\":%.1f,\"mixed_read_iops\":%.0f,\"mixed_us_p50\":%llu,"
             "\"mixed_us_p99\":%llu,\"mixed_write_mbps\":%.1f,\"create_ps\":%.0f,"
             "\"unlink_ps\":%.0f",
          r.stdSeqReadMbps, r.mixedReadIops, static_cast<unsigned long long>(r.mixed.p50),
          static_cast<unsigned long long>(r.mixed.p99), r.mixedWriteMbps, r.createPs, r.unlinkPs);
  // run context
  appendf(s, ",\"time\":\"%s\",\"version\":\"%s\",\"build_id\":\"" UCACHE_BUILD_ID "\",\"cmd\":\"%s\",\"wall_s\":%.1f,"
             "\"kernel\":\"%s\",\"arch\":\"%s\",\"ncpu\":%ld,\"mem_gb\":%.1f,\"cpu_model\":\"%s\","
             "\"dirty_ratio\":%d,\"dirty_background_ratio\":%d,\"dirty_limit_gb\":%.1f,"
             "\"dirty_absolute\":%s",
          jesc(e.startIso).c_str(), UCACHE_VERSION, jesc(o.cmdline).c_str(), e.wallS,
          jesc(e.mach.kernel).c_str(), jesc(e.mach.arch).c_str(), e.mach.ncpu, e.mach.memGb,
          jesc(e.mach.cpuModel).c_str(), e.mach.dirtyRatio, e.mach.dirtyBgRatio,
          e.mach.dirtyLimitGb, e.mach.dirtyAbsolute ? "true" : "false");
  appendf(s, ",\"mount\":\"%s\",\"mount_fstype\":\"%s\",\"mount_source\":\"%s\","
             "\"mount_opts\":\"%s\",\"mount_super_opts\":\"%s\",\"dev\":\"%u:%u\"",
          jesc(e.mnt.mountPoint).c_str(), jesc(e.mnt.fsType).c_str(), jesc(e.mnt.source).c_str(),
          jesc(e.mnt.opts).c_str(), jesc(e.mnt.superOpts).c_str(), e.mnt.maj, e.mnt.min);
  if (e.mnt.haveBlock)
    appendf(s, ",\"dev_name\":\"%s\",\"dev_model\":\"%s\",\"dev_rotational\":%d,"
               "\"dev_sched\":\"%s\",\"dev_size_gb\":%.1f",
            jesc(e.mnt.diskName).c_str(), jesc(e.mnt.model).c_str(), e.mnt.rotational,
            jesc(e.mnt.sched).c_str(), e.mnt.sizeGb);
  if (e.pre.valid)
    appendf(s, ",\"pre_read_mbps\":%.1f,\"pre_write_mbps\":%.1f,\"pre_busy_pct\":%.0f,"
               "\"pre_sample_s\":%.1f",
            e.pre.readMbps, e.pre.writeMbps, e.pre.busyPct, e.pre.seconds);
  appendf(s, ",\"load1_start\":%.2f,\"load1_end\":%.2f,\"cpu_busy_pct\":%.1f,"
             "\"cpu_iowait_pct\":%.1f",
          e.load0.l1, e.load1.l1, e.cpuBusyPct(), e.iowaitPct());
  if (e.haveDev())
    appendf(s, ",\"own_read_mb\":%.0f,\"own_write_mb\":%.0f",
            static_cast<double>(e.ownReadBytes) / 1e6, static_cast<double>(e.ownWriteBytes) / 1e6);
  if (e.haveDev())
    appendf(s, ",\"dev_read_mb\":%.0f,\"dev_write_mb\":%.0f,\"dev_read_ops\":%.0f,"
               "\"dev_write_ops\":%.0f,\"dev_util_pct\":%.0f",
            e.devReadMb(), e.devWriteMb(), e.devReads(), e.devWrites(), e.devUtilPct());
  appendf(s, "}\n");
  return s;
}

// How much the cold fill writes. The writeback knee is at a byte count — the
// kernel's dirty limit — so the volume must exceed it with enough left over to
// measure the throttled rate: 1.5x. The floor keeps the stage meaningful where
// the limit is tiny (one host has vm.dirty_bytes set to 0.10 GiB), and the clamp
// keeps it from filling the disk. Both are visible in the output rather than
// silently applied: the record states the volume, whether it was reached, and
// whether it crossed the limit at all.
uint64_t fillVolume(const DiskBenchOpts& o, double dirtyLimitGb, uint64_t testFileBytes) {
  if (o.fillVolume) // asked for explicitly: honoured, and the space is the caller's problem
    return std::max<uint64_t>(kAlign, o.fillVolume / kAlign * kAlign);
  constexpr uint64_t kFloor = 4ull << 30;
  uint64_t want = dirtyLimitGb > 0
                      ? static_cast<uint64_t>(dirtyLimitGb * 1.5 * static_cast<double>(1ull << 30))
                      : kFloor;
  if (want < kFloor)
    want = kFloor;
  // --size is the caller's space budget for the whole run. The test file is
  // removed before the fill starts (nothing reads it after the writeback mix),
  // so peak usage is the LARGER of the two rather than their sum — and capping
  // the default volume here keeps that promise on a small --size, where the
  // alternative was a quick 32 MiB check quietly writing 4 GiB.
  //
  // Cap on what was ASKED, not on what the build achieved. Capping on the
  // achieved size made the fill volume depend on whether an unrelated stage hit
  // its time cap: two disks measured back to back got 56.1 and 40.3 GiB, which
  // are different measurements of different things.
  if (want > testFileBytes)
    want = testFileBytes;
  return std::max<uint64_t>(kAlign, want / kAlign * kAlign);
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
  r.blockKib = o.blockBytes / 1024;
  r.threads = o.threads; // 0 = not given; never probed (see planBlock)
  r.env.mach = machineInfo();
  r.env.mnt = mountFor(path);
  r.env.startLocal = stamp("%Y-%m-%d %H:%M:%S %z");
  r.env.startIso = stamp("%Y-%m-%dT%H:%M:%S%z");
  r.env.load0 = loadAvg();
  r.env.cpu0 = cpuCounters();
  r.env.pre = sampleIdle(r.env.mnt.diskName, 2.0);
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
    double t0 = nowS(), cap = t0 + std::max(o.measurementSeconds * 3, 10.0);
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
    r.buildWriteMbps = overall;
    r.buildBurstMbps = overall;
    r.buildWindowS = totalS;
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
        r.buildBurstMbps = static_cast<double>(b1) / 1e6 / q1;
        r.buildWriteMbps = static_cast<double>(fsz - b3) / 1e6 / (totalS - q3);
        r.buildSplit = true;
      }
    }
    r.sizeCapped = fsz < want;
    r.env.ownWriteBytes += fsz;
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

  ::close(wfd);

  int rflags = O_RDONLY | (direct ? O_DIRECT : 0);
  int rfd = ::open(tf.c_str(), rflags);
  if (rfd < 0) {
    r.error = std::string("reopen failed: ") + std::strerror(errno);
    return r;
  }
  if (!direct)
    evict(rfd);

  // ===== STANDARD measurements: pinned block sizes and queue depths, so the
  // ===== numbers are identical in configuration on every machine and can be
  // ===== read against a datasheet. QD1 is the latency reference, QD16 is what
  // ===== the documented device grades are defined at, QD32 is the depth
  // ===== datasheets quote (and SATA's full NCQ).
  for (int qd : kStdQd) {
    if (!direct)
      evict(rfd);
    r.randr.push_back(randReadStage(tf, rflags, fsz, qd, o.measurementSeconds));
    r.env.ownReadBytes += r.randr.back().bytes;
  }
  {
    if (!direct)
      evict(rfd);
    StreamStat sr = bigStage(tf, fsz, 1, o.blockBytes, o.measurementSeconds, false, direct);
    r.stdSeqReadMbps = sr.mbps;
    r.env.ownReadBytes += sr.bytes;
  }
  // Sequential write, in place: the reproducible one (~6% across four runs of
  // one command, 96% of this drive's spec), unlike creating the test file.
  {
    StreamStat sw = bigStage(tf, fsz, 1, o.blockBytes, o.measurementSeconds, true, direct);
    r.stdSeqWriteMbps = sw.mbps;
    r.env.ownWriteBytes += sw.bytes;
  }

  // ===== PATTERN measurements: the shapes uCache generates. Reads are issued
  // ===== per analysis thread (unlike fills, which are single-stream), so the
  // ===== three read measurements need a job concurrency — and it is NOT
  // ===== probed. A number nobody chose, whether inherited from a queue-depth
  // ===== list or read off nproc, produces a "job concurrency" figure that is
  // ===== wrong wherever the job does not use every core. Without --threads
  // ===== they are skipped; the fill and the writeback mix still run, because
  // ===== their configuration is fully determined without it.
  if (r.threads > 0) {
    if (!direct)
      evict(rfd);
    r.patSeqRead = bigStage(tf, fsz, r.threads, o.blockBytes, o.measurementSeconds, false, direct);
    r.env.ownReadBytes += r.patSeqRead.bytes;
    // 48 KiB is what the byte tier serves at, 512 KiB what a replica serves at.
    // Both sit between the standard 4 KiB and 4 MiB points, in the interval
    // where a device stops being op-bound and turns bandwidth-bound.
    // Byte tier: scattered (measured 733 KiB mean between consecutive writes,
    // and reads follow the same basket layout) -> random.
    // Replica tier: branch-major, so consecutive reads land adjacent, which is
    // why it reaches ~599 KiB/op -> SEQUENTIAL, each thread in its own region.
    static const uint64_t kTiers[] = {48ull << 10};
    static const uint64_t kLadder[] = {4ull << 10,  8ull << 10,   16ull << 10,  32ull << 10,
                                       48ull << 10, 128ull << 10, 512ull << 10, 4096ull << 10};
    const uint64_t* sizes = o.sweep ? kLadder : kTiers;
    const size_t nsizes = o.sweep ? sizeof kLadder / sizeof kLadder[0] : 1;
    for (size_t i = 0; i < nsizes; ++i) {
      if (!direct)
        evict(rfd);
      r.patRead.push_back(
          randSizeStage(tf, rflags, fsz, r.threads, sizes[i], o.measurementSeconds, false));
      r.env.ownReadBytes += r.patRead.back().bytes;
      r.patReadKib.push_back(sizes[i] / 1024);
    }
    if (!direct)
      evict(rfd);
    r.patReplicaRead =
        bigStage(tf, fsz, r.threads, 512ull << 10, o.measurementSeconds, false, direct);
    r.env.ownReadBytes += r.patReplicaRead.bytes;
  }

  // --- STANDARD random write, 4 KiB at queue depth 32 (the datasheet number;
  // --- the shipped scattered-write stage is single-threaded, so the tool had
  // --- QD1 only while every spec sheet quotes QD32).
  r.randw = randSizeStage(tf, rflags, fsz, kStdWriteQd, kSmall, o.measurementSeconds, true);
  r.env.ownWriteBytes += r.randw.bytes;

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
    double t0 = nowS(), deadline = t0 + o.measurementSeconds;
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
    r.env.ownReadBytes += rops.load() * kSmall;
    r.env.ownWriteBytes += wbytes.load();
    r.mixedReadIops = static_cast<double>(rops.load()) / elapsed;
    r.mixed = percentiles(us);
    r.mixedWriteMbps = static_cast<double>(wbytes.load()) / 1e6 / elapsed;
  }
  ::close(rfd);

  // --- COLD FILL (both caching modes). Buffered is what a cache does; the
  // --- O_DIRECT leg says what the page cache was worth at this shape.
  // --- Bounded by volume, not by a window, because the writeback knee is at a
  // --- byte count. The legs run one after the other and each removes its files
  // --- at the end, so peak usage is one leg's volume, not both.
  // Nothing reads the test file after the writeback mix, and the fill needs the
  // space more: removing it here means peak usage is max(test file, fill volume)
  // rather than their sum, which is what lets --size stay the whole run's budget.
  ::unlink(tf.c_str());
  {
    const uint64_t vol =
        fillVolume(o, r.env.mach.dirtyLimitGb, std::max<uint64_t>(o.fileBytes, kMinFile));
    const double cap = std::max(o.measurementSeconds * 6, 120.0);
    r.fillWriters = std::max(1, o.fillWriters);
    r.fillBuf = fillStage(dir, o, /*direct=*/false, vol, r.env.mnt.diskName,
                          r.env.mach.dirtyLimitGb, cap);
    r.fillDir = fillStage(dir, o, /*direct=*/true, vol, r.env.mnt.diskName,
                          r.env.mach.dirtyLimitGb, cap);
    r.env.ownWriteBytes += r.fillBuf.bytes + r.fillDir.bytes;
  }

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
  const double W = o.measurementSeconds;
  const int nStd = static_cast<int>(sizeof kStdQd / sizeof kStdQd[0]);
  const bool pat = o.threads > 0;
  // Pattern, non-sweep: sequential at job concurrency, the byte tier, the
  // replica tier. With --sweep the single byte-tier point becomes the 8-point
  // ladder, and the sequential and replica measurements still run either way —
  // so it is 10, not 9, and an estimate short by one whole window is exactly
  // the arithmetic the plan exists to remove.
  // The two cold-fill legs are NOT window-bounded — they run until their volume
  // is written — so they are counted separately, at their time cap, or the
  // estimate would understate a slow device by minutes.
  const int measurements = 2 + nStd + 1                      // standard
                           + (pat ? (o.sweep ? 10 : 3) : 0) + 1; // pattern, fills excluded
  const double buildCap = std::max(W * 3, 10.0);
  const double fillCap = std::max(W * 6, 120.0);
  const double total =
      (buildCap + W * measurements + 2 * fillCap) * static_cast<double>(paths.size());
  std::string s;
  appendf(s, "run plan: %.1f s per measurement, %d measurements + 2 cold fills  ->  up to %.0f s",
          W, measurements,
          total);
  if (total >= 120)
    appendf(s, " (%.0f min)", total / 60.0);
  if (paths.size() > 1)
    appendf(s, " for %zu paths", paths.size());
  appendf(s, "\n");
  appendf(s, "  test file: build until %.1f GiB or %.0f s, whichever comes first\n",
          static_cast<double>(std::max<uint64_t>(o.fileBytes, kMinFile)) / (1ull << 30), buildCap);
  appendf(s, "  threads: %d\n", o.threads);
  appendf(s, "\n  Standard measurements\n");
  appendf(s, "    sequential %llu KiB read, QD1\n",
          static_cast<unsigned long long>(o.blockBytes / 1024));
  appendf(s, "    sequential %llu KiB write, QD1, in place\n",
          static_cast<unsigned long long>(o.blockBytes / 1024));
  for (int qd : kStdQd)
    appendf(s, "    random 4 KiB read, QD%d\n", qd);
  appendf(s, "    random 4 KiB write, QD%d\n", kStdWriteQd);
  appendf(s, "\n  Pattern measurements\n");
  if (pat) {
    appendf(s, "    sequential %llu KiB read, QD%d\n",
            static_cast<unsigned long long>(o.blockBytes / 1024), o.threads);
    appendf(s, "    sequential 512 KiB read, QD%d     (replica tier — branch-major, so\n"
               "                                      consecutive reads are adjacent)\n",
            o.threads);
    if (o.sweep)
      appendf(s, "    random read block sweep, QD%d     (4,8,16,32,48,128,512,4096 KiB —\n"
                 "                                      locates the op-bound/bandwidth knee)\n",
              o.threads);
    else {
      appendf(s, "    random 48 KiB read, QD%d          (byte tier — scattered)\n", o.threads);
    }
  }
  appendf(s, "    cold fill, buffered              (%d writer(s), %llu KiB writes into SPARSE\n"
             "                                      files at scattered offsets, RETAINED, not\n"
             "                                      synced until the end)\n",
          o.fillWriters, static_cast<unsigned long long>(o.fillBlock / 1024));
  appendf(s, "    cold fill, O_DIRECT              (same shape, no page cache)\n");
  appendf(s, "    random 4 KiB read under writeback, QD4\n");
  appendf(s, "\n  Untimed\n    fdatasync (5 samples), create / unlink\n\n");
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
  auto lastRead = [](const Result& r) { return r.patSeqRead.mbps; };
  auto lastWrite = [](const Result& r) { return r.stdSeqWriteMbps; };
  auto firstRand = [](const Result& r) { return r.randr.empty() ? 0.0 : r.randr.front().iops; };
  auto lastRand = [](const Result& r) { return r.randr.empty() ? 0.0 : r.randr.back().iops; };
  auto firstLat = [](const Result& r) { return r.randr.empty() ? 0.0 : r.randr.front().lat.p50 / 1e3; };
  row("test-file create MB/s", [](const Result& r) { return r.buildWriteMbps; }, "  %16.1f");
  row("seq read MB/s (QD1)", [](const Result& r) { return r.stdSeqReadMbps; }, "  %16.1f");
  row("seq read MB/s (threads)", lastRead, "  %16.1f");
  row("seq write MB/s (QD1)", lastWrite, "  %16.1f");
  row("cold fill MB/s (dev)", [](const Result& r) { return r.fillBuf.devMbps; }, "  %16.1f");
  row("rand write IOPS (QD32)", [](const Result& r) { return r.randw.iops; }, "  %16.0f");
  row("rand read IOPS (1)", firstRand, "  %16.0f");
  row("rand read IOPS (N)", lastRand, "  %16.0f");
  row("rand read p50 ms", firstLat, "  %16.2f");
  row("rand write IOPS (QD32)", [](const Result& r) { return r.randw.iops; }, "  %16.0f");
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
