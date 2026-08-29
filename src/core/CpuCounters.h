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

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

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

  // Threads alive in this process right now (/proc/self/status). 0 where
  // unavailable. Sampled while the run is active rather than at exit, because
  // by exit the pool has wound down and the peak is what a wall must be
  // divided by to mean anything.
  static uint64_t liveThreads();

 private:
  int insFd_ = -1;
  int cycFd_ = -1;
};

// How wide a run COULD go, measured rather than declared.
//
// The job's own thread setting is invisible from here and must stay that way,
// so the question is turned around: what is the most parallelism this run ever
// actually reached? CPU time sampled once a second gives cores-busy per
// interval, and the MAXIMUM over the run is the answer. A job limited to eight
// threads can never exceed eight cores, not even for one second; a job given
// thirty-two still bursts to thirty-two whenever data is there, however
// starved it is on average. So the peak reflects the ceiling the job was
// given, while the mean reflects how well it was fed -- and the two together
// say whether a run used the resources it had.
//
// Two measures that were tried and are worse: threads ALIVE counts every
// thread the process owns (33 for an eight-core job, 640 for an RNTuple one,
// and it differs by route for the same job), and threads that ever used CPU,
// even weighted by how much, returns the same 33 because ROOT spreads work
// across its whole arena.
//
// Sampling must be driven by something that happens THROUGHOUT the run. Driven
// by file opens it measured the opening burst and nothing else, and reported a
// peak of 1 core for a 32-thread job -- opens all land before the compute
// starts. Reads run for the whole run, so that is where it is called from, and
// the once-a-second check is a lock-free load so a hot path can afford it.
class WidthSampler {
 public:
  void sample();
  // Peak cores busy over any sampled interval; 0 before the second sample.
  uint64_t width() const;

 private:
  std::atomic<uint64_t> nextSampleUs_{0}; // lock-free gate for the hot path
  mutable std::mutex mu_;
  uint64_t lastCpuUs_ = 0;
  uint64_t lastWallUs_ = 0;
  double peakCores_ = 0.0;
};

WidthSampler& widthSampler();

} // namespace ucache
