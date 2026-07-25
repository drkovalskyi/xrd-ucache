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

// log2-bucketed histogram of microsecond durations: bucket i holds samples
// with floor(log2(us)) == i (bucket 0: us <= 1); 40 buckets cover ~13 days.
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
  std::atomic<uint64_t> hitDiskBytes{0};
  std::atomic<uint64_t> hitDiskSeq{0};        // preads starting at the previous pread's end
  std::atomic<uint64_t> replicaBytesServed{0}; // stitched bytes served from .tdata
  std::atomic<uint64_t> replicaReads{0};      // physical .tdata preads (one per
                                              // overlay page touched, so re-reads
                                              // of a page count again — the byte
                                              // tier's hitDiskReads analogue)
  std::atomic<uint64_t> relayBytes{0};        // pass-through serves (cache never touched)
  std::atomic<uint64_t> readvChunks{0};       // chunks across all vector reads
  std::atomic<uint64_t> readvMixed{0};        // vector reads containing hits AND misses
  // Write side:
  std::atomic<uint64_t> flushRuns{0};         // coalesced pwrite runs
  std::atomic<uint64_t> flushRunBytes{0};
  std::atomic<uint64_t> bufferStalls{0};      // cap-triggered synchronous drains
  std::atomic<uint64_t> bufferStallUs{0};     // wall time writers spent inside them
  Histogram hitReadUs;
  Histogram missReadUs;
  Histogram originRtUs;
  Histogram openUs;        // open-to-completion for cache-engaged handles
  Histogram replicaReadUs; // stitched serve spans
  Histogram flushWriteUs;  // per coalesced flush pwrite
  Histogram metaFlushUs;   // sidecar store spans

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
           openRetries = 0, openRetriesExhausted = 0,
           replicaOpens = 0, replicaPublished = 0, replicaInvalid = 0, replicaCrcFailures = 0,
           replicaPunchedBytes = 0, replicaOrphansSwept = 0;
  // Workflow counters:
  uint64_t filesOpened = 0, ramHitBytes = 0, firstTouchBytes = 0, hitDiskReads = 0,
           hitDiskBytes = 0, hitDiskSeq = 0, replicaBytesServed = 0, replicaReads = 0,
           relayBytes = 0,
           readvChunks = 0, readvMixed = 0, flushRuns = 0, flushRunBytes = 0,
           bufferStalls = 0, bufferStallUs = 0;
  // Histogram bucket sums (log2 µs), for CLI percentile rendering:
  std::vector<uint64_t> histHitRead, histOriginRt, histOpen, histReplicaRead, histFlushWrite;
};
// Sums the last JSON line of every <statsDir>/*.jsonl. Missing dir => zeros.
StatsTotals aggregateStats(const std::string& statsDir);

} // namespace ucache
