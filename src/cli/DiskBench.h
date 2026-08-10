// `ucache bench`: raw storage self-test of the cache dir (or any
// candidate dir). Measures the access patterns ucache actually generates —
// scattered small writes + fsync (the fill), random small reads at several
// stream counts (hit serving; flat scaling exposes QoS quotas), large-block
// reads and writes at several stream counts (the replica pattern and the
// batched fill), sequential read, reads under writeback (the observed
// production killer mode), create/unlink (cleanup). RAW numbers only — no
// verdicts; recommendations become a doctor feature once fleet data exists.
//
// Methodology (hardened by field probe failures): O_DIRECT when the
// filesystem supports it (else buffered + posix_fadvise-evict, and the mode
// is reported); a dedicated non-sparse test file (never reads cache objects —
// sparse holes measure nothing); every phase time-capped, so the tool still
// finishes on a volume that delivers a few dozen IOPS.
//
// Two properties the numbers depend on, and which the output therefore
// states outright:
//   - Multi-stream phases cycle IN PLACE over per-stream slices of the one
//     test file, so a long write window on a fast device cannot grow disk
//     usage beyond --size.
//   - A device write cache makes the first seconds of a write far faster
//     than the steady state, so the build phase reports burst and sustained
//     rates separately and the quotable figure is the sustained one.
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
  // Job concurrency for the PATTERN measurements. One number, optional,
  // defaulting to the core count: it used to be inherited from the queue-depth
  // list, so a bare run measured "job concurrency" at 16 on a 64-core box.
  int threads = 0;                   // --threads (0 = auto: nproc)
  uint64_t blockBytes = 4ull << 20;  // --block (sequential read/write size)
  // The expected-fill-pattern stage. Defaults are MEASURED, not assumed: a
  // cold analysis fills through ONE stream (uCache keys one entry per URL and
  // drains per entry, so the analysis thread count never reaches the write
  // path) in ~48 KiB writes (one contiguous run of requested pages — the
  // basket, not the coalescing ceiling).
  int fillWriters = 1;               // --fill writers=
  uint64_t fillBlock = 48ull << 10;  // --fill block=
  uint64_t fillFile = 256ull << 20;  // --fill file=
  std::string logPath = "ucache-bench.txt"; // --log; empty = --no-log
  std::string cmdline;               // the invocation, recorded in the log
};

// Runs the full suite on each path (a directory; a temp subdir is created and
// removed). Prints the run plan, then a human table plus one
// `ucache-bench-json: {...}` line per path, and a comparison table when more
// than one path is given. Returns 0 if every path completed, 1 if any failed.
int runDiskBench(const std::vector<std::string>& paths, const DiskBenchOpts& opts);

} // namespace ucache
