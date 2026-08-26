// Runtime configuration. Three-level settings model:
//   built-in defaults
//     < ucache keys in the XrdCl plugin conf   (the user's DEFAULTS — the one
//                                               hand-edited file)
//     < <cacheDir>/state                       (CURRENT values, written only by
//                                               `ucache set/unset` — never by hand)
//     < UCACHE_* environment                   (per-invocation override)
// The legacy ~/.config/ucache/config.toml layer is retired.
// The default page size is 4 KiB (a 16 KiB provisional failed the
// read-amplification gate on real NanoAOD traces).
//
// Thread-safety: immutable after construction; safe to share by const ref.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ucache {

enum class ValidateMode { kNone, kSize, kSizeMtime, kCksum };
enum class FsyncMode { kOff, kData, kAll };

struct Config {
  std::string cacheDir;                   // UCACHE_DIR / `dir =`; NO default:
                                          // empty => plugin passes through (warn),
                                          // doctor FAILs, CLI commands refuse
  uint32_t pageSize = 4096;               // UCACHE_PAGE_SIZE, new entries only
  // Growth is bounded by a free-disk FLOOR by default: the cache uses
  // the disk and LRU-evicts to keep `minFreeBytes` free, so an active session
  // fills most of the disk and reclaims under pressure. maxBytes is an OPTIONAL
  // hard cache-size cap for users who want a fixed size instead.
  uint64_t maxBytes = 0;   // UCACHE_MAX_BYTES; optional hard cap. 0 = no byte cap
                           // (default): the free-disk floor governs growth.
  bool budgetAuto = false; // (fromEnv) true iff UCACHE_MAX_BYTES is unset =>
                           // CacheStore auto-sets minFreeBytes from the disk.
  uint64_t minFreeBytes = 0; // UCACHE_MIN_FREE_BYTES; evict (LRU) whenever free
                             // disk drops below this — the default limit and the
                             // accurate cross-process guard (sees the shared dir).
                             // 0 = auto (min(50 GiB, 10% of total), clamped to
                             // <= half of free-at-init) when budgetAuto, else off.
  int evictCheckSeconds = 10; // UCACHE_EVICT_CHECK_S; eviction-check rate limit (0 = always)
  // UCACHE_EVICT_PROTECT_S. An entry is not an eviction candidate while its last
  // read is this recent, so a running analysis cannot evict its own working set
  // — LRU is pessimal for a cyclic scan: with a cache smaller than the set, the
  // victim it picks is exactly the entry wanted next, and the hit rate collapses
  // toward zero instead of the C/W a policy that simply held still would get.
  // Data untouched for longer than this — an earlier, finished study — stays the
  // first thing given up, which is the point.
  // When nothing is eligible, growth stops by DECLINING NEW ENTRIES rather than
  // evicting a peer (see CacheStore::admissionBlocked). 0 = off, i.e. plain LRU.
  uint32_t evictProtectSeconds = 86400;
  double highWater = 0.90;                // UCACHE_HIGH_WATER (fraction of maxBytes)
  double lowWater = 0.75;                 // UCACHE_LOW_WATER
  ValidateMode validate = ValidateMode::kSize; // UCACHE_VALIDATE
  FsyncMode fsync = FsyncMode::kOff;      // UCACHE_FSYNC
  int threads = 0;                        // UCACHE_THREADS; 0 = min(8, hw)
  int maxErrors = 5;                      // UCACHE_MAX_ERRORS per handle
  int metaFlushSeconds = 30;              // UCACHE_META_FLUSH_S
  // Fill write buffering: miss-fetched pages stage
  // in RAM and flush as offset-sorted large writes; the buffer also serves
  // reads, so a fill never does random IO against the cache disk. 0 = legacy
  // immediate per-page writes. Crash window = buffered-but-unflushed pages
  // (lost cleanly; publish stays flush-then-bitmap).
  int fillBufferMb = 48;                  // UCACHE_FILL_BUFFER_MB, per entry
  int fillBufferTotalMb = 1024;           // UCACHE_FILL_BUFFER_TOTAL_MB, process-wide
  // UCACHE_REVALIDATE_S: cache-freshness window (TTL). When a usable local
  // entry was last validated against the origin within this many seconds,
  // TRUST it — skip the remote Open+Stat and serve locally (the origin is
  // touched only on a genuine cache miss, fail-open). Default 7 days:
  // HEP data is write-once and warm passes that never depend on a flaky/
  // loaded/WAN remote are the founding reliability motivation. Trade-off: a
  // file REPLACED at the origin under the same name serves stale bytes until
  // the window expires (`ucache rm <url>` forces a re-check). 0 = revalidate
  // on every open (the pre-0.9.1 default).
  int revalidateSeconds = 604800;         // UCACHE_REVALIDATE_S (7 days)
  // UCACHE_OPEN_RETRIES (opt-in reliability control):
  // additional attempts after the first for a TRANSIENT open failure (flaky/
  // loaded/WAN remote). 0 = off (default; the Open path is unchanged). Each
  // retry constructs a fresh XrdCl::File (a failed open is terminal on the
  // object) and backs off with full jitter. Read-opens only; write-opens and
  // genuine errors (NotFound/NotAuthorized/...) are never retried. Pairs with
  // UCACHE_REVALIDATE_S: retry rescues the cold fill, revalidate carries warm
  // passes with zero remote contact.
  int openRetries = 0;                    // UCACHE_OPEN_RETRIES
  int openRetryBaseMs = 200;              // UCACHE_OPEN_RETRY_BASE_MS (backoff base)
  int openRetryMaxMs = 5000;              // UCACHE_OPEN_RETRY_MAX_MS (per-attempt cap)
  bool disable = false;                   // UCACHE_DISABLE
  bool transpose = true;                  // `transpose` / UCACHE_TRANSPOSE=0/off/
                                          // false: never serve replica views
                                          // (replica-tier kill switch)
  // Background recompression: ONE switch.
  // `recompress = on` => files the user's jobs actually read are queued at
  // close and transcoded by a detached background drainer, no questions asked
  // (off by default — opt-in CPU/disk; the user flipping the switch IS the
  // worth-it decision). `recompress_codecs` = which SOURCE codecs qualify
  // (static fact from basket headers). The default lists the two codecs that
  // are expensive to decode and are what stored physics data actually uses;
  // lz4 and zstd already decode cheaply, so transcoding them to ZSTD-1 would
  // spend disk to buy nothing.
  // The evidence gate (recompress_min_share) is PARKED: .cost/
  // calibration evidence is still collected, but no longer gates builds.
  // Serving already-built replicas is governed by `transpose`, not by these.
  bool recompress = false;                       // UCACHE_RECOMPRESS
  std::vector<std::string> recompressCodecs{"lzma", "zlib"}; // UCACHE_RECOMPRESS_CODECS
  // `recompress_reclaim`: what to punch from the v1 byte cache once
  // a valid replica exists. kSuperseded (default) frees only the ranges the
  // overlay relocated; kFull drops the entry's ENTIRE byte copy — the replica
  // becomes the durable form, and anything it does not cover (prefetch
  // margin, partial branches, header/streamers) refetches from origin on
  // demand (fail-open). For space-tight replica-primary setups.
  enum class Reclaim { kSuperseded, kFull };
  Reclaim recompressReclaim = Reclaim::kSuperseded; // UCACHE_RECOMPRESS_RECLAIM
  // Background transcode jobs during a fill pass. 0 = auto, which is
  // min(cores/2, threads/2) of the process doing the reading; a fixed small
  // number reached only 8-18% coverage on a large dataset, and the right share
  // depends on how much of the machine the analysis is actually using.
  int recompressDrainJobs = 0;                   // UCACHE_RECOMPRESS_DRAIN_JOBS
  // `trace = io` writes a sampled per-operation JSON trace
  // next to the process's stats file; `trace_sample = N` records every Nth
  // read-class op (1 = everything). Off ("") by default — zero cost.
  std::string trace;    // UCACHE_TRACE ("io" | "off"/"")
  int traceSample = 64; // UCACHE_TRACE_SAMPLE
  // Measurement holdout, in PER MILLE of files (0 = off, the default).
  // A selected file is served pure pass-through — origin only, cache
  // untouched — so its wall span measures what the origin costs for real work
  // under this run's own conditions. Comparing those spans against cached
  // files' spans, within the same run, is the only device that survives a
  // thread-count change or a faster analysis loop: nothing older than the run
  // enters the comparison. The price is p*(gain-1) of the wall, so it is FREE
  // exactly when the cache is buying nothing — which is when the truth matters
  // most. Per mille, not percent, because a trickle (1-5 permille) is the
  // steady-state setting once a measurement exists.
  int measurePermille = 0; // UCACHE_MEASURE_PERMILLE
  // Rotates which files are held out, so coverage accumulates over time
  // instead of punishing the same files forever. 0 = never rotate.
  uint64_t measureRotateSeconds = 86400; // UCACHE_MEASURE_ROTATE_SECONDS
  std::vector<std::string> keepCgi;       // UCACHE_KEEP_CGI comma list
  std::vector<std::string> allowHosts;    // UCACHE_ALLOW glob list (empty = all)
  std::vector<std::string> denyHosts;     // UCACHE_DENY glob list

  // Per-key provenance for `ucache settings` / doctor: "conf", "state" or
  // "env" for keys that were explicitly set; absent = built-in default.
  std::map<std::string, std::string> sources;

  // Loads the full stack: defaults -> plugin conf -> <cacheDir>/state -> env.
  static Config fromEnv();
  // As above, with the plugin conf supplied as the key/value map XrdCl hands
  // to XrdClGetPlugIn (url/lib/enable are XrdCl's and ignored here).
  static Config fromEnv(const std::map<std::string, std::string>* pluginConf);
  // As above, with the plugin conf read from a file (CLI: the same conf file
  // XrdCl would process, discovered via the standard search order).
  static Config fromEnv(const std::string& pluginConfPath);

  // The settings vocabulary: canonical key name + its UCACHE_* env twin.
  struct KeyInfo {
    const char* key;
    const char* envName;
  };
  static const std::vector<KeyInfo>& knownKeys();
  // Keys `ucache set` may write into the state file: any known key except
  // `dir` (the state file lives INSIDE the cache dir — bootstrap order).
  static bool stateSettable(const std::string& key);
  // Serialized current value of a known key (for `ucache settings`).
  std::string valueOf(const std::string& key) const;
  // The CLI-managed current-values file.
  static std::string statePath(const std::string& cacheDir) { return cacheDir + "/state"; }

  // page size must be a power of two in [4 KiB, 1 MiB].
  static bool validPageSize(uint32_t p) {
    return p >= 4096 && p <= (1u << 20) && (p & (p - 1)) == 0;
  }
};

} // namespace ucache
