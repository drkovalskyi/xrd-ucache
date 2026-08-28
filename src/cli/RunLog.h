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
  bool disabled = false; // ran with the cache OUT OF THE LOOP (UCACHE_DISABLE):
                         // a measured BASELINE, the only sound gain reference
  uint64_t handlesHighWater = 0; // peak concurrent cache handles; runs are only
                                 // pooled across time when this agrees
  // Work done and the width it was done at (CpuCounters.h). Instructions are
  // the fingerprint that says two runs are comparable; cpuUs against
  // wall x threads is the share of the machine that was computing rather than
  // waiting, which bounds from above what any cache could still win.
  uint64_t cpuUs = 0, instructions = 0, cycles = 0, threadsHighWater = 0;

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
    uint64_t spanUs = 0;     // wall this file was live and working, µs
    uint64_t originSize = 0; // the file's size AT THE ORIGIN — the one work
                             // measure that means the same on every route, and
                             // therefore the only sound weight for a share
    std::string mode;        // cached | fill | relay ("" = pre-span record)
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

  // ---- identity -----------------------------------------------------------
  // Hash of the sorted set of input URLs, as 6 hex characters. Two runs over
  // the same files share it; a run that died early opened fewer files and so
  // does not, which is what keeps a truncated run from being compared.
  // Computed by loadRuns; empty when the run left no per-file records.
  std::string sig;
  // Origin hosts seen, by file count. A dataset can be served from several
  // sites (redirectors, distributed sets), and the mix is a property of a RUN,
  // not of the input set: a baseline drawn from one site is weak evidence for
  // a run whose data came from another.
  std::map<std::string, uint64_t> originHosts;
  std::string topOriginHost() const;

  // ---- baseline qualification --------------------------------------------
  // A run is usable as a baseline when it is close enough to a pure direct
  // run. Two deviations, opposite signs, both bounded:
  //   * bytes served from cache make it FASTER  -> understates gain (safe)
  //   * fill-side cost makes it SLOWER          -> overstates gain (unsafe)
  // Measured in origin-equivalent bytes, because the replica tier serves
  // recompressed data and its byte count is not in the same units as the
  // origin's.
  double originShare() const;
  // Share of this run's THREAD-TIME that went into blocking on cache writes:
  // stall / (cpu + stall). Both terms are thread-time, so the ratio needs no
  // thread count -- which is what makes it usable. A version normalised by
  // threads_high_water was tried and FAILED in the field: that counter is
  // every thread the process owns, and an RNTuple job reported 584 where the
  // job was given 32, scaling a 35% fill down to 0.9% and passing a fill whose
  // wall was 2.12x its own direct reference. Measured across three fills whose
  // truth is known: 34.8% (2.12x, reject), 1.9% (0.97x) and 0.7% (1.01x).
  double fillCost() const;
  static constexpr double kMinOriginShare = 0.95;
  static constexpr double kMaxFillCost = 0.05;
  bool baselineQualified() const {
    return originShare() >= kMinOriginShare && fillCost() <= kMaxFillCost;
  }
  // Average cores executing over the run: CPU time divided by wall. Absolute
  // on purpose -- a SHARE needs a denominator, and threads_high_water counts
  // every thread the process owns (TBB workers, XrdCl, ROOT), not the width
  // the job was asked for, so a percentage against it reads about half what a
  // user expects. Cores busy needs no such assumption: 6 cores on a 32-thread
  // job is starving, 23 is working.
  double coresBusy() const;
  // Share of wall x threads spent on CPU. Only meaningful where the caller
  // knows the intended width; prefer coresBusy() for display.
  double busy() const;
};

// Every run in a stats directory, newest first. A missing directory yields an
// empty vector rather than an error: a cache that has never been used by a job
// is a normal state, not a fault.
//
// `archiveDir`, when given, is read too. `ucache stats --reset` starts a fresh
// COUNTER window by moving the records there rather than deleting them, so the
// performance record outlives a measurement window -- losing months of history
// to a command documented as "start a fresh measurement" would be a poor trade.
std::vector<Run> loadRuns(const std::string& statsDir, const std::string& archiveDir = "");

// What the cache bought, measured against a BASELINE run -- the same work done
// once with the cache out of the loop (UCACHE_DISABLE=1), which the plugin
// records like any other run. Only a measured baseline is a sound reference:
// two record-based substitutes were built and refuted against ground truth --
// the fill's wall (its cost is set by the cache's own writes: reported 1.7x
// where the truth was 0.70x) and service-time histograms (blocked-wait time is
// not wall time under 32-way overlap: reported 2.8x for the same 0.70x). What
// the records cannot carry, the estimate must not invent.
//
// The estimate is deliberately refusable. Outside the conditions it holds
// under it reports `valid == false` with a `reason`, and the caller must print
// the reason rather than the number -- a confidently wrong speedup is worse
// than no speedup. A gain BELOW 1 is reported, not clamped: a cache slower
// than the origin is a real answer and the whole point of measuring.
struct GainEstimate {
  bool valid = false;
  double gain = 0.0;          // (wall + saved) / wall
  double savedS = 0.0;        // seconds the origin would have cost beyond the cache
  double originMBs = 0.0;     // rate the baseline run saw from the origin
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
  size_t runsEstimated = 0; // runs any method could measure
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

// NOTE. An estimate built from per-file spans lived here and was REMOVED. It
// compared per-file COST between the two routes and was blind to OVERLAP --
// how much of that cost runs concurrently -- which differs between routes by
// more than the cost does. On a workload whose measured truth was a 0.72x LOSS
// it reported a 1.34x gain. Wall time is cost divided by overlap, and only a
// measurement spanning a period when the whole application used ONE route
// contains the overlap term.

// `all` should be the output of loadRuns (order is not relied on). The reference
// is chosen from runs that precede `run` in time and actually fetched from
// the origin.
GainEstimate estimateGain(const Run& run, const std::vector<Run>& all);

// One input set, across every run that used it. This is the view that answers
// "is the cache of use to me": gain is a property of a WORKLOAD, not of a
// cache, and a single aggregate hides the case that matters -- one dataset
// gaining 2.8x while another loses. Keyed by the input signature, so grouping
// needs nothing from the user.
struct Dataset {
  std::string sig;
  size_t runs = 0, measured = 0;
  size_t baselines = 0;          // runs usable as a reference for this set
  size_t incomplete = 0;         // runs that left no cumulative line (killed,
                                 // or still going) -- not a reason to advise
                                 // anything, they simply have not finished
  size_t files = 0;              // distinct files, the largest run's set
  size_t dirs = 0;               // distinct directories the inputs live in
  uint64_t originSize = 0;       // the input set's size at the origin
  uint64_t readBytes = 0;        // summed over its runs
  uint64_t measuredBytes = 0;    // ... over the runs that got a gain: coverage
                                 // by VOLUME, which run counts misrepresent
  std::map<std::string, uint64_t> hosts; // origin host -> file count
  // Aggregated per tier, by summed walls rather than averaged ratios: a mean
  // of ratios weights a two-minute run like a two-hour one.
  double byteWall = 0.0, byteSaved = 0.0;
  double replWall = 0.0, replSaved = 0.0;
  size_t byteRuns = 0, replRuns = 0;

  bool haveByte() const { return byteRuns && byteWall > 0.0; }
  bool haveRepl() const { return replRuns && replWall > 0.0; }
  double gainByte() const { return (byteWall + byteSaved) / byteWall; }
  double gainRepl() const { return (replWall + replSaved) / replWall; }
  std::string topHost() const;
};

// Group runs by input signature, newest-first within each. `maxEstimates`
// caps the gain estimation as in summarize().
std::vector<Dataset> byDataset(const std::vector<Run>& runs, size_t maxEstimates = 100);

} // namespace ucache
