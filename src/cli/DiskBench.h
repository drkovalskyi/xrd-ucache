// `ucache bench`: raw storage self-test of the cache dir (or any
// candidate dir). Measures the access patterns ucache actually generates —
// random small reads at several stream counts (hit serving; flat scaling
// exposes QoS quotas), large-block reads and writes at several stream counts
// (the replica pattern), sequential read, reads under writeback (the observed
// production killer mode), create/unlink (cleanup). RAW numbers only — no
// verdicts; recommendations become a doctor feature once fleet data exists.
// The FILL is deliberately not among them: an imitation of it measured 1.4x
// FASTER than a real cold fill of the same data on the same disk, so it was
// retired rather than tuned and the product's own code measures it instead
// (--cache-path, CacheBench.h).
//
// Methodology (hardened by field probe failures): keep the page cache out of
// the measurement, use a dedicated non-sparse test file (never reads cache
// objects — sparse holes measure nothing), and time-cap every phase so the
// tool still finishes on a volume that delivers a few dozen IOPS.
//
// HOW the page cache is kept out differs by platform, and the difference is
// reported rather than hidden — the `mode` field says which was used:
//   Linux   O_DIRECT, a guarantee with an alignment contract; buffered plus
//           posix_fadvise-evict where the filesystem refuses it.
//   macOS   F_NOCACHE, set after open. It ASKS the kernel not to cache this
//           file's data: no alignment contract, no promise that a given
//           transfer bypassed the cache, and no way to drop pages already
//           resident. Measured latencies show it working (device-real
//           microseconds, not RAM), but a macOS figure is not interchangeable
//           with an O_DIRECT one and should not be put in the same table
//           without saying so.
// macOS also has no per-device counters (/proc/diskstats) and no iowait, so
// the record omits those fields instead of substituting weaker ones; the
// device is named but not described.
//
// Three properties the numbers depend on, and which the output therefore
// states outright:
//   - Every phase cycles inside the one test file, so a long window on a fast
//     device cannot grow it beyond --size. The --cache-path stage is the
//     exception and is bounded by volume instead: it RETAINS what it writes,
//     because a cache does.
//   - Writing to fresh extents is not the same measurement as writing over
//     allocated ones. The test-file build pays allocation and the in-place
//     write does not, so their ratio is the cost of allocating on this device —
//     nothing on one disk measured here, nearly 2x on another of the same model.
//   - A rate is only a device property if the write finished. The build stage is
//     time-capped, and a capped run reports what that slow episode achieved
//     rather than what the device can do; `stopped at time cap` says which.
//
// Every run also appends a record to a log file (default `ucache-bench.txt`
// in the working directory) carrying the numbers plus the context needed to
// read them later: the exact command, the time, the mount and block device
// behind the target directory, and the machine's CPU and IO load.
//
// Thread-safety: runDiskBench is single-caller (CLI); it spawns and joins its
// own worker threads internally.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

struct DiskBenchOpts {
  uint64_t fileBytes = 1ull << 30;   // --size (test file ceiling; floor 16 MiB)
  double measurementSeconds = 5.0;   // --measurement-duration (per measurement)
  // Job concurrency for the PATTERN measurements. One number, REQUIRED: it is
  // what the caller's analyses run at, and neither the queue-depth list nor the
  // core count is an honest stand-in for that.
  int threads = 0;                   // --threads (0 = not given; refused)
  uint64_t blockBytes = 4ull << 20;  // --block (sequential read/write size)
  // --sweep: measure the random read at a ladder of block sizes instead of
  // just the two tier sizes. The knee between op-bound and bandwidth-bound
  // lies between 4 and 48 KiB on the devices measured so far, and no pair of
  // points either side of it can locate it. Off by default: it is device
  // characterisation, worth doing once per device rather than every run.
  bool sweep = false;
  // The ARRIVAL PATTERN handed to the --cache-path stage: how many clients are
  // filling at once and how long a contiguous run each one delivers before
  // moving elsewhere. Both are measured from real jobs, and they are the ONLY
  // thing about a fill this tool models — everything downstream of them
  // (staging, sorting, coalescing, checksums, sidecars) is the product's code.
  int fillWriters = 4;               // --fill writers= (measured: ~4 in flight)
  uint64_t fillBlock = 48ull << 10;  // --fill block=   (measured mean run)
  // --cache-path: measure the storage through uCache's OWN fill and read code
  // rather than through an imitation of it (CacheBench.h). Opt-in for now: it
  // changes the run's space and time profile substantially, and it is new.
  // --cache-sample overrides the automatic volume, which is otherwise
  // max(2x the kernel dirty limit, 30 s x the measured sequential write rate).
  bool cachePath = false;            // --cache-path
  uint64_t cacheSample = 0;          // --cache-sample (0 = automatic)
  std::string logPath = "ucache-bench.txt"; // --log; empty = --no-log
  std::string cmdline;               // the invocation, recorded in the log
};

// Runs the full suite on each path (a directory; a temp subdir is created and
// removed). Prints the run plan, then a human table plus one
// `ucache-bench-json: {...}` line per path, and a comparison table when more
// than one path is given. Returns 0 if every path completed, 1 if any failed.
int runDiskBench(const std::vector<std::string>& paths, const DiskBenchOpts& opts);

} // namespace ucache
