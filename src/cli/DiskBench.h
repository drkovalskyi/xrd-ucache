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
// Three properties the numbers depend on, and which the output therefore
// states outright:
//   - The read and in-place write phases cycle inside the one test file, so a
//     long window on a fast device cannot grow it beyond --size. The fill stage
//     is the exception and is bounded by volume instead: it RETAINS what it
//     writes, because a cache does.
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
  // The fill stage models a COLD FILL, and every element of it is there because
  // the previous version's shape was measured against a real one and found to
  // overstate it by ~1.7x on the same disk:
  //   - writes land at SCATTERED offsets in a SPARSE file, so every write is a
  //     first touch that allocates. Appending to a dense file instead let the
  //     kernel merge 48 KiB writes into ~510 KiB device writes, which a cache
  //     never gets: its writes go wherever the analysis read, and the next one
  //     is somewhere else.
  //   - files are RETAINED for the whole stage, so free space falls and
  //     garbage-collection pressure rises as it does during a real fill. The
  //     previous version unlinked each file and recycled the same blocks.
  //   - nothing is synced until the end, because a cache does not sync either
  //     (fsync defaults to off). That is also the only way the stage can reach
  //     the kernel's writeback throttle, which any fill larger than the dirty
  //     limit spends most of its life in.
  // Bounded by VOLUME rather than by a window, since the throttle knee is at a
  // byte count: default 1.5x the machine's dirty limit, enough to cross it and
  // still measure a sustained rate afterwards.
  int fillWriters = 4;               // --fill writers= (measured: ~4 in flight)
  uint64_t fillBlock = 48ull << 10;  // --fill block=   (measured mean run)
  uint64_t fillVolume = 0;           // --fill volume=  (0 = 1.5x dirty limit)
  std::string logPath = "ucache-bench.txt"; // --log; empty = --no-log
  std::string cmdline;               // the invocation, recorded in the log
};

// Runs the full suite on each path (a directory; a temp subdir is created and
// removed). Prints the run plan, then a human table plus one
// `ucache-bench-json: {...}` line per path, and a comparison table when more
// than one path is given. Returns 0 if every path completed, 1 if any failed.
int runDiskBench(const std::vector<std::string>& paths, const DiskBenchOpts& opts);

} // namespace ucache
