// Measure a storage location through uCache's OWN code, rather than through a
// hand-written imitation of it.
//
// The raw-storage stages in DiskBench are a frozen yardstick: fixed block sizes
// and queue depths, comparable across versions and machines. They cannot answer
// "what does uCache get here", because a model of the fill is a model — the
// previous one was pessimistic on three counts (no RAM staging, no offset
// sorting, no coalescing) and still 1.43x optimistic against a real cold fill.
//
// So this drives FileEntry/CacheStore directly. Staging, the per-entry buffer
// cap, offset-sorted coalesced drains, crc-at-staging, bitmap publication,
// sidecar rewrites, read-run coalescing and per-page verification are all the
// product's, not a reproduction. The ONLY thing still modelled is the order in
// which offsets arrive; replaying a captured trace would remove that too.
//
// Two phases, because they are two different passes a real workload makes and
// they have different limits:
//   FILL — a cold pass does essentially zero cache-disk reads (staged pages
//          serve the client), so a cold pass is a WRITE load and this measures it.
//   READ — a warm pass, with the page cache dropped first and every byte read
//          exactly once, at both tier shapes (scattered ~48 KiB = byte cache,
//          sequential ~512 KiB = replica).
//
// Numbers here are properties of a RELEASE, not of the device alone: change the
// staging policy and they move. The record carries the build id for that reason.
//
// Thread-safety: runCacheBench is single-caller; it spawns and joins its own
// workers.
#pragma once

#include <cstdint>
#include <string>

namespace ucache {

constexpr int kCacheCurveBuckets = 8;

// One phase's result. Rates are PAYLOAD over elapsed; device figures come from
// /proc/diskstats over the same window, because with nothing synced the
// application runs ahead of the disk by whatever is dirty.
struct CachePhase {
  double payloadMbps = 0;   // application bytes / elapsed
  double devMbps = 0;       // device traffic / elapsed (includes fs metadata)
  double devOpKib = 0;      // mean device request size
  double devQueueDepth = 0; // average requests in flight
  double seconds = 0;       // the timed window
  double syncS = 0;         // fill only: making the bytes durable, timed apart
  uint64_t bytes = 0;       // payload
  uint64_t ops = 0;         // application-level calls issued
  uint64_t p50Us = 0, p95Us = 0, p99Us = 0;
  // Rate per eighth of the sample, in order, from device bytes: a fill that
  // crosses the kernel's dirty limit is fast until it does and throttled after,
  // and the knee is at a byte count rather than a time.
  double curve[kCacheCurveBuckets] = {};
  int curveN = 0;
  bool ok = false;
};

struct CacheBenchOpts {
  std::string dir;         // where to build the cache (a subdir is created)
  std::string diskName;    // block device behind it, for /proc/diskstats
  uint64_t sampleBytes = 0;// total payload; see the sizing rule in the plan
  int entries = 8;         // cache entries, filled concurrently
  int writers = 4;         // concurrent fills in flight (measured product value)
  int threads = 1;         // job concurrency for the read phase
  uint64_t fillBlock = 48ull << 10;  // arrival run length
  uint64_t byteReadBlock = 48ull << 10;   // byte-tier read size
  uint64_t replicaReadBlock = 512ull << 10; // replica-tier read size
  int fillBufferMb = -1;   // -1 = product default; 0 = unstaged, for the variant
  double dirtyLimitGb = 0; // reported, and used to say whether the fill crossed it
};

struct CacheBenchResult {
  CachePhase fill, readByte, readReplica;
  uint64_t sampleBytes = 0;
  // Echoed back rather than left to the caller to remember: a record that
  // describes a configuration it did not run is the defect class this project
  // keeps paying for.
  int entries = 0, writers = 0, threads = 0;
  uint64_t fillBlockKib = 0, byteReadKib = 0, replicaReadKib = 0;
  int fillBufferMb = 0;
  // Fill threads blocked in a cap-triggered drain. The COUNT is arithmetic —
  // volume / buffer cap, the same on a fast disk and a slow one (measured: 1592
  // on both of two disks, against 1596 predicted) — so it can never fail and is
  // not evidence of anything. The TIME is the evidence: the share of writer
  // thread-time spent blocked says whether the disk or the generator was the
  // constraint.
  uint64_t stalls = 0;
  double stallMs = 0;
  double stallShare = 0;    // blocked thread-time / total writer thread-time
  uint64_t flushRuns = 0, flushRunBytes = 0; // what the product's drains looked like
  uint64_t hitDiskReads = 0, hitDiskBytes = 0; // and its read path
  bool crossedDirtyLimit = false;
  std::string error; // empty on success
};

// Builds a cache under opts.dir, fills it, then reads it back. Returns with
// `error` set on failure; every phase that ran still carries its numbers.
CacheBenchResult runCacheBench(const CacheBenchOpts& opts);

} // namespace ucache
