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
  // The most cores this run ever had busy in one second: the ceiling its
  // parallelism reached, measured rather than declared. Against coresBusy()
  // (the MEAN) it says whether the run used the resources it had -- a peak of
  // 30 with a mean of 3 is a run that could go wide and spent its life
  // waiting. Two runs with very different peaks are not comparable: they had
  // different amounts of machine, and a ratio between them measures that.
  uint64_t peakCores = 0;
  // Reads the application had outstanding at once. The load the ORIGIN sees,
  // and not derivable from thread counts. Displayed, never matched on.
  // Reads outstanding AT THE ORIGIN. Records written before this was measured
  // carry only the superseded dispatch-window counter, which is a different
  // quantity: they report nothing here rather than have the old number read as
  // the new one.
  uint64_t originReadsInFlight = 0;
  // Whether the record carried these at all. Zero is a real reading for both.
  bool havePeakCores = false, haveOriginReadsInFlight = false;

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
  std::vector<uint64_t> histFlushWrite, histMetaFlush;

  // Per-entry records from the companion, keyed by URL.
  struct FileRec {
    uint64_t ts = 0, opens = 0, servedBytes = 0, ramBytes = 0, replicaBytes = 0;
    uint64_t diskReads = 0, diskSeq = 0, diskBytes = 0, firstTouchBytes = 0, wireBytes = 0;
    uint64_t spanUs = 0;     // wall this file was live and working, µs
    uint64_t originSize = 0; // the file's size AT THE ORIGIN — the one work
                             // measure that means the same on every route, and
                             // therefore the only sound weight for a share
    uint64_t readBuckets = 0; // how many 1 MiB pieces of the file were read
    std::string readSig;     // which parts of THIS file the run read, in
                             // origin coordinates; empty when unknown (a
                             // replica built before the map existed)
    std::string mode;        // cached | fill | relay ("" = pre-span record)

    // The three tiers a byte can reach the application by. They are DISJOINT
    // counters written at three different places -- the byte tier in
    // readCached, the replica tier where stitched bytes are handed over, the
    // origin where fetched bytes are staged -- so the work this file did is
    // their SUM. Nothing here may be derived by comparing two of them: taking
    // the larger of served and wire silently drops the replica tier entirely,
    // which made a warm replica run, whose bytes are almost all replica, look
    // like a run that had been served nothing and fetched everything.
    //
    // ramBytes is deliberately absent: it is a subset of servedBytes (the part
    // of a byte-tier hit that came from a staged page rather than the disk),
    // not a fourth tier, and adding it would count those bytes twice.
    uint64_t deliveredBytes() const { return servedBytes + replicaBytes + wireBytes; }
    // Of that, what the cache itself delivered.
    uint64_t cacheDeliveredBytes() const { return servedBytes + replicaBytes; }
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
  // Hash of what was actually READ, not merely opened -- combined over the
  // per-file footprints. Two runs of different analyses over the same inputs
  // share a `sig` and differ here, which is the only thing that separates
  // them. Empty when any file could not contribute one, because a partial
  // answer would silently compare different work. This is an IDENTITY, for
  // display: whether two runs may be compared is decided from the per-file
  // signatures instead, since two routes can honestly disagree about a file
  // or two out of hundreds without having done different work.
  std::string readSig;
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
  // Superseded by overhead(), which measures the same thing against the right
  // denominator: cache-write time over ORIGIN-wait time, not over CPU time.
  // Kept because it is what the records of earlier runs support.
  double fillCost() const;
  static constexpr double kMinOriginShare = 0.95;
  bool baselineQualified() const {
    return originShare() >= kMinOriginShare && servedLessThanFetched() && overheadKnown() &&
           overhead() <= kMaxOverhead;
  }
  // The origin fetched at least as much as the cache served. This is a BYTE
  // test, and it is here because originShare() is not one: that weights each
  // file by its size AT THE ORIGIN, so a file touched for 5 MB counts the same
  // as a file read whole from the cache. It is therefore a size-weighted count
  // of files, and the cache's byte contribution is unbounded underneath it --
  // 380 files read 5 MB each from the origin and 20 read whole from the cache
  // scores exactly 0.95 while the cache delivered 97% of the bytes, and that
  // run was accepted as the measure of running WITHOUT a cache.
  //
  // Same shape as the defect that made a warm replica run a baseline, through
  // a different door: a run the cache served must never define what no cache
  // costs, however few files it served. This is the comparison a fill is
  // already judged by, applied to the reference as well.
  bool servedLessThanFetched() const { return originBytes >= cacheBytes(); }
  // Average cores executing over the run: CPU time divided by wall. Absolute
  // on purpose -- a SHARE needs a denominator, and threads_high_water counts
  // every thread the process owns (TBB workers, XrdCl, ROOT), not the width
  // the job was asked for, so a percentage against it reads about half what a
  // user expects. Cores busy needs no such assumption: 6 cores on a 32-thread
  // job is starving, 23 is working.
  double coresBusy() const;
  // Mean cores blocked on cache writes. Thread-time over wall, so it does NOT
  // convert to wall overhead -- see overhead().
  double coresWriting() const {
    return bufferStallUs ? (static_cast<double>(bufferStallUs) / 1e6) /
                               static_cast<double>(durationS())
                         : 0.0;
  }

  // Thread-time the run spent waiting on the ORIGIN, and thread-time it spent
  // on cache writes (flush, writer stalls, sidecar stores).
  double originWaitS() const;
  double writeWaitS() const;

  // **A COARSE estimate of what caching cost this run.** Blocked thread-time
  // spent writing to the cache, over blocked thread-time spent waiting on the
  // origin. It needs no baseline -- both terms come from the same run -- which
  // is why it exists: a direct run cannot serve as the reference here, because
  // the same job against the same origin measured 173 s and 1025 s within one
  // evening, and anything computed ACROSS runs inherits that spread.
  //
  // IT IS NOT A FRACTION OF WALL CLOCK, and an earlier version of this comment
  // said it was. Both terms are SUMS of per-operation durations across threads,
  // so each is thread-seconds, and the sum discards when each interval
  // happened. Two things therefore fail to cancel:
  //
  //   - The two activities run at different widths. Origin waits accrue on the
  //     job's read threads (32 in the measured campaigns); cache writes accrue
  //     on the executor pool, capped at 8. One fill recorded 22.8 threads
  //     waiting on the origin on average against 0.26 threads writing, so the
  //     ratio there mostly compared thread counts, not time.
  //   - Even at equal width, a ratio of 1.0 only means "the run took twice as
  //     long" if the two never overlap. They do overlap -- that is the point of
  //     writing on a background pool -- so it overstates whenever they do.
  //
  // MEASURED ERROR, against nine fills whose no-cache wall was measured
  // directly: reported over true ran from 0.17x to 3.01x, in both directions.
  // Four rearrangements of the recorded counters were tried and none was
  // better; the information needed for a true wall figure (the UNION of the
  // blocked intervals, not their sum) is not recorded at all.
  //
  // SO USE IT ONLY FOR WHAT IT CAN DO: telling a 1-2% effect from a 20%+ one.
  // At that job it was right on all nine. Sorted by the independently measured
  // truth, the reported values were
  //
  //     truly cheap (<=10%) : 0.011, 0.013, 0.015
  //     truly expensive     : 0.608, 1.055, 1.295, 1.459, 1.858, 2.227
  //
  // -- a 40x gap with kMaxOverhead sitting in the middle of it. Nothing was
  // measured between 0.015 and 0.608, so a value landing in that band is
  // outside the evidence and is the trigger to build the real measurement
  // rather than to trust this one.
  //
  // Meaningless for a warm run, which never asks the origin: 0 there means
  // "nothing to compare", not "no overhead".
  double overhead() const {
    const double o = originWaitS();
    return o > 0.0 ? writeWaitS() / o : 0.0;
  }
  // Cache-write time only means something against the origin wait it had to
  // hide behind, so a run with no origin timing cannot be judged at all.
  // Without this, a record too old or too thin to carry the histogram reports
  // overhead 0 and qualifies on an ABSENCE of evidence -- the opposite of what
  // every other guard here does.
  bool overheadKnown() const { return originWaitS() > 0.0; }
  // A fill is usable as a baseline when the cache added little to it. The
  // bound is NOT the cold-pass target restated: overhead() is not in those
  // units (see above). It is a separator, placed in the empty 40x gap between
  // the fills measured as cheap and those measured as expensive, and it is
  // worth exactly as much as that evidence -- nine fills, none of them in the
  // band between.
  static constexpr double kMaxOverhead = 0.10;
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
  // WHY a run has no gain, decided where the decision is actually made. The
  // summary used to re-derive this from the run's own fields and got three
  // classes wrong -- a fill and a run under the size floor both came out as
  // "no comparable baseline", which sent the reader off to record a baseline
  // that would not have helped. A reason and its label must come from one
  // place or they drift.
  enum class Why {
    kMeasured,      // a gain was produced
    kIsBaseline,    // this run IS the reference others are measured against
    kFilled,        // it filled the cache rather than being served by it
    kTooShort,      // under the duration floor
    kTooSmall,      // too little data moved to divide
    kIncomplete,    // no per-file records, or nothing attributable
    kReadDifferent, // a baseline exists but the two read different work
    kNoBaseline,    // nothing recorded that could serve as a reference
  };
  Why why = Why::kNoBaseline;
  bool valid = false;
  double gain = 0.0;          // (wall + saved) / wall
  double savedS = 0.0;        // seconds the origin would have cost beyond the cache
  double originMBs = 0.0;     // rate the baseline run saw from the origin
  double cacheMBs = 0.0;      // rate this run delivered at
  uint64_t originEquivBytes = 0; // what the origin would have shipped, NOT bytes served
  uint64_t matchedFiles = 0, runFiles = 0;
  // How many of the matched files BOTH runs left a read signature for, and
  // whether that was enough to actually check that the two runs did the same
  // work. False means the walls were compared on the strength of the file
  // names alone -- true of records written before signatures existed, and
  // worth saying rather than leaving a reader to assume the check ran.
  uint64_t sigPairs = 0;
  // The denominator the coverage RULE runs against, which is NOT matchedFiles:
  // it counts only files the reference actually fetched over the wire, because
  // a reference file with no wire bytes says nothing about what the origin
  // cost. Printing sigPairs over matchedFiles told the reader the check was
  // weaker than it was -- a comparison that legitimately cleared the half-way
  // bar could show a ratio below half.
  uint64_t comparedFiles = 0;
  bool workVerified = false;
  uint64_t referenceStartS = 0;
  // Which KIND of reference produced the number: a run explicitly recorded
  // with the cache disabled, or an ordinary first pass the estimator judged
  // effectively cache-free (a qualifying fill). The two carry different
  // confidence -- a flag in a record cannot be wrong, an inference can -- and
  // the output that presents the number must say which it stands on. Before
  // this existed the human text said "with the cache disabled" and the JSON
  // said "baseline" unconditionally, both wrong whenever the reference was a
  // fill.
  bool referenceDisabled = false;
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
// The same ceiling, for anything else that has to estimate run by run: every
// estimate walks every run, so an uncapped pass is quadratic in a directory
// that only ever grows.
inline constexpr size_t kMaxSummaryEstimates = 100;

// Records nobody made. The store writes a counter line when it closes, and
// every `ucache` invocation opens one, so a cache directory fills up with
// one-second entries carrying no files and no bytes, sitting among the runs
// that matter and burying them. Ten seconds is far under the floor any
// measurement needs and far over anything a CLI call takes, so it separates
// the two without argument.
inline constexpr uint64_t kMinListedDurationS = 10;
std::vector<Run> withoutTrivial(std::vector<Run> runs);

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
