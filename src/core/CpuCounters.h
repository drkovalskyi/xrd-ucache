// Per-process CPU accounting for the run record: how much work a run did, and
// how much of its wall the machine actually spent doing it.
//
// Two sources, deliberately both:
//
// * `getrusage` — CPU time for the whole process, always available. Complete,
//   but coarse: it cannot separate work from spin, and it says nothing about
//   how efficiently the work executed.
// * `perf_event_open` — retired instructions and cycles. Precise, and their
//   ratio is the only way to tell "the same job ran slower" from "a different
//   job ran". BUT the counters can only start when this object is built, and
//   the plugin is loaded on the first client call — so a process that spends
//   time before touching a file has that time in rusage and NOT in the
//   counters. Consumers must treat instruction totals as "since the cache
//   engaged", never as "for the process".
//
// Instruction totals are the work fingerprint: two runs that executed the same
// number of instructions did the same computation, which is what makes their
// walls comparable. Measured on this project, a cached and a cache-disabled run
// of one job agreed to 2.5% on instructions and 0.6% on cycles while their
// walls differed 2.5x.
//
// perf_event_open is unavailable under some kernel settings and in some
// containers. That is not an error: the counters simply report zero and every
// consumer falls back to rusage.
//
// Thread-safety: open/read from any thread; the counters aggregate all threads
// of this process (inherit=1). Reads are independent.
#pragma once

#include <cstdint>

namespace ucache {

class CpuCounters {
 public:
  CpuCounters();  // opens and enables the counters if the kernel permits
  ~CpuCounters();
  CpuCounters(const CpuCounters&) = delete;
  CpuCounters& operator=(const CpuCounters&) = delete;

  // Retired instructions / CPU cycles since construction; 0 when unavailable.
  uint64_t instructions() const;
  uint64_t cycles() const;
  // True when the kernel gave us the counters.
  bool available() const { return insFd_ >= 0 && cycFd_ >= 0; }

  // Process CPU time (user+system) in microseconds, from getrusage. Always
  // available, and covers the whole process rather than only our lifetime.
  static uint64_t processCpuUs();

 private:
  int insFd_ = -1;
  int cycFd_ = -1;
};

} // namespace ucache
