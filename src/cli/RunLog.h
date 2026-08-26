// Run-level view of the records uCache already writes.
//
// A "run" is one process's work with the cache. The store claims
// `<host>-<pid>-<start>-<seq>.jsonl` when it is constructed and appends a
// cumulative counter line when it tears down, while every file close appends a
// record to the `.files.jsonl` companion. Those two files together say what one
// job did and when, so a history needs no periodic sampling: the events already
// carry their own timestamps, and the gap between them means no bytes moved.
//
// Nothing here opens the cache or needs the plugin — it is file parsing, which
// is what makes it testable against a synthesized stats directory.
//
// Thread-safety: none required; plain readers used by one CLI process.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ucache {

// One process's contribution to the record.
struct Run {
  std::string host;
  std::string stem;      // path with no suffix; identifies the companion files
  uint64_t pid = 0;
  uint64_t startS = 0;   // store construction, from the file name
  uint64_t endS = 0;     // last cumulative line, or the newest per-file record
  bool complete = false; // a cumulative line was found (a killed job has none)

  // Cumulative counters, from the last complete line.
  uint64_t opens = 0, filesOpened = 0;
  uint64_t servedBytes = 0, hitBytes = 0, ramHitBytes = 0, replicaBytesServed = 0;
  uint64_t relayBytes = 0, originBytes = 0, originReads = 0, originReadvs = 0;
  uint64_t hitDiskReads = 0, hitDiskBytes = 0, replicaReads = 0, replicaReadBytes = 0;
  uint64_t crcFailures = 0, replicaCrcFailures = 0, replicaInvalid = 0;
  uint64_t failopenEvents = 0, metaCorrupt = 0, validationsFailed = 0;
  uint64_t evictedEntries = 0, evictedBytes = 0;
  uint64_t pageWrites = 0, flushRuns = 0, flushRunBytes = 0;
  uint64_t bufferStalls = 0, bufferStallUs = 0;
  std::vector<uint64_t> histHitRead, histReplicaRead, histOriginRt, histOpen;

  // Per-entry records from the companion, keyed by URL.
  struct FileRec {
    uint64_t ts = 0, opens = 0, servedBytes = 0, ramBytes = 0, replicaBytes = 0;
    uint64_t diskReads = 0, diskSeq = 0, diskBytes = 0, firstTouchBytes = 0, wireBytes = 0;
  };
  std::map<std::string, FileRec> files;

  // Wall span the cache was live for. Never zero: a sub-second run would
  // otherwise divide by nothing, and one second is the clock's resolution.
  uint64_t durationS() const { return endS > startS ? endS - startS : 1; }
  // Bytes the cache itself delivered. Relay bytes are excluded on purpose:
  // the cache never touched them, so they are not something it can be credited
  // with, and origin bytes are what it FAILED to have.
  uint64_t cacheBytes() const { return hitBytes + replicaBytesServed; }
  // A run that asked the origin for nothing and was served by the cache.
  bool warm() const { return originBytes == 0 && cacheBytes() > 0; }
  // The acceptance triple: anything non-zero means correctness machinery fired.
  uint64_t faults() const {
    return crcFailures + replicaCrcFailures + replicaInvalid + failopenEvents + metaCorrupt;
  }
};

// Every run in a stats directory, newest first. A missing directory yields an
// empty vector rather than an error: a cache that has never been used by a job
// is a normal state, not a fault.
std::vector<Run> loadRuns(const std::string& statsDir);

// What the cache bought, estimated from two rates the records already carry:
// what the origin delivered when these files were first fetched, and what the
// cache delivers now.
//
// The estimate is deliberately refusable. Outside the conditions it was
// validated under it reports `valid == false` with a `reason`, and the caller
// must print the reason rather than the number -- a confidently wrong speedup
// is worse than no speedup.
struct GainEstimate {
  bool valid = false;
  double gain = 0.0;          // (wall + saved) / wall
  double savedS = 0.0;        // seconds the origin would have cost beyond the cache
  double originMBs = 0.0;     // rate measured when the reference run filled
  double cacheMBs = 0.0;      // rate this run delivered at
  uint64_t originEquivBytes = 0; // what the origin would have shipped, NOT bytes served
  uint64_t matchedFiles = 0, runFiles = 0;
  uint64_t referenceStartS = 0;
  std::string reason;         // set whenever !valid; also worth printing when valid
};

// Everything the cache has done, across every run it still has records for.
// This is the headline: one run says what happened last time, the aggregate
// says whether the cache is worth having.
struct Totals {
  size_t runs = 0;          // runs with records
  size_t runsEstimated = 0; // runs the gain estimate accepted
  size_t distinctFiles = 0; // union across runs, not a sum
  uint64_t firstStartS = 0, lastEndS = 0;
  uint64_t durationS = 0;   // summed run spans, which is what the rates divide by
  uint64_t hitBytes = 0, replicaBytes = 0, relayBytes = 0, originBytes = 0;
  uint64_t faults = 0;
  double savedS = 0.0;      // summed over the runs that were estimated
  double estimatedDurationS = 0.0; // and their spans, so the ratio is honest
  bool haveGain = false;
  double gain = 0.0;        // (their span + what they saved) / their span
  size_t gainCapped = 0;    // runs NOT estimated because of the work cap, if any

  uint64_t cacheBytes() const { return hitBytes + replicaBytes; }
};

// Aggregate, estimating the gain for at most `maxEstimates` of the newest runs
// (each estimate is a pass over every other run, so this is quadratic and has
// to stop somewhere). Whatever it skips is reported in `gainCapped` rather
// than silently dropped.
Totals summarize(const std::vector<Run>& runs, size_t maxEstimates = 100);

// `all` should be the output of loadRuns (order is not relied on). The reference
// is chosen from runs that precede `run` in time and actually fetched from
// the origin.
GainEstimate estimateGain(const Run& run, const std::vector<Run>& all);

} // namespace ucache
