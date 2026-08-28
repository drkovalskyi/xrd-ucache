#include "CpuCounters.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
int openCounter(uint64_t config) {
  struct perf_event_attr attr;
  ::memset(&attr, 0, sizeof attr);
  attr.type = PERF_TYPE_HARDWARE;
  attr.size = sizeof attr;
  attr.config = config;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  attr.inherit = 1; // count every thread this process spawns
  const int fd = static_cast<int>(
      ::syscall(SYS_perf_event_open, &attr, /*pid=*/0, /*cpu=*/-1, /*group=*/-1, /*flags=*/0));
  return fd; // < 0 simply means "not permitted here"
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

} // namespace ucache
