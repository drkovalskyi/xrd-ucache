#include "CpuCounters.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>

#if defined(__linux__)
#include <cstring>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif

namespace ucache {

#if defined(__linux__)
namespace {
// `inherit` follows the whole task tree, and a task tree includes anything the
// process SPAWNS. This one spawns a recompression helper that decodes and
// re-encodes gigabytes, so its instructions landed in the run's own count --
// and the same work with recompression off then looked like different work,
// which is the one comparison these counters exist to support. Asking the
// kernel to drop the event when a task execs removes the helper without
// touching the threads that do the analysis, since those never exec.
//
// Kernels before 5.13 have no such bit and reject the attribute outright, so
// the open is retried without it rather than losing the counters entirely.
int openCounter(uint64_t config, bool dropOnExec) {
  struct perf_event_attr attr;
  ::memset(&attr, 0, sizeof attr);
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof attr;
  attr.config = config;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.inherit = 1; // count every thread this process spawns
#if defined(PERF_ATTR_SIZE_VER7)
  if (dropOnExec)
    attr.remove_on_exec = 1;
#else
  (void)dropOnExec;
#endif
  // CLOEXEC as well: the helper has no use for the descriptor either.
  const int fd = static_cast<int>(::syscall(SYS_perf_event_open, &attr, /*pid=*/0, /*cpu=*/-1,
                                            /*group=*/-1, PERF_FLAG_FD_CLOEXEC));
  return fd; // < 0 simply means "not permitted here"
}

int openCounter(uint64_t config) {
  const int fd = openCounter(config, /*dropOnExec=*/true);
  return fd >= 0 ? fd : openCounter(config, /*dropOnExec=*/false);
}

uint64_t readCounter(int fd) {
  if (fd < 0)
    return 0;
  uint64_t v = 0;
  if (::read(fd, &v, sizeof v) != static_cast<ssize_t>(sizeof v))
    return 0;
  return v;
}
} // namespace

CpuCounters::CpuCounters() {
  insFd_ = openCounter(PERF_COUNT_HW_INSTRUCTIONS);
  cycFd_ = openCounter(PERF_COUNT_HW_CPU_CYCLES);
  if (insFd_ >= 0)
    ::ioctl(insFd_, PERF_EVENT_IOC_ENABLE, 0);
  if (cycFd_ >= 0)
    ::ioctl(cycFd_, PERF_EVENT_IOC_ENABLE, 0);
}

CpuCounters::~CpuCounters() {
  if (insFd_ >= 0)
    ::close(insFd_);
  if (cycFd_ >= 0)
    ::close(cycFd_);
}

uint64_t CpuCounters::instructions() const { return readCounter(insFd_); }
uint64_t CpuCounters::cycles() const { return readCounter(cycFd_); }

#else  // not Linux: no perf_event_open, rusage still works

CpuCounters::CpuCounters() = default;
CpuCounters::~CpuCounters() = default;
uint64_t CpuCounters::instructions() const { return 0; }
uint64_t CpuCounters::cycles() const { return 0; }

#endif

uint64_t CpuCounters::liveThreads() {
#if defined(__linux__)
  FILE* f = ::fopen("/proc/self/status", "re");
  if (!f)
    return 0;
  char buf[256];
  uint64_t n = 0;
  while (::fgets(buf, sizeof buf, f))
    if (::strncmp(buf, "Threads:", 8) == 0) {
      n = ::strtoull(buf + 8, nullptr, 10);
      break;
    }
  ::fclose(f);
  return n;
#else
  return 0;
#endif
}

uint64_t CpuCounters::processCpuUs() {
  struct rusage ru;
  if (::getrusage(RUSAGE_SELF, &ru) != 0)
    return 0;
  const uint64_t u = static_cast<uint64_t>(ru.ru_utime.tv_sec) * 1000000ull +
                     static_cast<uint64_t>(ru.ru_utime.tv_usec);
  const uint64_t s = static_cast<uint64_t>(ru.ru_stime.tv_sec) * 1000000ull +
                     static_cast<uint64_t>(ru.ru_stime.tv_usec);
  return u + s;
}

void WidthSampler::sample() {
  struct timespec ts;
  if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return;
  const uint64_t wall = static_cast<uint64_t>(ts.tv_sec) * 1000000ull +
                        static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
  // Lock-free gate: this is called from the read path, which runs millions of
  // times, and all but one call a second must cost a load and a compare.
  uint64_t due = nextSampleUs_.load(std::memory_order_relaxed);
  if (wall < due)
    return;
  if (!nextSampleUs_.compare_exchange_strong(due, wall + 1000000ull,
                                             std::memory_order_relaxed))
    return; // another thread is taking this second's sample
  const uint64_t cpu = CpuCounters::processCpuUs();
  std::lock_guard<std::mutex> g(mu_);
  if (lastWallUs_ == 0) {
    lastWallUs_ = wall;
    lastCpuUs_ = cpu;
    return;
  }
  const uint64_t dw = wall - lastWallUs_;
  if (dw == 0)
    return;
  const double cores = static_cast<double>(cpu - lastCpuUs_) / static_cast<double>(dw);
  lastWallUs_ = wall;
  lastCpuUs_ = cpu;
  if (cores > peakCores_)
    peakCores_ = cores;
}

uint64_t WidthSampler::width() const {
  std::lock_guard<std::mutex> g(mu_);
  return static_cast<uint64_t>(peakCores_ + 0.5);
}

WidthSampler& widthSampler() {
  static WidthSampler* s = new WidthSampler(); // never destroyed, on purpose
  return *s;
}

} // namespace ucache
