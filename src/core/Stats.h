// Counters + log2 latency histograms. These are a correctness
// surface — external tooling consumes them — and are tested as such.
//
// Thread-safety: all mutation is on std::atomic with relaxed ordering
// (monotonic counters; exact cross-counter consistency is not required).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace ucache {

class Tracer; // sampled IO tracer (Trace.h)

// log2-bucketed histogram of a magnitude — microsecond durations, or bytes:
// bucket i holds samples with floor(log2(v)) == i (bucket 0: v <= 1);
// 40 buckets cover ~13 days, or 1 TiB.
struct Histogram {
  static constexpr int kBuckets = 40;
  std::atomic<uint64_t> b[kBuckets] = {};

  void add(uint64_t us) {
    int i = us <= 1 ? 0 : 63 - __builtin_clzll(us);
    if (i >= kBuckets)
      i = kBuckets - 1;
    b[i].fetch_add(1, std::memory_order_relaxed);
  }
  std::string toJson() const; // "[n0,n1,...]" trailing zeros trimmed
};

struct Stats {
  std::atomic<uint64_t> opens{0};
  std::atomic<uint64_t> validationsFailed{0};
  std::atomic<uint64_t> hitBytes{0};
  std::atomic<uint64_t> missBytes{0};
  std::atomic<uint64_t> originBytes{0}; // post-rounding — the amplification numerator
  std::atomic<uint64_t> servedBytes{0};
  std::atomic<uint64_t> originReads{0};
  std::atomic<uint64_t> fetchesJoined{0}; // misses that joined an in-flight fetch
  std::atomic<uint64_t> originReadvs{0};
  std::atomic<uint64_t> pageWrites{0};
  std::atomic<uint64_t> crcFailures{0};
  std::atomic<uint64_t> metaCorrupt{0};
  std::atomic<uint64_t> evictedEntries{0};
  std::atomic<uint64_t> evictedBytes{0};
  std::atomic<uint64_t> failopenEvents{0};
  // Entries NOT admitted because every resident entry was still inside the
  // protection window (Config::evictProtectSeconds). Deliberately separate from
  // failopenEvents: that counter means something went wrong, and the benchmark
  // acceptance triple requires it to be zero. This is a capacity decision, and
  // the reads it declines to cache still succeed.
  std::atomic<uint64_t> admissionsBypassed{0};
  std::atomic<uint64_t> disabledHandles{0};
  // Open-retry (docs/STATS.md):
  std::atomic<uint64_t> openRetries{0};          // transient open failures re-attempted
  std::atomic<uint64_t> openRetriesExhausted{0}; // opens that gave up after the budget
  // Replica tier (docs/STATS.md):
  std::atomic<uint64_t> replicaOpens{0};        // stitched views adopted
  std::atomic<uint64_t> replicaPublished{0};    // successful publishes
  std::atomic<uint64_t> replicaInvalid{0};      // quarantined at open (stale/torn)
  std::atomic<uint64_t> replicaCrcFailures{0};  // overlay page CRC mismatches
  std::atomic<uint64_t> replicaPunchedBytes{0}; // v1 bytes reclaimed by punch
  std::atomic<uint64_t> replicaOrphansSwept{0}; // orphaned replica files removed
  // Workflow observability counters. Serve side:
  std::atomic<uint64_t> filesOpened{0};       // DISTINCT keys opened this process
  std::atomic<uint64_t> ramHitBytes{0};       // hit bytes served from staged RAM
  std::atomic<uint64_t> firstTouchBytes{0};   // byte-tier bytes served for the FIRST time
                                              // within an entry's in-process lifetime —
                                              // served/first_touch = within-open re-read
                                              // factor; reopen loops show in opens/file
  std::atomic<uint64_t> hitDiskReads{0};      // physical .data preads on the hit path
                                              // (one per COALESCED run of resident
                                              // pages, not one per page)
  std::atomic<uint64_t> hitDiskBytes{0};
  std::atomic<uint64_t> hitDiskSeq{0};        // preads starting at the previous pread's end
  std::atomic<uint64_t> replicaBytesServed{0}; // stitched bytes served from .tdata
  std::atomic<uint64_t> replicaReads{0};      // physical .tdata preads (one per
                                              // coalesced run of overlay pages, so
                                              // re-reads of a page count again — the
                                              // byte tier's hitDiskReads analogue)
  std::atomic<uint64_t> replicaReadBytes{0};  // bytes those preads moved (>= served:
                                              // whole overlay pages are checksummed)
  std::atomic<uint64_t> relayBytes{0};        // pass-through serves (cache never touched)
  std::atomic<uint64_t> readvChunks{0};       // chunks across all vector reads
  std::atomic<uint64_t> readvCalls{0};        // vector reads the cache handled
                                              // (chunks/calls = batch width)
  std::atomic<uint64_t> readvMixed{0};        // vector reads containing hits AND misses
  // Write side:
  std::atomic<uint64_t> flushRuns{0};         // coalesced pwrite runs
  std::atomic<uint64_t> flushRunBytes{0};
  std::atomic<uint64_t> bufferStalls{0};      // cap-triggered synchronous drains
  std::atomic<uint64_t> bufferStallUs{0};     // wall time writers spent inside them
  // Most cache-engaged handles open at once during this process. The origin's
  // delivery is a non-monotone function of concurrency, so a measurement taken
  // at one width says little about another: runs are only pooled across time
  // when this agrees, and a change in it is what re-arms measurement.
  std::atomic<uint64_t> handlesHighWater{0};
  // Most threads alive in this process at any sampled moment. A run's wall is
  // only interpretable against the width it ran at, and CPU time divided by
  // wall x this is the share of the machine that was actually computing.
  std::atomic<uint64_t> threadsHighWater{0};
  // Reads outstanding AT THE ORIGIN, and the most there ever were.
  //
  // This is the concurrency the origin sees, and its delivery rate is a
  // function of it -- one measured site gives 147, 1868 and 632 IOPS at 1, 16
  // and 64 concurrent streams, so not even monotone. Two runs at the same
  // value put the same load on the origin; two at different values are not
  // comparable, whatever their thread counts say. Counted where a read is
  // ISSUED to the origin and released at the TOP of its completion handler,
  // before the caller's own handler runs, so it spans the read rather than the
  // call that started it. Releasing at destruction instead would hold it
  // across the caller's handler, and a caller that starts its next read from
  // in there -- the shape that creates real concurrency -- would be counted as
  // two reads where it had one.
  //
  // A warm run reads 0, correctly: nothing was outstanding at the origin
  // because nothing went there.
  std::atomic<uint64_t> originReadsInFlight{0};
  std::atomic<uint64_t> originReadsInFlightHighWater{0};
  // SUPERSEDED, kept only so the old name is not silently reused for the new
  // meaning: this counted reads being HANDED OFF at once.
  //
  // DEAD. Nothing increments either of these, anywhere, so both are emitted as
  // a permanent zero. They are kept only so the old NAME is not silently
  // reused for the new meaning, and emitted only so a consumer parsing the
  // record does not lose a key it has always seen.
  //
  // What it used to count was reads being SET UP at once, which is not what
  // sets the origin's delivery rate: every route dispatches and returns while
  // a synchronous caller blocks above this library, so it spanned the handoff
  // rather than the read, and a job with thirty-two reads genuinely in flight
  // could report a handful. Measuring the real thing meant counting down in
  // the completion handlers on every route -- a change to the read path, which
  // was then made. The counter above is that measurement; use it.
  //
  // An earlier version of this comment described the change as still to come,
  // and stayed that way after it had been made.
  std::atomic<uint64_t> readsInFlight{0};
  std::atomic<uint64_t> readsInFlightHighWater{0};
  Histogram hitReadUs;
  Histogram missReadUs;
  Histogram originRtUs;
  Histogram openUs;        // open-to-completion for cache-engaged handles
  Histogram replicaReadUs; // stitched serve spans
  Histogram flushWriteUs;  // per coalesced flush pwrite
  Histogram metaFlushUs;   // sidecar store spans
  // Size histograms (log2 BYTES, same bucketing as the µs ones). The pair
  // that makes read shape visible: what the client asks for vs what the
  // cache disk is actually asked for.
  Histogram reqReadBytes;     // per request arriving at the plugin (Read, readv chunk)
  Histogram hitReadSize;      // per byte-tier pread
  Histogram replicaReadSize;  // per replica-tier pread

  // Set once at store init when `trace = io`; null = off.
  // Not dumped; carried here so every path that has Stats& can trace.
  Tracer* tracer = nullptr;

  // JSON object body (no braces): `"opens":1,...,"hist_hit_read_us":[...]`.
  // CacheStore composes the full stats line (adds ts/host/pid/entries).
  std::string toJsonBody() const;
};

// Cumulative counters summed across process stats files, for the CLI
// `stats`/`status`. Counters are monotonic cumulative snapshots, so the LAST
// line of each file supersedes earlier ones; totals sum the last line per file.
struct StatsTotals {
  int files = 0;
  uint64_t opens = 0, validationsFailed = 0, hitBytes = 0, missBytes = 0, originBytes = 0,
           servedBytes = 0, originReads = 0, fetchesJoined = 0, originReadvs = 0, pageWrites = 0, crcFailures = 0,
           metaCorrupt = 0, evictedEntries = 0, evictedBytes = 0, failopenEvents = 0,
           admissionsBypassed = 0,
           openRetries = 0, openRetriesExhausted = 0,
           replicaOpens = 0, replicaPublished = 0, replicaInvalid = 0, replicaCrcFailures = 0,
           replicaPunchedBytes = 0, replicaOrphansSwept = 0;
  // Workflow counters:
  uint64_t handlesHighWater = 0, threadsHighWater = 0, readsInFlightHighWater = 0,
           originReadsInFlightHighWater = 0;
  uint64_t filesOpened = 0, ramHitBytes = 0, firstTouchBytes = 0, hitDiskReads = 0,
           hitDiskBytes = 0, hitDiskSeq = 0, replicaBytesServed = 0, replicaReads = 0,
           replicaReadBytes = 0, relayBytes = 0,
           readvChunks = 0, readvCalls = 0, readvMixed = 0, flushRuns = 0, flushRunBytes = 0,
           bufferStalls = 0, bufferStallUs = 0;
  // Histogram bucket sums (log2 µs), for CLI percentile rendering:
  std::vector<uint64_t> histHitRead, histOriginRt, histOpen, histReplicaRead, histFlushWrite;
  // … and log2 bytes, for the read-shape block:
  std::vector<uint64_t> histReqRead, histHitReadSize, histReplicaReadSize;
  // True when the summed files do NOT all use the same counter vocabulary —
  // i.e. some were written before reads were coalesced (hit_disk_reads counted
  // PAGES) and some after (it counts RUNS). Summing those is meaningless, so
  // derived per-read figures must be suppressed rather than printed.
  bool schemaMixed = false;
};
// Sums the last JSON line of every <statsDir>/*.jsonl. Missing dir => zeros.
StatsTotals aggregateStats(const std::string& statsDir);

} // namespace ucache
