// `ucache bench`: raw storage self-test of the cache dir (or any
// candidate dir). Measures the access patterns ucache actually generates —
// scattered small writes + fsync (the fill), random small reads at 1 and N
// streams (hit serving; flat N-stream scaling exposes QoS quotas), sequential
// read/write (the replica pattern), reads under writeback (the observed
// production killer mode), create/unlink (cleanup). RAW numbers only — no
// verdicts; recommendations become a doctor feature once fleet data exists.
//
// Methodology (hardened by field probe failures): O_DIRECT when the
// filesystem supports it (else buffered + posix_fadvise-evict, and the mode
// is reported); a dedicated non-sparse test file (never reads cache objects —
// sparse holes measure nothing); every phase time-capped.
//
// Thread-safety: runDiskBench is single-caller (CLI); it spawns and joins its
// own worker threads internally.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

struct DiskBenchOpts {
  uint64_t fileBytes = 1ull << 30; // --size (test file; floor 16 MiB)
  double phaseSeconds = 5.0;       // --seconds (per timed phase)
  int wideStreams = 16;            // parallel-read stream count
};

// Runs the full suite on each path (a directory; a temp subdir is created and
// removed). Prints a human table plus one `ucache-bench-json: {...}` line per
// path, and a comparison table when more than one path is given. Returns 0 if
// every path completed, 1 if any failed.
int runDiskBench(const std::vector<std::string>& paths, const DiskBenchOpts& opts);

} // namespace ucache
