// ucache — command-line tool for the per-user XrdCl read cache.
// Zero runtime Python; links libucache-core only. Config via Config::fromEnv()
// with the settings layering: conf defaults < state (CLI-set current values,
// `ucache set/unset/settings`) < UCACHE_* env.
#include "CacheStore.h"
#include "Config.h"
#include "HelperPath.h"
#include "IOBackend.h"
#include "MetaFile.h"
#include "ReplicaStore.h"
#include "RunLog.h"
#include "DiskBench.h"
#include "UrlKey.h"
#ifdef UCACHE_HAVE_TRANSPOSE
#include "CacheSource.h"
#include "RNTupleRewrite.h"
#include "Transposer.h"
namespace tp = ucache::transpose;
#endif

#include <fcntl.h>
#include <optional>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <limits.h>
#include <mach-o/dyld.h> // _NSGetExecutablePath: this platform's /proc/self/exe
#endif
#include <atomic>
#include <mutex>
#include <set>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <fstream>
#include <iterator>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

using namespace ucache;

namespace {

void usage() {
  std::fputs(
      "usage: ucache <command> [args]\n"
      "  --version | -V    print the version and build id, and exit\n"
      "  setup [--host H] [--dir PATH]  write the single conf file (activation +\n"
      "                    settings, cache dir explicit) to ~/.xrootd/client.plugins.d\n"
      "  doctor            check install, filesystem, and activation\n"
      "  test <url>        end-to-end self-test of YOUR setup: cold + warm whole-file\n"
      "                    read via xrdcp; warm must be origin-free; cleans up the\n"
      "                    entry it created (a pre-existing entry is kept)\n"
      "  enable | disable  turn caching on/off (flips the plugin conf)\n"
      "  summary [--detail]  overall performance and what the cache has saved you;\n"
      "                    --detail adds the last run: tiers, read sizes, health\n"
      "  history [--top N] one row per run, newest first — whether the numbers\n"
      "                    are holding up across runs, versions and machines\n"
      "  status            cache location, budget, usage, and aggregated stats\n"
      "  bench [PATH ...] [--size SZ] [--measurement-duration S] [--threads N]\n"
      "                    [--block KB] [--fill k=v,...] [--cache-path]\n"
      "                    [--cache-sample SZ] [--log FILE | --no-log]\n"
      "                    measure a cache location's raw storage performance.\n"
      "                    STANDARD measurements use pinned block sizes and\n"
      "                    queue depths, so they compare to a datasheet or to\n"
      "                    another machine; PATTERN measurements use the shapes\n"
      "                    uCache generates, at --threads concurrency, so they\n"
      "                    predict what a job gets here. --threads is REQUIRED\n"
      "                    and never guessed: it is the concurrency your\n"
      "                    analyses run at, which is not the core count unless\n"
      "                    they happen to match.\n"
      "                    -S is per MEASUREMENT; the run is measurements x S\n"
      "                    plus building the test file. The plan and the total\n"
      "                    print before anything runs. Default location: the\n"
      "                    configured cache dir; several PATHs print a\n"
      "                    comparison. Every run appends its numbers plus the\n"
      "                    machine, load and block device behind PATH to\n"
      "                    ./ucache-bench.txt\n"
      "  netbench <root://url> [--streams 1,4,16] [--block KB] [--seconds S]\n"
      "                    measure the ORIGIN's random-read rates from this\n"
      "                    machine — the numbers a cache location must beat\n"
      "  ls [--sort age|size]  list cached entries (size, cached, coverage,\n"
      "                    last-used age, replica, pinned); default sort = size\n"
      "  stats [--reset | --files [--top N]]\n"
      "                    aggregate stats/*.jsonl across all processes, plus the\n"
      "                    derived workflow picture (tiers, opens/file, re-read\n"
      "                    factor, seq%%, latency percentiles). --files: per-file\n"
      "                    records, costliest first. --reset deletes the stats\n"
      "                    window (fresh measurement; warns if a job looks live)\n"
      "  evict [--older-than DUR | --newer-than DUR | --to-size SIZE] [--dry-run]\n"
      "                    no flags: one eviction pass to the configured budget;\n"
      "                    --older-than 30d: drop entries unused for that long;\n"
      "                    --newer-than 1h: drop entries used within the window\n"
      "                    (undo a polluting run);\n"
      "                    --to-size 20g: LRU-evict down to that total size\n"
      "  rm <url> [url...] remove specific entries (byte cache + replica)\n"
      "  clear [--yes] [--keep-pinned]  empty the whole cache (prompts unless --yes)\n"
      "  pin   <url>       protect an entry from eviction\n"
      "  unpin <url>       remove pin protection\n"
      "  verify <url>      CRC-scrub an entry (detect + invalidate bad pages)\n"
      "  branches <url>    which branches your analysis read (fully-cached\n"
      "                    branches, bytes, source codec, and the summary share)\n"
      "  recompress [--jobs N] [--yes] [--strict]\n"
      "                                 transcode the cached files whose source codec is in\n"
      "                    recompress_codecs (default lzma,zlib), foreground with live\n"
      "                    progress (default jobs: half the CPU cores). For automatic\n"
      "                    background builds of everything your jobs read, turn the\n"
      "                    switch on instead: `ucache set recompress on`. With\n"
      "                    `recompress_reclaim = full` the sweep also drops each\n"
      "                    replicated entry's v1 byte copy (space back right away;\n"
      "                    uncovered reads refetch from origin on demand)\n"
      "  settings          every setting: effective value + where it comes from\n"
      "                    (default | conf | state | env)\n"
      "  set <key> <value> set a CURRENT value (state file in the cache dir) —\n"
      "                    your defaults in ucache.conf are never touched\n"
      "  unset <key>       drop a current value (back to your defaults)\n"
      "\n"
      "developer plumbing (gates/debugging):\n"
      "  materialize <url> [--branches a,b] [--punch] [--from-file F --overlay-out P]\n"
      "  untranspose <url> drop an entry's replica (keeps the byte cache)\n"
      "\n"
      "config: defaults = `key = value` lines in ucache.conf, edited by hand\n"
      "(USER_GUIDE §2) < current values = `ucache set` < UCACHE_* env (one job)\n",
      stderr);
}

std::string human(uint64_t b) {
  // Binary divisors => binary labels. The old "GB" label for 1024^3 made the
  // author double-check a 939 GB dataset that status showed as "854.7 GB".
  const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(b);
  int i = 0;
  while (v >= 1024.0 && i < 4) {
    v /= 1024.0;
    ++i;
  }
  char out[32];
  std::snprintf(out, sizeof out, i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
  return out;
}

// One-line summary of the freshness window — the setting that decides whether
// warm opens contact the origin at all, so `status` and `doctor` display it.
std::string freshnessSummary(const Config& cfg);

// Compact relative age ("3d", "5h", "12m", "8s", "2w") since `atime`.
std::string humanAge(uint64_t atime, uint64_t now) {
  if (atime == 0)
    return "-";
  uint64_t d = now > atime ? now - atime : 0;
  char out[16];
  if (d < 60)
    std::snprintf(out, sizeof out, "%llus", (unsigned long long)d);
  else if (d < 3600)
    std::snprintf(out, sizeof out, "%llum", (unsigned long long)(d / 60));
  else if (d < 86400)
    std::snprintf(out, sizeof out, "%lluh", (unsigned long long)(d / 3600));
  else if (d < 86400 * 7)
    std::snprintf(out, sizeof out, "%llud", (unsigned long long)(d / 86400));
  else
    std::snprintf(out, sizeof out, "%lluw", (unsigned long long)(d / 604800));
  return out;
}

std::string freshnessSummary(const Config& cfg) {
  if (cfg.revalidateSeconds <= 0)
    return "off — every open re-checks the origin (revalidate_seconds=0)";
  uint64_t now = static_cast<uint64_t>(::time(nullptr));
  uint64_t w = static_cast<uint64_t>(cfg.revalidateSeconds);
  char buf[192];
  std::snprintf(buf, sizeof buf,
                "%s — entries validated within this window are served with no origin "
                "contact (revalidate_seconds=%d)",
                humanAge(now - w, now).c_str(), cfg.revalidateSeconds);
  return buf;
}

// Byte size with optional k/m/g/t suffix (e.g. "20g"). false on parse error.
bool parseSizeArg(const char* v, uint64_t& out) {
  if (v[0] < '0' || v[0] > '9') // reject empty, sign, leading space (strtoull
    return false;               // would negate '-N' into a huge value)
  char* end = nullptr;
  errno = 0;
  unsigned long long n = std::strtoull(v, &end, 10);
  if (end == v || errno)
    return false;
  uint64_t mult = 1;
  if (*end) {
    switch (*end) {
    case 'k': case 'K': mult = 1ull << 10; break;
    case 'm': case 'M': mult = 1ull << 20; break;
    case 'g': case 'G': mult = 1ull << 30; break;
    case 't': case 'T': mult = 1ull << 40; break;
    default: return false;
    }
    if (end[1])
      return false;
  }
  if (n > UINT64_MAX / mult) // n*mult would wrap (e.g. 16777216t -> 0)
    return false;
  out = n * mult;
  return true;
}

// Duration with optional s/m/h/d/w suffix (default seconds; e.g. "30d", "12h").
bool parseDurationArg(const char* v, uint64_t& secs) {
  if (v[0] < '0' || v[0] > '9') // reject empty, sign, leading space
    return false;
  char* end = nullptr;
  errno = 0;
  unsigned long long n = std::strtoull(v, &end, 10);
  if (end == v || errno)
    return false;
  uint64_t mult = 1;
  if (*end) {
    switch (*end) {
    case 's': mult = 1; break;
    case 'm': mult = 60; break;
    case 'h': mult = 3600; break;
    case 'd': mult = 86400; break;
    case 'w': mult = 604800; break;
    default: return false;
    }
    if (end[1])
      return false;
  }
  if (n > UINT64_MAX / mult)
    return false;
  secs = n * mult;
  return true;
}

// Value of a "--flag V" / "--flag=V" option at argv[i]; advances i past the
// consumed token(s). Returns nullptr if the value is missing.
const char* flagValue(int argc, char** argv, int& i, size_t flagLen) {
  const char* a = argv[i];
  if (a[flagLen] == '=')
    return a + flagLen + 1;
  if (i + 1 < argc)
    return argv[++i];
  return nullptr;
}

// Stats aggregation (last-cumulative-line per file, summed across processes)
// lives in ucache-core (Stats.h aggregateStats) so it is unit-tested there.
// Percentiles out of a log2 histogram are bucket-resolution: good to a factor
// of two, i.e. regimes rather than lab numbers. Durations are reported at the
// bucket MIDPOINT (1.5*2^i), which is the better estimator for values spread
// smoothly inside a bucket. Sizes are reported at the bucket FLOOR (2^i),
// because read sizes cluster on exact powers of two and page multiples, which
// sit at the bottom of a bucket: with the midpoint, a workload of exactly-4 KiB
// reads printed "6.0 KiB" four lines under an exactly-computed "mean 4.0 KiB".
// Returns the bucket's lower bound; scale it at the call site.
double histQuantile(const std::vector<uint64_t>& h, double q) {
  uint64_t total = 0;
  for (uint64_t c : h)
    total += c;
  if (total == 0)
    return -1.0;
  uint64_t target = static_cast<uint64_t>(q * static_cast<double>(total));
  if (target == 0)
    target = 1;
  uint64_t cum = 0;
  for (size_t i = 0; i < h.size(); ++i) {
    cum += h[i];
    if (cum >= target)
      return i == 0 ? 1.0 : static_cast<double>(1ull << i);
  }
  return -1.0;
}

std::string histPctile(const std::vector<uint64_t>& h, double q) {
  const double lo = histQuantile(h, q);
  if (lo < 0)
    return "-";
  const double us = lo <= 1.0 ? lo : 1.5 * lo; // durations: bucket midpoint
  char buf[32];
  if (us < 1000)
    std::snprintf(buf, sizeof buf, "%.0fus", us);
  else if (us < 1e6)
    std::snprintf(buf, sizeof buf, "%.1fms", us / 1e3);
  else
    std::snprintf(buf, sizeof buf, "%.1fs", us / 1e6);
  return buf;
}

std::string pctiles(const std::vector<uint64_t>& h) {
  if (h.empty())
    return "-";
  return histPctile(h, 0.50) + " / " + histPctile(h, 0.95) + " / " + histPctile(h, 0.99);
}

// Same, for the log2-BYTE histograms.
std::string pctilesB(const std::vector<uint64_t>& h) {
  if (h.empty())
    return "-";
  std::string out;
  for (double q : {0.50, 0.95, 0.99}) {
    const double v = histQuantile(h, q);
    if (!out.empty())
      out += " / ";
    out += v < 0 ? "-" : human(static_cast<uint64_t>(v));
  }
  return out;
}

void printStats(const StatsTotals& t) {
  std::printf("stats across %d process file(s):\n", t.files);
  auto row = [](const char* n, uint64_t v) {
    std::printf("  %-18s %llu\n", n, (unsigned long long)v);
  };
  auto rowB = [](const char* n, uint64_t v) {
    std::printf("  %-18s %llu (%s)\n", n, (unsigned long long)v, human(v).c_str());
  };
  row("opens", t.opens);
  rowB("hit_bytes", t.hitBytes);
  rowB("miss_bytes", t.missBytes);
  rowB("origin_bytes", t.originBytes);
  rowB("served_bytes", t.servedBytes);
  row("origin_reads", t.originReads);
  row("fetches_joined", t.fetchesJoined);
  row("origin_readvs", t.originReadvs);
  row("page_writes", t.pageWrites);
  row("crc_failures", t.crcFailures);
  row("evicted_entries", t.evictedEntries);
  rowB("evicted_bytes", t.evictedBytes);
  row("failopen_events", t.failopenEvents);
  row("admissions_bypassed", t.admissionsBypassed);
  row("open_retries", t.openRetries);
  row("open_retries_exhausted", t.openRetriesExhausted);
  row("validations_failed", t.validationsFailed);

  // The derived workflow picture — the divisions a human would do.
  std::printf("workflow:\n");
  if (t.filesOpened)
    std::printf("  files opened       %llu distinct (%.1f opens/file)\n",
                (unsigned long long)t.filesOpened,
                static_cast<double>(t.opens) / static_cast<double>(t.filesOpened));
  const uint64_t diskB = t.hitBytes > t.ramHitBytes ? t.hitBytes - t.ramHitBytes : 0;
  std::printf("  served by tier     ram %s | disk %s | replica %s | origin %s | relay %s\n",
              human(t.ramHitBytes).c_str(), human(diskB).c_str(),
              human(t.replicaBytesServed).c_str(), human(t.missBytes).c_str(),
              human(t.relayBytes).c_str());
  if (t.schemaMixed)
    std::printf("  NOTE               stats files span a version where read counters changed\n"
                "                     meaning (per page, now per coalesced run) — per-read\n"
                "                     figures below are omitted. `stats --reset` for a clean window.\n");
  if (t.hitDiskReads && !t.schemaMixed)
    std::printf("  hit disk reads     %llu (mean %s, %.0f%% sequential)\n",
                (unsigned long long)t.hitDiskReads,
                human(t.hitDiskBytes / t.hitDiskReads).c_str(),
                100.0 * static_cast<double>(t.hitDiskSeq) /
                    static_cast<double>(t.hitDiskReads));
  if (t.replicaReads && !t.schemaMixed)
    std::printf("  replica reads      %llu (mean %s)\n",
                (unsigned long long)t.replicaReads,
                human((t.replicaReadBytes ? t.replicaReadBytes : t.replicaBytesServed) /
                      t.replicaReads)
                    .c_str());
  if (t.firstTouchBytes && t.hitBytes)
    std::printf("  re-read factor     %.1fx (first touch %s of %s byte-tier serves)\n",
                static_cast<double>(t.hitBytes) / static_cast<double>(t.firstTouchBytes),
                human(t.firstTouchBytes).c_str(), human(t.hitBytes).c_str());
  if (t.readvChunks) {
    std::printf("  vector reads       %llu chunks, %llu mixed hit+miss vectors\n",
                (unsigned long long)t.readvChunks, (unsigned long long)t.readvMixed);
    if (t.readvCalls && !t.schemaMixed)
      std::printf("  vector width       %llu calls (mean %.1f chunks/call)\n",
                  (unsigned long long)t.readvCalls,
                  static_cast<double>(t.readvChunks) / static_cast<double>(t.readvCalls));
  }
  if (t.flushRuns)
    std::printf("  fill flushes       %llu runs (mean %s)\n",
                (unsigned long long)t.flushRuns,
                human(t.flushRunBytes / t.flushRuns).c_str());
  if (t.bufferStalls)
    std::printf("  fill stalls        %llu (%.1f s waiting on the cache disk)\n",
                (unsigned long long)t.bufferStalls,
                static_cast<double>(t.bufferStallUs) / 1e6);
  if (!t.histReqRead.empty() || !t.histHitReadSize.empty() ||
      !t.histReplicaReadSize.empty()) {
    // Read shape: what the client asked for, next to what the cache disk was
    // asked for. The second should be >= the first, never a fraction of it.
    std::printf("read size p50/p95/p99 (log2 buckets, floor):\n");
    if (!t.histReqRead.empty())
      std::printf("  requested          %s\n", pctilesB(t.histReqRead).c_str());
    if (!t.histHitReadSize.empty())
      std::printf("  byte-tier pread    %s\n", pctilesB(t.histHitReadSize).c_str());
    if (!t.histReplicaReadSize.empty())
      std::printf("  replica pread      %s\n", pctilesB(t.histReplicaReadSize).c_str());
  }
  std::printf("latency p50/p95/p99:\n");
  std::printf("  hit read           %s\n", pctiles(t.histHitRead).c_str());
  std::printf("  origin rt          %s\n", pctiles(t.histOriginRt).c_str());
  if (!t.histReplicaRead.empty())
    std::printf("  replica read       %s\n", pctiles(t.histReplicaRead).c_str());
  if (!t.histOpen.empty())
    std::printf("  entry setup        %s\n", pctiles(t.histOpen).c_str());
  if (!t.histFlushWrite.empty())
    std::printf("  flush write        %s\n", pctiles(t.histFlushWrite).c_str());
}

// Per-file record renderer: aggregate stats/*.files.jsonl per key — the
// per-file view that makes two workflows comparable without guessing.
int cmdStatsFiles(const Config& cfg, size_t topN) {
  auto fieldU64 = [](const std::string& line, const char* key) -> uint64_t {
    std::string needle = std::string("\"") + key + "\":";
    auto p = line.find(needle);
    if (p == std::string::npos)
      return 0;
    p += needle.size();
    uint64_t v = 0;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9')
      v = v * 10 + static_cast<uint64_t>(line[p++] - '0');
    return v;
  };
  struct Agg {
    uint64_t opens = 0, served = 0, ram = 0, replica = 0, diskReads = 0, diskSeq = 0,
             firstTouch = 0, wire = 0;
  };
  std::map<std::string, Agg> byKey;
  const std::string sdir = cfg.cacheDir + "/stats";
  DIR* d = ::opendir(sdir.c_str());
  if (d) {
    while (dirent* e = ::readdir(d)) {
      std::string n = e->d_name;
      if (n.size() < 12 || n.compare(n.size() - 12, 12, ".files.jsonl") != 0)
        continue;
      std::ifstream in(sdir + "/" + n);
      std::string line;
      while (std::getline(in, line)) {
        auto kp = line.find("\"key\":\"");
        if (kp == std::string::npos)
          continue;
        kp += 7;
        auto ke = line.find('"', kp);
        if (ke == std::string::npos)
          continue;
        Agg& a = byKey[line.substr(kp, ke - kp)];
        a.opens += fieldU64(line, "opens");
        a.served += fieldU64(line, "served_bytes");
        a.ram += fieldU64(line, "ram_bytes");
        a.replica += fieldU64(line, "replica_bytes");
        a.diskReads += fieldU64(line, "disk_reads");
        a.diskSeq += fieldU64(line, "disk_seq");
        a.firstTouch += fieldU64(line, "first_touch_bytes");
        a.wire += fieldU64(line, "wire_bytes");
      }
    }
    ::closedir(d);
  }
  if (byKey.empty()) {
    std::puts("no per-file records yet — they are written when a process closes its "
              "last handle on an entry (plugin runs only, not CLI invocations)");
    return 0;
  }
  std::vector<std::pair<std::string, Agg>> rows(byKey.begin(), byKey.end());
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second.served > b.second.served; });
  std::printf("per-file records: %zu file(s), top %zu by served bytes\n", rows.size(),
              std::min(topN, rows.size()));
  std::printf("  %-10s %-6s %-9s %-5s %-7s %-10s %-10s %s\n", "SERVED", "OPENS", "DISKRD",
              "SEQ%", "REREAD", "REPLICA", "FROMWIRE", "FILE");
  size_t shown = 0;
  Agg rest;
  uint64_t restServed = 0;
  size_t restN = 0;
  for (const auto& [key, a] : rows) {
    if (shown < topN) {
      char seq[8] = "-";
      if (a.diskReads)
        std::snprintf(seq, sizeof seq, "%.0f%%",
                      100.0 * static_cast<double>(a.diskSeq) /
                          static_cast<double>(a.diskReads));
      char rr[12] = "-";
      if (a.firstTouch)
        std::snprintf(rr, sizeof rr, "%.1fx",
                      static_cast<double>(a.served) / static_cast<double>(a.firstTouch));
      std::string tail = key.size() > 58 ? "…" + key.substr(key.size() - 57) : key;
      std::printf("  %-10s %-6llu %-9llu %-5s %-7s %-10s %-10s %s\n",
                  human(a.served).c_str(), (unsigned long long)a.opens,
                  (unsigned long long)a.diskReads, seq, rr, human(a.replica).c_str(),
                  human(a.wire).c_str(), tail.c_str());
      ++shown;
    } else {
      restServed += a.served;
      rest.diskReads += a.diskReads;
      ++restN;
    }
  }
  if (restN)
    std::printf("  (+ %zu more file(s): %s served, %llu disk reads)\n", restN,
                human(restServed).c_str(), (unsigned long long)rest.diskReads);
  return 0;
}

// Absolute path of the running binary, or empty when it cannot be determined.
// Used to find things installed beside us — the netbench helper, the plugin
// library — so a build tree and an install tree both work without configuration.
// Empty is a normal answer, not an error: every caller falls back to a search.
std::string selfExePath() {
  char buf[4096];
#if defined(__APPLE__)
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0)
    return {};
  // Unlike the symlink other platforms expose, what the loader returns here is
  // whatever argv[0] resolved to: it may be relative and may contain symlinks,
  // and both break "look next to me".
  char resolved[PATH_MAX];
  if (::realpath(buf, resolved))
    return resolved;
  return buf;
#else
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
#endif
}

// `ucache netbench`: thin front for the ucache-netbench
// helper — the origin-baseline companion of `bench`. A separate binary
// because it needs XrdCl, which this CLI deliberately does not link (same
// spawn pattern as the recompress drainer); exec keeps stdio and exit code.
int cmdNetbench(int argc, char** argv) {
  std::vector<std::string> cands;
  if (const char* v = ::getenv("UCACHE_NETBENCH"))
    cands.push_back(v);
  if (std::string exe = selfExePath(); !exe.empty()) {
    auto slash = exe.rfind('/');
    std::string bindir = slash == std::string::npos ? "." : exe.substr(0, slash);
    cands.push_back(bindir + "/ucache-netbench");          // install tree (bin/)
    cands.push_back(bindir + "/../plugin/ucache-netbench"); // build tree
  }
  struct ::stat st;
  for (const auto& c : cands)
    if (::stat(c.c_str(), &st) == 0) {
      std::vector<char*> args;
      args.push_back(const_cast<char*>(c.c_str()));
      for (int i = 2; i < argc; ++i)
        args.push_back(argv[i]);
      args.push_back(nullptr);
      ::execv(c.c_str(), args.data());
      std::fprintf(stderr, "netbench: exec %s failed: %s\n", c.c_str(),
                   std::strerror(errno));
      return 1;
    }
  std::fputs("netbench: ucache-netbench helper not found — it ships next to the\n"
             "plugin and needs an XrdCl installation (see USER_GUIDE §1); on a\n"
             "machine without xrootd only the disk-side `ucache bench` runs\n",
             stderr);
  return 2;
}

// `ucache bench`: raw storage numbers for the cache dir or any
// candidate dirs. No store, no verdicts — measurement only.
int cmdBench(const Config& cfg, int argc, char** argv) {
  DiskBenchOpts opts;
  std::vector<std::string> paths;
  for (int i = 1; i < argc; ++i) { // argv[0] is the program: record the rest
    if (!opts.cmdline.empty())
      opts.cmdline += ' ';
    else
      opts.cmdline = "ucache ";
    opts.cmdline += argv[i];
  }
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--size" || a.rfind("--size=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 6);
      if (!v || !parseSizeArg(v, opts.fileBytes)) {
        std::fputs("bench: --size needs a size (e.g. 512m, 2g)\n", stderr);
        return 2;
      }
    } else if (a == "--measurement-duration" || a.rfind("--measurement-duration=", 0) == 0 ||
               a == "--phase-seconds" || a.rfind("--phase-seconds=", 0) == 0 ||
               a == "--seconds" || a.rfind("--seconds=", 0) == 0) {
      // One measurement runs for this long; the run is measurements x this,
      // plus building the test file. The two older spellings stay as aliases.
      size_t len = a.rfind("--measurement-duration", 0) == 0 ? 22
                   : a.rfind("--phase-seconds", 0) == 0     ? 15
                                                            : 9;
      const char* v = flagValue(argc, argv, i, len);
      char* end = nullptr;
      double s = v ? std::strtod(v, &end) : 0;
      if (!v || end == v || s <= 0 || s > 300) {
        std::fputs("bench: --measurement-duration needs a number in (0, 300]\n", stderr);
        return 2;
      }
      opts.measurementSeconds = s;
    } else if (a == "--threads" || a.rfind("--threads=", 0) == 0) {
      // Job concurrency for the PATTERN measurements only; the standard ones
      // are pinned so they stay comparable across machines.
      const char* v = flagValue(argc, argv, i, 9);
      char* end = nullptr;
      long n = v ? std::strtol(v, &end, 10) : 0;
      if (!v || end == v || n < 1 || n > 1024) {
        std::fputs("bench: --threads needs a count in [1, 1024]; it is required and never "
                   "guessed\n",
                   stderr);
        return 2;
      }
      opts.threads = static_cast<int>(n);
    } else if (a == "--block" || a.rfind("--block=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 7);
      char* end = nullptr;
      unsigned long long kb = v ? std::strtoull(v, &end, 10) : 0;
      if (!v || end == v || kb < 4 || kb > 65536 || kb % 4 != 0) {
        std::fputs("bench: --block needs KiB in [4, 65536], a multiple of 4\n", stderr);
        return 2;
      }
      opts.blockBytes = kb * 1024;
    } else if (a == "--fill" || a.rfind("--fill=", 0) == 0) {
      // One flag, keyword list, so a later parameter does not add another
      // flag: --fill writers=4,block=48k
      const char* v = flagValue(argc, argv, i, 6);
      bool bad = !v || !*v;
      for (const char* p = v; !bad && *p;) {
        const char* eq = std::strchr(p, '=');
        if (!eq) {
          bad = true;
          break;
        }
        std::string key(p, static_cast<size_t>(eq - p));
        const char* val = eq + 1;
        uint64_t num = 0;
        if (key == "writers") {
          char* end = nullptr;
          long w = std::strtol(val, &end, 10);
          if (end == val || w < 1 || w > 256)
            bad = true;
          else
            opts.fillWriters = static_cast<int>(w);
        } else if (key == "block") {
          if (!parseSizeArg(std::string(val, std::strcspn(val, ",")).c_str(), num) || num == 0)
            bad = true;
          else
            opts.fillBlock = num;
        } else {
          bad = true;
        }
        const char* comma = std::strchr(p, ',');
        if (!comma)
          break;
        p = comma + 1;
      }
      if (bad) {
        std::fputs("bench: --fill takes writers=N,block=SZ "
                   "(e.g. --fill writers=4,block=48k)\n"
                   "       these describe the arrival pattern the uCache-path stage generates;\n"
                   "       its VOLUME is --cache-sample (retired here: volume=)\n",
                   stderr);
        return 2;
      }
    } else if (a == "--log" || a.rfind("--log=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 5);
      if (!v || !*v) {
        std::fputs("bench: --log needs a file path (--no-log disables logging)\n", stderr);
        return 2;
      }
      opts.logPath = v;
    } else if (a == "--cache-path") {
      opts.cachePath = true;
    } else if (a == "--cache-sample" || a.rfind("--cache-sample=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 14);
      if (!v || !parseSizeArg(v, opts.cacheSample) || opts.cacheSample == 0) {
        std::fputs("bench: --cache-sample needs a size (e.g. 8g); it overrides the automatic\n"
                   "       volume, which is max(2x the kernel dirty limit, 30 s x this run's\n"
                   "       measured sequential write rate)\n",
                   stderr);
        return 2;
      }
      opts.cachePath = true; // asking for a size means asking for the stage
    } else if (a == "--sweep") {
      opts.sweep = true;
    } else if (a == "--no-log") {
      opts.logPath.clear();
    } else if (!a.empty() && a[0] == '-') {
      std::fprintf(stderr, "bench: unknown option '%s'\n", a.c_str());
      return 2;
    } else {
      paths.push_back(a);
    }
  }
  if (paths.empty()) {
    if (cfg.cacheDir.empty()) {
      std::fputs("bench: no cache dir configured and no PATH given — "
                 "`ucache bench /some/dir` tests any location\n",
                 stderr);
      return 2;
    }
    paths.push_back(cfg.cacheDir);
  }

  // --threads is REQUIRED: the pattern measurements are quoted as "at your
  // job's concurrency", and there is no honest way to supply that number on
  // the user's behalf. Inheriting it from a queue-depth list, or reading it
  // off nproc, both produced a figure nobody chose (nproc is 64 on the
  // development box while the reference analysis runs 32).
  if (opts.threads <= 0) {
    std::fputs("bench: --threads N is required — it is the concurrency your analyses run at,\n"
               "       and it is never guessed. It is NOT the core count unless they match.\n"
               "       e.g. ucache bench --size 64g --measurement-duration 60 --threads 32 PATH\n",
               stderr);
    return 2;
  }
  return runDiskBench(paths, opts);
}

// A promise of disk space that has not been written yet, released however the
// build ends. Without this every `continue` in the worker (declined by codec,
// unparseable, publish failed) would leak its claim, and a pass over content it
// declines outright would talk itself into "no space" having written nothing.
struct InFlightClaim {
  std::atomic<uint64_t>* ledger = nullptr;
  uint64_t bytes = 0;
  InFlightClaim() = default;
  InFlightClaim(std::atomic<uint64_t>* l, uint64_t b) : ledger(l), bytes(b) {}
  InFlightClaim(InFlightClaim&& o) noexcept : ledger(o.ledger), bytes(o.bytes) {
    o.ledger = nullptr;
  }
  InFlightClaim& operator=(InFlightClaim&& o) noexcept {
    if (this != &o) {
      release();
      ledger = o.ledger;
      bytes = o.bytes;
      o.ledger = nullptr;
    }
    return *this;
  }
  InFlightClaim(const InFlightClaim&) = delete;
  InFlightClaim& operator=(const InFlightClaim&) = delete;
  void release() {
    if (ledger)
      ledger->fetch_sub(bytes, std::memory_order_relaxed);
    ledger = nullptr;
  }
  ~InFlightClaim() { release(); }
};

// Upper bound on the replica an entry's resident bytes will produce: the
// measured ZSTD-1/LZMA footprint ratio. Deliberately an over-estimate — the
// cost of guessing high is a build deferred, of guessing low an eviction storm.
// Shared by the sweep's up-front pre-flight and the drainer's per-file budget so
// the two can never disagree about what "will not fit" means.
// Guarded because both callers are: a build without the codec libraries has no
// transposer, nothing to estimate for, and -Werror=unused-function turns an
// unused helper into a hard build failure. That configuration is what the
// sanitizer, coverage, crash-suite and clean-install jobs build.
#ifdef UCACHE_HAVE_TRANSPOSE
static uint64_t estimatedReplicaBytes(uint64_t cachedBytes) {
  return cachedBytes + (cachedBytes * 2) / 5; // 1.4x
}
#endif

// Free bytes above the eviction floor: what a pass may consume before LRU
// starts evicting. `~0ull` (unlimited) when eviction is disabled or the volume
// cannot be queried — absent a floor there is nothing to protect.
static uint64_t headroomToFloor(const Config& cfg, IOBackend& io) {
  // Ask the store for the floor rather than reading cfg.minFreeBytes: the
  // automatic floor is resolved into the STORE's copy of the Config, so a
  // caller holding the pre-resolution one reads 0 and would conclude there is
  // nothing to protect — silently disabling every check gated on headroom.
  const uint64_t floor = CacheStore::effectiveMinFree(cfg, io);
  if (!floor)
    return ~0ull; // eviction genuinely off: nothing to stay clear of
  uint64_t avail = 0, total = 0;
  if (io.spaceInfo(cfg.cacheDir, avail, total) != 0 || !total)
    return 0; // eviction IS on but free space is unknowable: decline rather
              // than gamble — a deferred build is retried, a storm is not.
  return avail > floor ? avail - floor : 0;
}
// Does the cache hold ANY replica? `doctor` has no cache store by design (it
// must report on a cacheDir that cannot be opened), and the question only needs
// a yes/no, so this stops at the first hit instead of counting.
bool anyReplicaExists(const std::string& cacheDir) {
  const std::string root = cacheDir + "/objects";
  DIR* d = ::opendir(root.c_str());
  if (!d)
    return false;
  bool found = false;
  while (dirent* shard = ::readdir(d)) {
    if (shard->d_name[0] == '.')
      continue;
    DIR* sd = ::opendir((root + "/" + shard->d_name).c_str());
    if (!sd)
      continue;
    while (dirent* f = ::readdir(sd)) {
      const std::string n = f->d_name;
      if (n.size() > 6 && n.compare(n.size() - 6, 6, ".tmeta") == 0) {
        found = true;
        break;
      }
    }
    ::closedir(sd);
    if (found)
      break;
  }
  ::closedir(d);
  return found;
}

// Why is `recompress = on` producing nothing?
//
// Three unrelated causes present themselves to the user identically — files
// queue at every close and no replica ever appears — and each one cost real
// time to diagnose by hand at least once. None of them needs a record of the
// last pass: all three are answerable from the live state, which is why there
// is no pass-outcome file here. Returns "" when nothing looks wrong.
//
// `deep` allows the codec comparison, which parses a cached file; `status` stays
// cheap and leaves that to `doctor`.
std::string recompressStall(const Config& cfg, IOBackend& io, size_t queued, size_t replicaN,
                            bool deep) {
  if (!cfg.recompress || replicaN > 0)
    return "";
  // Helper resolvability is asked FIRST and independently of the queue: when the
  // helper is missing the plugin deliberately queues nothing, so a check gated on
  // queue depth would go silent in precisely the state it exists to explain.
  std::string exe;
  if (!recompressHelperResolvable(exe))
    return "the `ucache` helper is not executable (" + exe +
           "), so the background worker never runs and nothing is queued — put ucache on "
           "PATH, or set UCACHE_RECOMPRESS_HELPER to its full path";
  if (queued == 0)
    return ""; // helper fine and nothing waiting: nothing to explain
  if (headroomToFloor(cfg, io) == 0)
    return "there is no headroom above the eviction floor, so background builds are "
           "declined rather than evicting your cache — free space, or set "
           "recompress_reclaim = full to replace byte copies instead of adding to them";
  if (!deep)
    return "nothing has been built yet; run `ucache doctor` for the reason";
#ifdef UCACHE_HAVE_TRANSPOSE
  // Sample from the queue itself — those ARE the entries that failed to build,
  // and reading a few lines needs no cache store, which `doctor` does not have
  // (it must work before one can be opened). If their content is in a codec the
  // policy does not list, every entry like them is being declined, and naming
  // both codecs is the whole remedy.
  std::vector<std::string> probe;
  {
    std::ifstream q(cfg.cacheDir + "/recompress.pending");
    std::string line;
    while (probe.size() < 8 && std::getline(q, line))
      if (!line.empty())
        probe.push_back(line);
  }
  for (const auto& e : probe) {
    auto key = UrlKey::parse(e, cfg.keepCgi);
    if (!key)
      continue;
    auto cm = MetaFile::load(io, key->metaPath(cfg.cacheDir));
    if (!cm)
      continue;
    tp::FileMeta fm = tp::parseFile(key->dataPath(cfg.cacheDir), "Events");
    if (!fm.error.empty())
      continue;
    int fd = ::open(key->dataPath(cfg.cacheDir).c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
      continue;
    CacheSource csrc;
    csrc.fd = fd;
    csrc.meta = &*cm;
    std::string seen;
    for (const auto& b : fm.branches)
      if (tp::fullyCached(b, csrc)) {
        seen = tp::branchCodec(fm, b, csrc);
        if (!seen.empty())
          break;
      }
    ::close(fd);
    if (seen.empty())
      continue;
    bool listed = false;
    for (const auto& want : cfg.recompressCodecs)
      if (want == seen)
        listed = true;
    if (!listed) {
      std::string list;
      for (const auto& c : cfg.recompressCodecs)
        list += (list.empty() ? "" : ",") + c;
      return "this cache holds " + seen + " content but recompress_codecs = " + list +
             ", so every entry is declined — `ucache set recompress_codecs " + seen + "`";
    }
    break; // codec is listed: not the cause
  }
#else
  (void)io;
#endif
  return "nothing has been built yet (no cause identified — see " +
         cfg.cacheDir + "/recompress.log)";
}

// ---------------------------------------------------------------------------
// Run-level readouts: `summary` (what the last run got) and `history` (whether
// that is holding up over time). Both read only records the plugin already
// writes -- there is no sampling loop and no new state file.
// ---------------------------------------------------------------------------

// "39s", "5m39s", "1h14m", "2d3h" -- a duration a person reads at a glance.
std::string humanDur(uint64_t s) {
  char out[32];
  if (s < 60)
    std::snprintf(out, sizeof out, "%llus", (unsigned long long)s);
  else if (s < 3600)
    std::snprintf(out, sizeof out, "%llum%02llus", (unsigned long long)(s / 60),
                  (unsigned long long)(s % 60));
  else if (s < 86400)
    std::snprintf(out, sizeof out, "%lluh%02llum", (unsigned long long)(s / 3600),
                  (unsigned long long)((s % 3600) / 60));
  else
    std::snprintf(out, sizeof out, "%llud%lluh", (unsigned long long)(s / 86400),
                  (unsigned long long)((s % 86400) / 3600));
  return out;
}

std::string stamp(uint64_t t) {
  char out[32];
  const time_t tt = static_cast<time_t>(t);
  struct tm tmv;
  ::localtime_r(&tt, &tmv);
  std::strftime(out, sizeof out, "%Y-%m-%d %H:%M", &tmv);
  return out;
}

double mbPerS(uint64_t bytes, uint64_t seconds) {
  return seconds ? static_cast<double>(bytes) / static_cast<double>(seconds) / 1e6 : 0.0;
}

// What the run label should say. Deliberately not a percentage: "warm" and
// "fill" are the two states a user acts on differently.
const char* runKind(const Run& r) {
  if (r.originBytes && r.cacheBytes() == 0)
    return "fill";
  if (r.originBytes == 0 && r.cacheBytes())
    return "warm";
  if (r.originBytes && r.cacheBytes())
    return "mixed";
  return "idle";
}

int cmdHistory(const Config& cfg, int argc, char** argv) {
  size_t top = 20;
  bool asJson = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--json"))
      asJson = true;
    else if (!std::strcmp(argv[i], "--top") && i + 1 < argc)
      top = static_cast<size_t>(std::max(1, ::atoi(argv[++i])));
    else {
      std::fprintf(stderr, "history: unknown argument %s\n", argv[i]);
      return 2;
    }
  }
  const auto runs = loadRuns(cfg.cacheDir + "/stats");
  if (runs.empty()) {
    if (asJson)
      std::puts("{\"runs\":[]}");
    else
      std::puts("no runs recorded yet — records appear when a job that used the "
                "cache exits (CLI invocations do not write them)");
    return 0;
  }
  const size_t shown = std::min(top, runs.size());
  if (!asJson)
    std::printf("%zu run(s) recorded, newest first (showing %zu)\n", runs.size(), shown);

  // The aggregate goes FIRST: one run says what happened last time, the total
  // says whether the cache is worth having at all.
  const Totals t = summarize(runs);
  const uint64_t tServed = t.cacheBytes() + t.relayBytes;
  auto pct = [](uint64_t part, uint64_t whole) {
    return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
  };
  if (asJson) {
    std::printf("{\"totals\":{\"runs\":%zu,\"runs_estimated\":%zu,\"distinct_files\":%zu,"
                "\"duration_s\":%llu,\"cache_bytes\":%llu,\"origin_bytes\":%llu,"
                "\"relay_bytes\":%llu,\"faults\":%llu,\"saved_s\":%.1f,\"gain\":",
                t.runs, t.runsEstimated, t.distinctFiles, (unsigned long long)t.durationS,
                (unsigned long long)t.cacheBytes(), (unsigned long long)t.originBytes,
                (unsigned long long)t.relayBytes, (unsigned long long)t.faults, t.savedS);
    if (t.haveGain)
      std::printf("%.3f},\"runs\":[", t.gain);
    else
      std::printf("null},\"runs\":[");
  } else {
    std::printf("%-16s %8s %7s %10s %10s %10s  %-18s %6s %s\n", "WHEN", "DUR", "FILES",
                "SERVED", "ORIGIN", "RATE", "BYTE/REPL/RELAY", "FAULTS", "GAIN");
    char allTiers[32], allRate[16], allGain[16], allWhen[24];
    std::snprintf(allTiers, sizeof allTiers, "%.0f%%/%.0f%%/%.0f%%",
                  pct(t.hitBytes, tServed), pct(t.replicaBytes, tServed),
                  pct(t.relayBytes, tServed));
    std::snprintf(allRate, sizeof allRate, "%.0f MB/s",
                  mbPerS(tServed + t.originBytes, t.durationS));
    if (t.haveGain)
      std::snprintf(allGain, sizeof allGain, "%.2fx", t.gain);
    else
      std::snprintf(allGain, sizeof allGain, "-");
    std::snprintf(allWhen, sizeof allWhen, "ALL (%zu runs)", t.runs);
    std::printf("%-16s %8s %7zu %10s %10s %10s  %-18s %6llu %s\n", allWhen,
                humanDur(t.durationS).c_str(), t.distinctFiles, human(tServed).c_str(),
                human(t.originBytes).c_str(), allRate, allTiers,
                (unsigned long long)t.faults, allGain);
    if (t.haveGain)
      std::printf("%-16s estimated across %zu of %zu runs — about %s of origin time "
                  "not spent\n",
                  "", t.runsEstimated, t.runs, humanDur((uint64_t)t.savedS).c_str());
    if (t.gainCapped)
      std::printf("%-16s (%zu older run(s) not included in the gain — too many to "
                  "estimate)\n", "", t.gainCapped);
    std::printf("%-16s %8s %7s %10s %10s %10s  %-18s %6s %s\n", "----", "----", "-----",
                "------", "------", "----", "---------------", "------", "----");
  }

  for (size_t i = 0; i < shown; ++i) {
    const Run& r = runs[i];
    const uint64_t total = r.hitBytes + r.replicaBytesServed + r.relayBytes;
    const double pb = total ? 100.0 * static_cast<double>(r.hitBytes) / static_cast<double>(total) : 0.0;
    const double pr = total ? 100.0 * static_cast<double>(r.replicaBytesServed) / static_cast<double>(total) : 0.0;
    const double pl = total ? 100.0 * static_cast<double>(r.relayBytes) / static_cast<double>(total) : 0.0;
    const GainEstimate g = estimateGain(r, runs);
    if (asJson) {
      std::printf("%s{\"start\":%llu,\"duration_s\":%llu,\"host\":\"%s\",\"pid\":%llu,"
                  "\"kind\":\"%s\",\"files\":%llu,\"served_bytes\":%llu,"
                  "\"origin_bytes\":%llu,\"hit_bytes\":%llu,\"replica_bytes\":%llu,"
                  "\"relay_bytes\":%llu,\"faults\":%llu,\"gain\":",
                  i ? "," : "", (unsigned long long)r.startS,
                  (unsigned long long)r.durationS(), r.host.c_str(),
                  (unsigned long long)r.pid, runKind(r),
                  (unsigned long long)r.files.size(), (unsigned long long)r.servedBytes,
                  (unsigned long long)r.originBytes, (unsigned long long)r.hitBytes,
                  (unsigned long long)r.replicaBytesServed,
                  (unsigned long long)r.relayBytes, (unsigned long long)r.faults());
      if (g.valid)
        std::printf("%.3f}", g.gain);
      else
        std::printf("null}");
      continue;
    }
    char gainCell[16];
    if (g.valid)
      std::snprintf(gainCell, sizeof gainCell, "%.2fx", g.gain);
    else
      std::snprintf(gainCell, sizeof gainCell, "%s",
                    r.originBytes && r.cacheBytes() == 0 ? "fill" : "-");
    char tiers[32];
    std::snprintf(tiers, sizeof tiers, "%.0f%%/%.0f%%/%.0f%%", pb, pr, pl);
    char rate[16];
    std::snprintf(rate, sizeof rate, "%.0f MB/s", mbPerS(total + r.originBytes, r.durationS()));
    std::printf("%-16s %8s %7zu %10s %10s %10s  %-18s %6llu %s\n", stamp(r.startS).c_str(),
                humanDur(r.durationS()).c_str(), r.files.size(), human(total).c_str(),
                human(r.originBytes).c_str(), rate, tiers,
                (unsigned long long)r.faults(), gainCell);
  }
  if (asJson)
    std::puts("]}");
  else if (runs.size() > shown)
    std::printf("(%zu older run(s) not shown — `--top %zu` for more)\n", runs.size() - shown,
                runs.size());
  return 0;
}

int cmdSummary(CacheStore& store, int argc, char** argv) {
  const Config& cfg = store.config();
  bool asJson = false, detail = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--json"))
      asJson = true;
    else if (!std::strcmp(argv[i], "--detail") || !std::strcmp(argv[i], "-d"))
      detail = true;
    else {
      std::fprintf(stderr, "summary: unknown argument %s\n", argv[i]);
      return 2;
    }
  }
  const auto runs = loadRuns(cfg.cacheDir + "/stats");
  auto entries = store.listEntries();
  uint64_t used = 0, replicaTotal = 0, replicaN = 0;
  for (const auto& e : entries) {
    used += e.cachedBytes;
    if (e.replicaBytes) {
      replicaTotal += e.replicaBytes;
      ++replicaN;
    }
  }
  uint64_t avail = 0, totalSpace = 0, headroom = 0;
  if (RealIO::instance().spaceInfo(cfg.cacheDir, avail, totalSpace) == 0 && totalSpace)
    headroom = cfg.minFreeBytes && avail > cfg.minFreeBytes ? avail - cfg.minFreeBytes : 0;

  const Run* last = runs.empty() ? nullptr : &runs.front();
  GainEstimate gain;
  if (last)
    gain = estimateGain(*last, runs);
  else
    gain.reason = "no runs recorded yet"; // a JSON consumer gets a reason too

  if (asJson) {
    const Totals tj = summarize(runs);
    std::printf("{\"cache_dir\":\"%s\",\"entries\":%zu,\"cached_bytes\":%llu,"
                "\"replica_bytes\":%llu,\"replicas\":%llu,\"headroom_bytes\":%llu",
                cfg.cacheDir.c_str(), entries.size(), (unsigned long long)used,
                (unsigned long long)replicaTotal, (unsigned long long)replicaN,
                (unsigned long long)headroom);
    std::printf(",\"overall\":{\"runs\":%zu,\"runs_estimated\":%zu,\"duration_s\":%llu,"
                "\"cache_bytes\":%llu,\"origin_bytes\":%llu,\"faults\":%llu,"
                "\"saved_s\":%.1f,\"gain\":",
                tj.runs, tj.runsEstimated, (unsigned long long)tj.durationS,
                (unsigned long long)tj.cacheBytes(), (unsigned long long)tj.originBytes,
                (unsigned long long)tj.faults, tj.savedS);
    if (tj.haveGain)
      std::printf("%.3f}", tj.gain);
    else
      std::printf("null}");
    if (last) {
      std::printf(",\"last_run\":{\"start\":%llu,\"duration_s\":%llu,\"kind\":\"%s\","
                  "\"files\":%zu,\"cache_bytes\":%llu,\"origin_bytes\":%llu,"
                  "\"relay_bytes\":%llu,\"faults\":%llu}",
                  (unsigned long long)last->startS, (unsigned long long)last->durationS(),
                  runKind(*last), last->files.size(),
                  (unsigned long long)last->cacheBytes(),
                  (unsigned long long)last->originBytes,
                  (unsigned long long)last->relayBytes,
                  (unsigned long long)last->faults());
    }
    std::printf(",\"gain\":");
    if (gain.valid)
      std::printf("{\"estimate\":%.3f,\"saved_s\":%.1f,\"origin_mb_s\":%.1f,"
                  "\"matched_files\":%llu,\"origin_equivalent_bytes\":%llu}",
                  gain.gain, gain.savedS, gain.originMBs,
                  (unsigned long long)gain.matchedFiles,
                  (unsigned long long)gain.originEquivBytes);
    else
      std::printf("null");
    std::printf(",\"gain_reason\":\"%s\"}\n", gain.reason.c_str());
    return 0;
  }

  // Overall first. The last run answers "what just happened"; the aggregate
  // answers "is this cache worth having", which is the question being asked.
  const Totals t = summarize(runs);
  if (t.runs) {
    std::printf("overall    : %zu run(s) over %s — %s served from cache, %s from the "
                "origin\n",
                t.runs, humanDur(t.durationS).c_str(), human(t.cacheBytes()).c_str(),
                human(t.originBytes).c_str());
    if (t.haveGain)
      std::printf("  gain     : ~%.1fx versus reading from the origin (estimate) — about "
                  "%s of origin time not spent, across %zu of %zu run(s)\n",
                  t.gain, humanDur(static_cast<uint64_t>(t.savedS)).c_str(),
                  t.runsEstimated, t.runs);
    else
      std::printf("  gain     : not estimated for any run yet\n");
    if (t.faults)
      std::printf("  health   : %llu fault(s) across all runs — `ucache verify <url>`\n",
                  (unsigned long long)t.faults);
    else
      std::printf("  health   : OK — no faults in any recorded run\n");
  }

  std::printf("cache      : %s — %zu entries, %s on disk", cfg.cacheDir.c_str(),
              entries.size(), human(used + replicaTotal).c_str());
  if (cfg.minFreeBytes)
    std::printf(", %s headroom", headroom ? human(headroom).c_str() : "NO");
  std::putchar('\n');

  if (!detail) {
    if (last)
      std::puts("next       : `ucache summary --detail` for the last run; "
                "`ucache history` for the trend");
    else
      std::puts("next       : run your analysis once, then `ucache summary` again");
    return 0;
  }

  if (!last) {
    std::puts("last run   : none recorded yet — records appear when a job that used "
              "the cache exits");
    std::puts("next       : run your analysis once, then `ucache summary` again");
    return 0;
  }

  const uint64_t nowS = static_cast<uint64_t>(::time(nullptr));
  std::printf("last run   : %s (%s ago), ran %s, %zu file(s) — %s\n",
              stamp(last->startS).c_str(), humanAge(last->endS, nowS).c_str(),
              humanDur(last->durationS()).c_str(), last->files.size(), runKind(*last));
  std::printf("delivered  : %s from cache; %s crossed the network\n",
              human(last->cacheBytes()).c_str(),
              human(last->originBytes + last->relayBytes).c_str());
  const uint64_t tiered = last->hitBytes + last->replicaBytesServed + last->relayBytes;
  if (tiered)
    std::printf("  tiers    : byte %.1f%% | replica %.1f%% | relay %.1f%%\n",
                100.0 * static_cast<double>(last->hitBytes) / static_cast<double>(tiered),
                100.0 * static_cast<double>(last->replicaBytesServed) / static_cast<double>(tiered),
                100.0 * static_cast<double>(last->relayBytes) / static_cast<double>(tiered));
  std::printf("  rate     : %.0f MB/s delivered over the run\n",
              mbPerS(tiered + last->originBytes, last->durationS()));
  if (last->hitDiskReads)
    std::printf("  byte tier: %llu disk reads (mean %s)\n",
                (unsigned long long)last->hitDiskReads,
                human(last->hitDiskBytes / last->hitDiskReads).c_str());
  if (last->replicaReads)
    std::printf("  replica  : %llu disk reads (mean %s)\n",
                (unsigned long long)last->replicaReads,
                human(last->replicaReadBytes / last->replicaReads).c_str());
  if (last->faults() == 0)
    std::puts("health     : OK — no checksum failures, fail-open events, or invalid "
              "replicas");
  else
    std::printf("health     : %llu fault(s) — crc %llu, replica crc %llu, invalid "
                "replica %llu, fail-open %llu, sidecar %llu. Run `ucache verify <url>`\n",
                (unsigned long long)last->faults(), (unsigned long long)last->crcFailures,
                (unsigned long long)last->replicaCrcFailures,
                (unsigned long long)last->replicaInvalid,
                (unsigned long long)last->failopenEvents,
                (unsigned long long)last->metaCorrupt);
  if (!entries.empty())
    std::printf("recompress : %llu of %zu entries have a replica (%.0f%%)\n",
                (unsigned long long)replicaN, entries.size(),
                100.0 * static_cast<double>(replicaN) / static_cast<double>(entries.size()));

  // The estimate, or the reason there isn't one. Never both a number and a
  // doubt: outside the conditions it was validated under it is not printed.
  if (gain.valid) {
    std::printf("gain       : ~%.1fx versus reading from the origin (estimate)\n", gain.gain);
    std::printf("             basis: %.0f MB/s measured when these files were first "
                "fetched (%s), %llu of %llu files matched; ~%.0f s saved\n",
                gain.originMBs, stamp(gain.referenceStartS).c_str(),
                (unsigned long long)gain.matchedFiles, (unsigned long long)gain.runFiles,
                gain.savedS);
    std::puts("             read-path estimate — an A/B against a direct run is the "
              "only measurement");
  } else {
    std::printf("gain       : not estimated — %s\n", gain.reason.c_str());
  }
  std::puts("next       : `ucache history` for the trend across runs");
  return 0;
}

int cmdStatus(CacheStore& store, IOBackend& io) {
  const Config& cfg = store.config(); // the EFFECTIVE config (post budget resolution)
  auto entries = store.listEntries();
  uint64_t used = 0, pinned = 0, protectedN = 0, protectedBytes = 0;
  // Recomputed here from live state rather than read from the plugin's latch:
  // `status` is a separate process, and a state file would be one more thing to
  // keep in sync or expire.
  const uint64_t nowS = static_cast<uint64_t>(::time(nullptr));
  const uint64_t protectCutoff = cfg.evictProtectSeconds && nowS > cfg.evictProtectSeconds
                                     ? nowS - cfg.evictProtectSeconds
                                     : 0;
  for (const auto& e : entries) {
    used += e.cachedBytes;
    if (e.pinned)
      ++pinned;
    if (protectCutoff && e.atime >= protectCutoff && !e.pinned) {
      ++protectedN;
      protectedBytes += e.cachedBytes + e.replicaBytes;
    }
  }
  std::printf("cache dir : %s\n", cfg.cacheDir.c_str());
  if (cfg.maxBytes)
    std::printf("budget    : %s hard cap (evict at %.0f%%)\n", human(cfg.maxBytes).c_str(),
                cfg.highWater * 100.0);
  else if (cfg.minFreeBytes)
    std::printf("budget    : keep %s free (no byte cap; uses the disk)\n",
                human(cfg.minFreeBytes).c_str());
  else
    std::printf("budget    : eviction disabled\n");
  // Make "the cache is full" visible BEFORE eviction bites — the
  // production eviction storm was invisible in this output until it was fatal.
  if (cfg.minFreeBytes) {
    uint64_t avail = 0, total = 0;
    if (RealIO::instance().spaceInfo(cfg.cacheDir, avail, total) == 0 && total) {
      if (avail <= cfg.minFreeBytes)
        std::printf("headroom  : NONE — disk free (%s) is at the eviction floor (%s); "
                    "every new fill evicts something\n",
                    human(avail).c_str(), human(cfg.minFreeBytes).c_str());
      else
        std::printf("headroom  : %s until eviction starts (disk free %s, floor %s)\n",
                    human(avail - cfg.minFreeBytes).c_str(), human(avail).c_str(),
                    human(cfg.minFreeBytes).c_str());
    }
  }
  if (cfg.evictProtectSeconds) {
    uint64_t avail = 0, total = 0;
    const bool haveSpace =
        RealIO::instance().spaceInfo(cfg.cacheDir, avail, total) == 0 && total;
    const bool atFloor = cfg.minFreeBytes && haveSpace && avail <= cfg.minFreeBytes;
    // The state worth shouting about: no room left AND nothing old enough to give
    // up, so the cache has stopped growing. Say what it means and how to undo it,
    // because a cache that has quietly stopped caching looks like a slow cache.
    if (atFloor && protectedN == entries.size() - pinned && !entries.empty())
      std::printf("protected : %llu entries (%s) read within the last %s — ALL of them, "
                  "and the disk is at the floor, so NEW FILES ARE NOT BEING CACHED. "
                  "`ucache evict --older-than <dur>`, or lower evict_protect_seconds\n",
                  static_cast<unsigned long long>(protectedN), human(protectedBytes).c_str(),
                  humanAge(1, 1 + cfg.evictProtectSeconds).c_str());
    else
      std::printf("protected : %llu of %zu entries (%s) read within the last %s — not "
                  "evictable, so a running job cannot evict its own working set\n",
                  static_cast<unsigned long long>(protectedN), entries.size(),
                  human(protectedBytes).c_str(),
                  humanAge(1, 1 + cfg.evictProtectSeconds).c_str());
  }
  std::printf("freshness : %s\n", freshnessSummary(cfg).c_str());
  std::printf("entries   : %zu (%llu pinned)\n", entries.size(), (unsigned long long)pinned);
  // Footprint summary: what fraction of the original files the
  // cache holds — the at-a-glance answer to "did my analysis read most of
  // the data or a thin slice", aggregated and as a per-file median.
  uint64_t origTotal = 0, replicaTotal = 0;
  size_t replicaN = 0;
  std::vector<double> covs;
  covs.reserve(entries.size());
  for (const auto& e : entries) {
    origTotal += e.fileSize;
    if (e.replicaBytes) {
      replicaTotal += e.replicaBytes;
      ++replicaN;
    }
    covs.push_back(e.coverage);
  }
  if (origTotal) {
    std::sort(covs.begin(), covs.end());
    double med = covs.empty() ? 0.0 : covs[covs.size() / 2];
    std::printf("original  : %s (total size of the cached files at the origin)\n",
                human(origTotal).c_str());
    std::printf("cached    : %s — %.1f%% of the original bytes (median file: %.1f%%)\n",
                human(used).c_str(), 100.0 * static_cast<double>(used) /
                                         static_cast<double>(origTotal),
                100.0 * med);
  } else
    std::printf("cached    : %s\n", human(used).c_str());
  std::printf("disk used : %s (%s cached bytes + %s recompressed)\n",
              human(used + replicaTotal).c_str(), human(used).c_str(),
              human(replicaTotal).c_str());
  size_t queueDepth = 0;
  {
    std::ifstream pend(cfg.cacheDir + "/recompress.pending");
    std::string line;
    while (std::getline(pend, line))
      if (!line.empty())
        ++queueDepth;
    if (queueDepth)
      std::printf("queued    : %zu entr%s awaiting background recompression\n", queueDepth,
                  queueDepth == 1 ? "y" : "ies");
  }
  if (replicaN)
    std::printf("recompressed: %zu entr%s, %s (recompress %s; codecs:%s%s)\n",
                replicaN,
                replicaN == 1 ? "y" : "ies", human(replicaTotal).c_str(),
                cfg.recompress ? "on" : "off — manual `ucache recompress` only",
                [&] {
                  std::string s;
                  for (const auto& codec : cfg.recompressCodecs)
                    s += " " + codec;
                  return s;
                }()
                    .c_str(),
                cfg.recompressReclaim == Config::Reclaim::kFull
                    ? "; reclaim full — replicas replace the byte copy"
                    : "");
  else
    std::printf("recompressed: none%s\n",
                cfg.recompress
                    ? " yet (recompress = on: files your jobs read build in the background)"
                    : " — `ucache set recompress on` for automatic background builds, "
                      "or `ucache recompress` to transcode what is cached now");
  // Queued but nothing built: say why, rather than leaving the user to notice
  // an empty recompress.log. Cheap checks only here — status is a fast command.
  if (std::string why = recompressStall(cfg, io, queueDepth, replicaN, /*deep=*/false);
      !why.empty())
    std::printf("recompress: %s\n", why.c_str());
  printStats(aggregateStats(cfg.cacheDir + "/stats"));
  return 0;
}

int cmdLs(CacheStore& store, int argc, char** argv) {
  std::string sortBy = "size"; // default: largest first (as before)
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--sort" || a.rfind("--sort=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 6);
      if (!v) {
        std::fputs("ls: --sort needs 'age' or 'size'\n", stderr);
        return 2;
      }
      sortBy = v;
    } else {
      std::fprintf(stderr, "ls: unknown option '%s'\n", a.c_str());
      return 2;
    }
  }
  if (sortBy != "size" && sortBy != "age") {
    std::fputs("ls: --sort must be 'age' or 'size'\n", stderr);
    return 2;
  }
  auto entries = store.listEntries();
  if (sortBy == "age") // stalest first — the cleanup-decision order
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.atime < b.atime; });
  else
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.cachedBytes > b.cachedBytes; });
  uint64_t now = static_cast<uint64_t>(::time(nullptr));
  std::printf("%-9s %-9s %-6s %-6s %-9s %-4s  %s\n", "SIZE", "CACHED", "COV%", "LAST", "RECOMP",
              "PIN", "KEY");
  for (const auto& e : entries)
    std::printf("%-9s %-9s %5.1f%% %-6s %-9s %-4s  %s\n", human(e.fileSize).c_str(),
                human(e.cachedBytes).c_str(), e.coverage * 100.0,
                humanAge(e.atime, now).c_str(),
                e.replicaBytes ? human(e.replicaBytes).c_str() : "-", e.pinned ? "yes" : "",
                e.key.c_str());
  std::printf("(%zu entries)\n", entries.size());
  return 0;
}

#ifdef UCACHE_HAVE_TRANSPOSE
// Byte sources for the native transposer: a plain file, or the entry's
// sparse v1 .data image gated by its bitmap (only cached ranges are usable —
// `materialize` never touches the network).
struct FdSource : tp::Source {
  int fd = -1;
  uint64_t size = 0;
  bool read(void* dst, uint64_t n, uint64_t off) override {
    return ::pread(fd, dst, n, off) == static_cast<ssize_t>(n);
  }
  bool has(uint64_t off, uint64_t n) override { return off + n <= size; }
};

// Native overlay build: parse + transcode from `srcPath` (bitmap-
// gated when it is the cache image). Returns 0 and fills meta/tdata.
int buildNative(const std::string& srcPath, const MetaData* cacheMeta,
                const std::string& branches, const std::string& tree,
                ReplicaMeta& outMeta, std::vector<uint8_t>& outTdata,
                std::string& summary, uint64_t* oldHotOut = nullptr,
                uint64_t* newHotOut = nullptr,
                const std::vector<std::string>& codecs = {}) {
  tp::FileMeta fm = tp::parseFile(srcPath, tree);
  if (!fm.error.empty()) {
    std::fprintf(stderr, "materialize: parse failed: %s\n", fm.error.c_str());
    return 1;
  }
  int fd = ::open(srcPath.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::fprintf(stderr, "materialize: cannot open %s\n", srcPath.c_str());
    return 1;
  }
  FdSource fsrc;
  CacheSource csrc;
  tp::Source* src;
  if (cacheMeta) {
    csrc.fd = fd;
    csrc.meta = cacheMeta;
    src = &csrc;
  } else {
    fsrc.fd = fd;
    fsrc.size = static_cast<uint64_t>(fm.fend);
    src = &fsrc;
  }
  std::vector<std::string> hot;
  if (!branches.empty()) {
    std::istringstream ss(branches);
    std::string b;
    while (std::getline(ss, b, ','))
      if (!b.empty())
        hot.push_back(b);
    hot = tp::withCounters(fm, hot, *src);
  } else {
    hot = tp::deriveHotBranches(fm, *src, codecs); // learner + codec policy
    if (hot.empty()) {
      ::close(fd);
      return 3; // nothing qualifies (not read enough, or codec policy) — skip
    }
  }
  tp::Overlay ov = tp::buildOverlay(fm, *src, hot);
  ::close(fd);
  if (!ov.error.empty()) {
    std::fprintf(stderr, "materialize: build failed: %s\n", ov.error.c_str());
    return 1;
  }
  if (oldHotOut)
    *oldHotOut = ov.oldBytes;
  if (newHotOut)
    *newHotOut = ov.newBytes;
  outMeta = std::move(ov.meta);
  outTdata = std::move(ov.tdata);
  char buf[256];
  std::snprintf(buf, sizeof buf,
                "  built natively: %llu baskets from %zu branches "
                "(%llu transcoded, %llu verbatim, %llu raw-fallback), "
                "%s -> %s hot bytes",
                (unsigned long long)ov.baskets, hot.size(),
                (unsigned long long)ov.transcoded, (unsigned long long)ov.verbatim,
                (unsigned long long)ov.fallbackRaw, human(ov.oldBytes).c_str(),
                human(ov.newBytes).c_str());
  summary = buf;
  char mbuf[160];
  std::snprintf(mbuf, sizeof mbuf, "\n  relocated metadata: %s -> %s at rest (%.2fx, ZSTD)",
                human(ov.metaRawBytes).c_str(), human(ov.metaStoredBytes).c_str(),
                ov.metaStoredBytes ? (double)ov.metaRawBytes / (double)ov.metaStoredBytes
                                   : 1.0);
  summary += mbuf;
  return 0;
}
#endif // UCACHE_HAVE_TRANSPOSE

// materialize <url> [--branches a,b|--import PREFIX] [--punch] [--from-file F]
// [--overlay-out PREFIX]: build (natively, from the entry's cached bytes —
// or from an explicit file in test mode) or import a replica overlay, and
// publish it through the crash-safe publish path. --overlay-out writes the
// overlay files instead of publishing (the byte-differential gate vs the
// Python builder).
// `store` may be NULL only in the pure builder mode (--from-file +
// --overlay-out): a file->file transform that never touches the cache and
// therefore needs no cache dir (deliberate exemption; the roundtrip gate runs
// it store-less).
int cmdMaterialize(CacheStore* store, const Config& cfg, IOBackend& io, int argc,
                   char** argv) {
  const char* url = nullptr;
  std::string prefix, branches, fromFile, overlayOut, tree = "Events";
  bool punch = false;
  for (int i = 2; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--import") && i + 1 < argc)
      prefix = argv[++i];
    else if (!std::strcmp(argv[i], "--branches") && i + 1 < argc)
      branches = argv[++i];
    else if (!std::strcmp(argv[i], "--from-file") && i + 1 < argc)
      fromFile = argv[++i];
    else if (!std::strcmp(argv[i], "--overlay-out") && i + 1 < argc)
      overlayOut = argv[++i];
    else if (!std::strcmp(argv[i], "--tree") && i + 1 < argc)
      tree = argv[++i];
    else if (!std::strcmp(argv[i], "--punch"))
      punch = true;
    else if (!url)
      url = argv[i];
    else {
      std::fprintf(stderr, "materialize: unexpected argument %s\n", argv[i]);
      return 2;
    }
  }
  if (!url && overlayOut.empty()) {
    std::fputs("materialize: needs a <url> (or --from-file F --overlay-out P)\n", stderr);
    return 2;
  }
  std::optional<UrlKey> key;
  if (url) {
    key = UrlKey::parse(url, cfg.keepCgi); // SAME normalization as the plugin
    if (!key) {
      std::fprintf(stderr, "%s: not a valid URL\n", url);
      return 2;
    }
  }

  ReplicaMeta meta;
  std::vector<uint8_t> tdata8;
  std::string buildSummary;
  if (prefix.empty()) {
#ifdef UCACHE_HAVE_TRANSPOSE
    std::optional<MetaData> cm;
    std::string srcPath = fromFile;
    if (srcPath.empty()) {
      if (!store) {
        std::fputs("materialize: no cache dir configured — set `dir =` in ucache.conf "
                   "(USER_GUIDE §2) or UCACHE_DIR\n", stderr);
        return 2;
      }
      cm = MetaFile::load(io, key->metaPath(cfg.cacheDir));
      if (!cm) {
        std::fprintf(stderr, "materialize: %s is not in the byte cache "
                             "(run the analysis once first)\n",
                     key->key.c_str());
        return 1;
      }
      srcPath = key->dataPath(cfg.cacheDir);
    }
    if (int rc = buildNative(srcPath, cm ? &*cm : nullptr, branches, tree, meta, tdata8,
                             buildSummary)) {
      if (rc == 3)
        std::fputs("materialize: no fully-cached branches to transpose "
                   "(run the analysis once to teach the cache)\n",
                   stderr);
      return 1;
    }
    if (cm) { // validators: what the plugin validated the entry against (§7)
      meta.originMtime = cm->originMtime;
      meta.cksumKind = cm->cksumKind;
      meta.originCksum = cm->originCksum;
      if (meta.originSize != cm->fileSize) {
        std::fputs("materialize: cached size != parsed fEND; refusing\n", stderr);
        return 1;
      }
    }
    if (!overlayOut.empty()) { // differential-gate mode: emit files, no publish
      std::ofstream td(overlayOut + ".tdata", std::ios::binary);
      td.write(reinterpret_cast<const char*>(tdata8.data()),
               static_cast<std::streamsize>(tdata8.size()));
      std::ofstream mp(overlayOut + ".map");
      mp << "ucache-overlay-map v1\n";
      mp << "origin_size " << meta.originSize << "\n";
      mp << "virtual_size " << meta.virtualSize << "\n";
      mp << "encoding zstd1\n";
      mp << "encoder_version " << meta.encoderVersion << "\n";
      for (const auto& e : meta.extents)
        mp << "extent " << e.virtOff << " " << e.len << " " << e.tdataOff << "\n";
      for (const auto& r : meta.superseded)
        mp << "superseded " << r.off << " " << r.len << "\n";
      std::printf("overlay written to %s.{tdata,map}\n%s\n", overlayOut.c_str(),
                  buildSummary.c_str());
      return 0;
    }
#else
    std::fputs("materialize: built without the native transposer (codec libs "
               "missing); use --import PREFIX\n",
               stderr);
    return 2;
#endif
  }

  if (!prefix.empty()) { // import mode
  std::ifstream map(prefix + ".map");
  if (!map) {
    std::fprintf(stderr, "materialize: cannot read %s.map\n", prefix.c_str());
    return 1;
  }
  std::string line;
  if (!std::getline(map, line) || line != "ucache-overlay-map v1") {
    std::fprintf(stderr, "materialize: %s.map is not an overlay map (v1)\n", prefix.c_str());
    return 1;
  }
  while (std::getline(map, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream ss(line);
    std::string k;
    ss >> k;
    if (k == "origin_size")
      ss >> meta.originSize;
    else if (k == "virtual_size")
      ss >> meta.virtualSize;
    else if (k == "encoder_version")
      ss >> meta.encoderVersion;
    else if (k == "encoding") {
      std::string v;
      ss >> v;
      meta.encoding = v == "zstd1"  ? ReplicaMeta::kZstd1
                      : v == "lz4"  ? ReplicaMeta::kLz4
                      : v == "raw"  ? ReplicaMeta::kRaw
                                    : 0;
    } else if (k == "extent") {
      ReplicaMeta::Extent e;
      ss >> e.virtOff >> e.len >> e.tdataOff;
      meta.extents.push_back(e);
    } else if (k == "superseded") {
      ReplicaMeta::Range r;
      ss >> r.off >> r.len;
      meta.superseded.push_back(r);
    } // unknown keys: ignored (forward compatibility)
  }
  if (!meta.encoding || !meta.originSize || meta.virtualSize < meta.originSize) {
    std::fprintf(stderr, "materialize: %s.map is incomplete\n", prefix.c_str());
    return 1;
  }

  std::ifstream td(prefix + ".tdata", std::ios::binary | std::ios::ate);
  if (!td) {
    std::fprintf(stderr, "materialize: cannot read %s.tdata\n", prefix.c_str());
    return 1;
  }
  tdata8.resize(static_cast<size_t>(td.tellg()));
  td.seekg(0);
  td.read(reinterpret_cast<char*>(tdata8.data()),
          static_cast<std::streamsize>(tdata8.size()));
  if (!td) {
    std::fprintf(stderr, "materialize: short read of %s.tdata\n", prefix.c_str());
    return 1;
  }
  } // import mode

  if (!store) { // url/publish modes need the cache
    std::fputs("materialize: no cache dir configured — set `dir =` in ucache.conf "
               "(USER_GUIDE §2) or UCACHE_DIR\n", stderr);
    return 2;
  }
  ReplicaStore rs(io, cfg, store->stats());
  int rc = rs.publish(*key, meta, tdata8.data(), tdata8.size());
  if (rc != 0) {
    std::fprintf(stderr, "materialize: publish failed (%d)\n", rc);
    return 1;
  }
  // Adoption check right away — the same validated open the plugin will do.
  auto view = rs.openView(*key, meta.originSize, meta.originMtime, meta.cksumKind,
                          meta.originCksum);
  if (!view) {
    std::fputs("materialize: published replica failed its own adoption check\n", stderr);
    return 1;
  }
  if (!buildSummary.empty())
    std::puts(buildSummary.c_str());
  std::printf("materialized %s\n  overlay %s in %zu extent(s), virtual size %s\n",
              key->key.c_str(), human(tdata8.size()).c_str(), view->meta().extents.size(),
              human(view->virtualSize()).c_str());
  uint64_t punched = 0;
  if (punch) {
    if (auto entry = store->open(*key, meta.originSize)) {
      punched = rs.punchSuperseded(*entry, view->meta().superseded);
      std::printf("  punched %s of superseded v1 pages\n", human(punched).c_str());
    } else {
      std::puts("  (entry not in the byte cache; nothing to punch)");
    }
  }
  return 0;
}

// transpose --auto: one-shot sweep — natively materialize every cached
// entry that has data but no replica yet (the learned hot set = every
// fully-cached branch). Failures are counted and skipped (fail-open); rerun
// any time (idempotent: entries with a replica are skipped).
// ---- Recompression: one switch --------------------------------------------
//
// `recompress = on` (conf default or `ucache set recompress on`) => the
// plugin queues each file at close and a DETACHED nice'd drainer transcodes
// it. The only skip rules are static facts: the codec list (recompress_codecs
// — transcoding zstd->zstd is pointless) and buildability (the hot branches
// must be fully cached). The retired evidence gate (min-share threshold,
// verdicts, --estimate/--force) is PARKED — the user flipping the switch IS
// the worth-it decision. The plugin still writes .cost sidecars and builds
// still calibrate the decode rate below: evidence keeps accumulating for the
// gate's future revival, but nothing is gated on it.
// Explicit `ucache recompress` = the same sweep over the existing cache,
// foreground, with live progress.

// Calibrated LZMA decode rate (MB/s == bytes/µs), measured from real
// transcodes (Overlay.decodeBytes/decodeNs) and persisted per cache dir.
// 0 = not calibrated yet. Not consulted for decisions while the gate is
// parked — maintained for its revival. Only the transcoding sweep calls
// these, so codec-less builds must not compile them (-Werror=unused-function).
#ifdef UCACHE_HAVE_TRANSPOSE
double readCalibration(const Config& cfg) {
  std::ifstream in(cfg.cacheDir + "/calibration");
  double v = 0;
  std::string k;
  if (in >> k >> v && k == "lzma_mbps" && v > 0)
    return v;
  return 0;
}
void writeCalibration(const Config& cfg, double mbps) {
  double old = readCalibration(cfg);
  double v = old > 0 ? 0.7 * old + 0.3 * mbps : mbps; // rolling
  const std::string tmp = cfg.cacheDir + "/calibration.tmp";
  std::ofstream out(tmp, std::ios::trunc);
  out << "lzma_mbps " << v << "\n";
  out.close();
  ::rename(tmp.c_str(), (cfg.cacheDir + "/calibration").c_str());
}
#endif // UCACHE_HAVE_TRANSPOSE

// Modes: kSweep (explicit `ucache recompress`: foreground pass over the
// existing cache with live progress; punches superseded v1 pages of what it
// builds), kDrain (background worker fed by recompress.pending: never
// punches LIVE v1 pages, defers to the next drain; log-only output).
enum class RecompressMode { kSweep, kDrain };

#ifdef UCACHE_HAVE_TRANSPOSE
// Reclaim policy, applied wherever a pass punches v1 pages under a
// VALIDATED replica view. `superseded` (default) frees only the ranges the
// overlay relocated; `full` drops the entry's ENTIRE v1 byte copy (whole-file
// range; releaseRanges keeps the partial tail page) — the replica becomes the
// durable form, and anything it does not cover (prefetch margin, partial
// branches, header/streamers) refetches from origin on demand (fail-open).
static uint64_t punchPerReclaim(ReplicaStore& rs, FileEntry& entry, const ReplicaMeta& meta,
                                const Config& cfg) {
  if (cfg.recompressReclaim == Config::Reclaim::kFull) {
    std::vector<ReplicaMeta::Range> whole{{0, meta.originSize}};
    return rs.punchSuperseded(entry, whole);
  }
  return rs.punchSuperseded(entry, meta.superseded);
}

#endif

int cmdRecompress(CacheStore& store, const Config& cfg, IOBackend& io, int jobs,
                  RecompressMode mode, bool yes = false, bool strict = false) {
#ifndef UCACHE_HAVE_TRANSPOSE
  (void)store;
  (void)cfg;
  (void)io;
  (void)jobs;
  (void)mode;
  (void)yes;
  (void)strict;
  std::fputs("recompress: built without the native transposer (codec libs missing)\n", stderr);
  return 2;
#else
  const bool drain = mode == RecompressMode::kDrain;
  // ONE recompression pass at a time per cache. A pass punches the v1 pages its
  // replicas supersede; a concurrent pass reading those pages works from a
  // bitmap snapshot that predates the punch. Those reads are CAUGHT rather than
  // trusted (CacheSource verifies every page), so this lock is not what makes
  // the feature correct — it stops two passes wasting effort on the same files
  // and keeps "will retry" noise out of the log. Only the background worker used
  // to take it, which left the recommended workflow — background builds plus one
  // explicit sweep — racing itself. The file keeps its name: a worker from an
  // older build may be holding THIS path right now and must still exclude us.
  //
  // The background pass is BOUNDED IN TIME (kDrainBudget below) and requeues
  // whatever it does not reach, so a foreground sweep's wait is bounded by
  // construction. That bound is what lets this be one plain lock: an earlier
  // design added a second "a sweep is waiting" lock, a probe between files and a
  // yield, purely because an unbounded batch could hold the cache for hours —
  // and every one of those moving parts was a way to lose the queue or stall.
  int lockFd = ::open((cfg.cacheDir + "/recompress.drain.lock").c_str(),
                      O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  auto closeFds = [&] {
    if (lockFd >= 0)
      ::close(lockFd);
    lockFd = -1;
  };
  if (lockFd < 0) {
    // A background worker has no business running unprotected; a user who asked
    // for a sweep gets it, with the reason stated.
    if (drain)
      return 0;
    std::fprintf(stderr, "recompress: cannot open the pass lock (%s); proceeding without it\n",
                 std::strerror(errno));
  } else if (::flock(lockFd, LOCK_EX | LOCK_NB) != 0) {
    // Distinguish "someone holds it" from "this filesystem cannot lock".
    // Treating every failure as contention made background recompression stop
    // for good, and silently, wherever flock is unimplemented.
    const int err = errno;
    if (err == EWOULDBLOCK || err == EAGAIN || err == EACCES) {
      if (drain) {
        closeFds();
        return 0; // another pass owns the cache; it will claim this queue
      }
      std::fputs("recompress: waiting for the background worker to finish its batch\n", stderr);
      if (::flock(lockFd, LOCK_EX) != 0) {
        std::fprintf(stderr, "recompress: lock wait failed (%s); proceeding without it\n",
                     std::strerror(errno));
        closeFds();
      }
    } else {
      if (drain) {
        std::ofstream log(cfg.cacheDir + "/recompress.log", std::ios::app);
        log << "recompress: advisory locking unavailable on this filesystem (" << std::strerror(err)
            << "); not running unprotected — use `ucache recompress` explicitly\n";
        closeFds();
        return 0;
      }
      std::fprintf(stderr, "recompress: advisory locking unavailable (%s); proceeding without it\n",
                   std::strerror(err));
      closeFds();
    }
  }
  std::ofstream drainLog;
  if (drain) {
    ::setpriority(PRIO_PROCESS, 0, 10); // background work yields the CPU
    drainLog.open(cfg.cacheDir + "/recompress.log", std::ios::app);
    // Be explicit when the capacity guard is inert. It protects the free-disk
    // floor; a cache governed by an explicit `max_bytes` evicts on its own
    // trigger, which this guard cannot see. Saying so beats a silent exception
    // to "background recompression never evicts".
    if (headroomToFloor(cfg, io) == ~0ull && drainLog)
      drainLog << "recompress: no free-disk floor is configured, so background builds are "
                  "not capacity-guarded (a max_bytes cache still evicts on its own "
                  "trigger); set min_free_bytes to bound them\n";
  }
  const bool reclaimFull = cfg.recompressReclaim == Config::Reclaim::kFull;
  std::vector<CacheStore::EntryInfo> work;
  std::vector<CacheStore::EntryInfo> reclaimWork; // replicated, v1 copy still on disk
  int preSkipped = 0, preAlready = 0;
  if (drain) {
    const std::string pending = cfg.cacheDir + "/recompress.pending";
    const std::string batch = cfg.cacheDir + "/recompress.working";
    // A batch left behind by a pass that was killed mid-flight: nothing ever
    // read this file again, so those keys were lost until someone ran an
    // explicit sweep. Fold it into this batch instead. Safe under the pass lock:
    // only the holder can be looking at it.
    std::set<std::string> seen;
    auto absorb = [&](const std::string& path) {
      std::ifstream in(path);
      std::string line;
      while (std::getline(in, line))
        if (!line.empty() && seen.insert(line).second) {
          CacheStore::EntryInfo e;
          e.key = line;
          work.push_back(e);
        }
    };
    struct ::stat wst;
    const bool orphan = io.stat(batch, &wst) == 0;
    if (orphan)
      absorb(batch); // recovered from a previous pass that died
    if (::rename(pending.c_str(), batch.c_str()) == 0)
      absorb(batch);
    ::unlink(batch.c_str());
  } else {
    for (const auto& e : store.listEntries()) {
      if (e.cachedBytes == 0 || e.replicaBytes > 0) {
        if (e.replicaBytes > 0)
          ++preAlready; // has a replica: reported apart from "nothing cached"
        else
          ++preSkipped;
        // reclaim full, retroactive half: entries recompressed by EARLIER
        // passes still hold their v1 byte copy (> the kept tail page).
        if (reclaimFull && e.replicaBytes > 0 && e.cachedBytes > cfg.pageSize)
          reclaimWork.push_back(e);
      } else {
        work.push_back(e);
      }
    }
  }
  if (drain && work.empty()) {
    closeFds();
    return 0;
  }
  // Capacity pre-flight (sweep only): state what this sweep is likely to cost
  // against the headroom to the eviction trigger. ADVISORY, and deliberately so
  // since the per-file budget above now guards both modes: the estimate exists
  // to tell a human what they are about to spend an hour on, not to decide
  // whether the pass is safe. It cannot decide that honestly — it is a flat
  // upper bound (1.4x of the candidates' resident bytes), while the measured
  // ratio ranges from 1.04x to 1.45x with the source codec AND the container, so
  // refusing on it declines sweeps that fit. One field run refused a sweep
  // estimated at 148.3 GiB against 134.7 GiB of headroom; the replicas needed
  // 110.3 GiB, and the campaign reported a full results table with an empty
  // replica tier. What protects the floor is the per-file claim, which defers
  // exactly the files that do not fit and reports them as `deferred (no space)`.
  if (!drain && !work.empty()) {
    uint64_t candBytes = 0;
    for (const auto& e : work)
      candBytes += e.cachedBytes;
    uint64_t reclaimCredit = 0;
    if (reclaimFull) {
      reclaimCredit = candBytes;
      for (const auto& e : reclaimWork)
        reclaimCredit += e.cachedBytes;
    }
    const uint64_t estReplicas = estimatedReplicaBytes(candBytes);
    const uint64_t growth = estReplicas > reclaimCredit ? estReplicas - reclaimCredit : 0;
    const uint64_t headroom = headroomToFloor(cfg, io);
    uint64_t avail = 0, total = 0;
    io.spaceInfo(cfg.cacheDir, avail, total); // for the arithmetic printed below
    if (growth > headroom) {
      std::fprintf(stderr,
                   "recompress: this sweep may not fit entirely — each file that does not is "
                   "deferred, never evicted around:\n"
                   "  candidates        : %zu file(s), %s cached\n"
                   "  replicas, est.    : up to %s (1.4x of the cached bytes — an UPPER bound; "
                   "measured 1.04x-1.45x by source codec and container)\n"
                   "  reclaimed, est.   : %s (%s)\n"
                   "  net growth, est.  : up to %s\n"
                   "  headroom to floor : %s (disk free %s, eviction floor %s)\n"
                   "the pass builds what fits and defers the rest (`deferred (no space)` in the "
                   "summary below). To fit more, free space first (`ucache evict --to-size`, "
                   "`rm`, or `recompress_reclaim = full` to replace byte copies).\n",
                   work.size(), human(candBytes).c_str(), human(estReplicas).c_str(),
                   human(reclaimCredit).c_str(),
                   reclaimFull ? "reclaim full" : "reclaim superseded: counted as 0 — "
                                                  "punch size is unknowable before the build",
                   human(growth).c_str(), human(headroom).c_str(), human(avail).c_str(),
                   human(cfg.minFreeBytes).c_str());
      if (yes)
        std::fputs("proceeding (--yes; the estimate is advisory either way)\n", stderr);
    }
  }
  if (jobs < 1)
    jobs = 1;
  if (static_cast<size_t>(jobs) > work.size())
    jobs = work.empty() ? 1 : static_cast<int>(work.size());
  // Background capacity budget. Headroom is re-measured per file rather than
  // seeded once: the drainer is spawned BY a live job that is still filling the
  // same volume, so "this pass is the only writer" is false exactly when it
  // matters. What the pass must track itself is only the bytes it has promised
  // but not yet written — once a build lands, statvfs sees it.
  // Guard applies whenever eviction is enabled at all; ~0ull means it is off.
  // Applies to BOTH modes. It used to be background-only, on the reasoning that
  // a sweep asks the user up front — but an up-front estimate is not a guard:
  // once approved, nothing re-checked the floor, so a sweep whose estimate was
  // low (or that ran beside another writer) evicted anyway; and a sweep whose
  // estimate was HIGH built nothing at all, which is how a field run produced a
  // complete-looking results table with an empty replica tier. The per-file
  // claim below is the real protection — live headroom, one file at a time —
  // and it works the same whoever asked for the pass.
  const bool budgetGuard = headroomToFloor(cfg, io) != ~0ull;
  std::atomic<uint64_t> inFlight{0};
  std::atomic<int> deferred{0};
  std::atomic<uint64_t> deferBytes{0};
  std::vector<std::string> requeue; // deferred keys, put back under outMu
  std::atomic<size_t> next{0};
  std::atomic<int> done{0}, skipped{preSkipped}, failed{0}, incomplete{0};
  std::atomic<int> declined{0}, already{preAlready};
  std::set<std::string> declinedCodecs; // observed source codecs, under outMu
  std::atomic<uint64_t> totOverlay{0}, totPunched{0};
  std::atomic<uint64_t> decB{0}, decNs{0};
  std::atomic<size_t> processed{0};
  // 30 s: long enough that a batch makes real progress at ~2 s/file, short
  // enough that a user's sweep is never left guessing. The plugin respawns a
  // worker every 15 s while a job is running, so continuity does not depend on
  // one pass draining everything.
  static constexpr time_t kDrainBudget = 30;
  // Test hook: a gate cannot wait 30 s per leg, and the property under test —
  // "the remainder is requeued, never dropped" — is independent of the value.
  time_t budgetS = kDrainBudget;
  if (const char* b = ::getenv("UCACHE_TEST_DRAIN_BUDGET_S"); b && *b)
    budgetS = static_cast<time_t>(std::strtoll(b, nullptr, 10));
  const time_t deadline = ::time(nullptr) + budgetS;
  std::atomic<bool> overBudget{false};
  std::mutex outMu;
  const bool progress = mode == RecompressMode::kSweep && ::isatty(2);
  auto worker = [&] {
    ReplicaStore rs(io, cfg, store.stats());
    for (;;) {
      // The background pass gives itself a deadline. Whatever it does not reach
      // goes back on the queue, so a foreground sweep never waits longer than
      // one budget plus the file in flight — the property that makes a single
      // plain lock sufficient. Checked BEFORE claiming an index, so every index
      // handed out is processed and the untouched tail is exactly [next, end).
      if (drain && ::time(nullptr) >= deadline) {
        overBudget = true;
        break;
      }
      const size_t i = next.fetch_add(1);
      if (i >= work.size())
        break;
      size_t nDone = processed.fetch_add(1) + 1;
      if (progress)
        std::fprintf(stderr, "\rrecompress: %zu/%zu — %d recompressed, %d nothing to do, %d failed",
                     nDone, work.size(), done.load(), skipped.load(), failed.load());
      const auto& e = work[i];
      auto key = UrlKey::parse(e.key, cfg.keepCgi);
      if (!key) {
        std::lock_guard<std::mutex> g(outMu);
        std::fprintf(stderr, "recompress: build failed: not a usable cache key: %s\n",
                     e.key.c_str());
        ++failed;
        continue;
      }
      {
        struct ::stat st;
        if (io.stat(ReplicaStore::tmetaPath(*key, cfg.cacheDir), &st) == 0) {
          ++already; // replica exists (or appeared while this pass ran)
          continue;
        }
      }
      auto cm = MetaFile::load(io, key->metaPath(cfg.cacheDir));
      if (!cm) {
        std::lock_guard<std::mutex> g(outMu);
        std::fprintf(stderr,
                     "recompress: build failed: %s has no readable cache sidecar "
                     "(evicted, or never cached)\n",
                     key->key.c_str());
        ++failed;
        continue;
      }
      // Capacity budget: a replica this pass writes must fit in the headroom
      // above the eviction floor. Neither mode may be the thing that pushes a
      // volume into eviction, and neither can know up front what it will cost —
      // so both decide here, per file, against live headroom. Deciding per file
      // rather than per batch
      // matters under `recompress_reclaim = full`, where each build hands back
      // the v1 copy and so pays for the next one.
      // Charge the FULL estimate, with no credit for the byte copy that
      // `reclaim = full` will hand back: a background pass punches nothing until
      // after its workers have joined, so that space is not free during the very
      // window this guard exists to bound.
      const uint64_t est = budgetGuard ? estimatedReplicaBytes(cm->cachedBytes()) : 0;
      InFlightClaim claim; // releases on every exit from this iteration
      if (budgetGuard) {
        uint64_t promised = inFlight.load(std::memory_order_relaxed);
        bool claimed = false;
        for (;;) {
          const uint64_t head = headroomToFloor(cfg, io); // live, per file
          if (est + promised > head || est + promised < est)
            break; // no room now; a later pass may have it
          if (inFlight.compare_exchange_weak(promised, promised + est,
                                             std::memory_order_relaxed)) {
            claim = InFlightClaim{&inFlight, est};
            claimed = true;
            break;
          }
        }
        if (!claimed) { // leave it queued rather than evict the user's cache
          ++deferred;
          deferBytes += est;
          std::lock_guard<std::mutex> g(outMu);
          requeue.push_back(e.key);
          continue;
        }
      }
      // Parse + hot set once; gate before any transcode work.
      // Publishing is identical whatever produced the overlay, so both the
      // basket and the page builder end here rather than each growing a copy.
      auto publishOverlay = [&](const tp::Overlay& ov) -> bool {
        ReplicaMeta meta = ov.meta;
        meta.originMtime = cm->originMtime;
        meta.cksumKind = cm->cksumKind;
        meta.originCksum = cm->originCksum;
        if (meta.originSize != cm->fileSize ||
            rs.publish(*key, meta, ov.tdata.data(), ov.tdata.size()) != 0)
          return false;
        decB += ov.decodeBytes;
        decNs += ov.decodeNs;
        auto view = rs.openView(*key, meta.originSize, meta.originMtime, cm->cksumKind,
                                cm->originCksum);
        if (!view)
          return false;
        uint64_t punchedB = 0;
        if (mode == RecompressMode::kSweep) // background never punches live v1
          if (auto entry = store.open(*key, meta.originSize, meta.originMtime, cm->cksumKind,
                                      cm->originCksum))
            punchedB = punchPerReclaim(rs, *entry, view->meta(), cfg);
        totOverlay += ov.tdata.size();
        totPunched += punchedB;
        ++done;
        return true;
      };

      tp::FileMeta fm = tp::parseFile(key->dataPath(cfg.cacheDir), "Events");
      // An RNTuple container has no TTree to find, so the tree parse failing is
      // the FIRST hint that this might be one. Only then is the RNTuple parse
      // attempted, which keeps the cost off every TTree entry and leaves that
      // path's behaviour exactly as it was.
      tp::RNTupleMeta rm;
      bool isRNTuple = false;
      if (!fm.error.empty()) {
        rm = tp::parseRNTuple(key->dataPath(cfg.cacheDir), "");
        isRNTuple = rm.error.empty();
      }
      if (!fm.error.empty() && !isRNTuple) {
        // The parser reads the header, keys list and tree key from the sparse
        // image DIRECTLY, without the bitmap gate and checksum that basket reads
        // go through, so on a partially cached entry a hole surfaces here as
        // "not a ROOT file" or similar. Coverage tells the two apart: on a fully
        // cached entry the file really is malformed. Either way SAY so — this
        // path used to increment `failed` and print nothing at all.
        const bool complete = cm->bitmap.count() == cm->npages();
        std::lock_guard<std::mutex> g(outMu);
        if (complete) {
          std::fprintf(stderr, "recompress: build failed: parse: %s\n", fm.error.c_str());
          ++failed;
        } else {
          std::fprintf(stderr,
                       "recompress: not built yet, will retry: parse: %s "
                       "(entry only partially cached)\n",
                       fm.error.c_str());
          ++incomplete;
        }
        continue;
      }
      int fd = ::open(key->dataPath(cfg.cacheDir).c_str(), O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        std::lock_guard<std::mutex> g(outMu);
        std::fprintf(stderr, "recompress: build failed: cannot open %s (%s)\n",
                     key->dataPath(cfg.cacheDir).c_str(), std::strerror(errno));
        ++failed;
        continue;
      }
      CacheSource csrc;
      csrc.fd = fd;
      csrc.meta = &*cm;
      if (isRNTuple) {
        // Pages are relocated per (cluster, column) range, to ZSTD-1 — the
        // same target the basket path uses.
        auto rw = tp::buildRNTupleRewrite(rm, csrc, rm.fileSize, 1, cfg.recompressCodecs);
        if (rw.rangesRelocated == 0) {
          // Same three-way distinction the basket path draws, for the same
          // reason: "nothing happened" hides whether the policy declined the
          // file, nothing was cached, or there was nothing to do.
          ::close(fd);
          if (rw.rangesDeclined > 0) {
            ++declined;
            std::lock_guard<std::mutex> g(outMu);
            if (!rw.declinedCodec.empty())
              declinedCodecs.insert(rw.declinedCodec);
          } else {
            ++skipped;
          }
          continue;
        }
        tp::Overlay ov = tp::rnTupleOverlay(rm, rw);
        ::close(fd);
        if (!ov.error.empty()) {
          std::lock_guard<std::mutex> g(outMu);
          std::fprintf(stderr, "recompress: build failed: %s\n", ov.error.c_str());
          ++failed;
          continue;
        }
        if (!publishOverlay(ov)) ++failed;
        continue;
      }
      auto hot = tp::deriveHotBranches(fm, csrc, cfg.recompressCodecs);
      if (hot.empty()) {
        // Nothing to transcode here. Two very different reasons, and conflating
        // them is what made a 100%-declined dataset read as "already optimal":
        // the entry may hold no fully-cached content at all, or its content may
        // be in a codec the policy does not list. In the second case the codec
        // we DID see is the one fact that tells the user what to change, and it
        // is already in hand — so record it.
        std::string seen;
        for (const auto& b : fm.branches)
          if (tp::fullyCached(b, csrc)) {
            seen = tp::branchCodec(fm, b, csrc);
            if (!seen.empty())
              break;
          }
        if (!seen.empty()) {
          ++declined;
          std::lock_guard<std::mutex> g(outMu);
          declinedCodecs.insert(seen);
        } else {
          ++skipped;
        }
        ::close(fd);
        continue;
      }
      tp::Overlay ov = tp::buildOverlay(fm, csrc, hot);
      ::close(fd);
      if (!ov.error.empty()) {
        // "Not available" is not a failure: the entry is incompletely cached,
        // or bytes were reclaimed under the build. It retries for free on the
        // next pass, and calling it `build failed` in the log makes a run that
        // in fact succeeded read like a broken one.
        std::lock_guard<std::mutex> g(outMu);
        if (ov.transient && csrc.sawRot) {
          // Present-but-corrupt bytes: waiting cannot fix this one, so it is a
          // failure with a remedy rather than a retry.
          std::fprintf(stderr,
                       "recompress: build failed: %s — cached bytes failed their checksum "
                       "(run `ucache verify %s`)\n",
                       ov.error.c_str(), e.key.c_str());
          ++failed;
        } else if (ov.transient) {
          std::fprintf(stderr, "recompress: not built yet, will retry: %s\n", ov.error.c_str());
          ++incomplete;
        } else {
          std::fprintf(stderr, "recompress: build failed: %s\n", ov.error.c_str());
          ++failed;
        }
        continue;
      }
      if (!publishOverlay(ov))
        ++failed;
    }
  };
  // A throw escaping a std::thread terminates the process, and this worker
  // allocates (strings, set/vector inserts). For the detached background pass
  // that would abort mid-batch; for a sweep it would kill the user's command.
  // Count it and carry on — the pass is fail-open by design.
  auto guardedWorker = [&] {
    try {
      worker();
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> g(outMu);
      std::fprintf(stderr, "recompress: worker aborted: %s\n", e.what());
      ++failed;
    } catch (...) {
      std::lock_guard<std::mutex> g(outMu);
      std::fputs("recompress: worker aborted\n", stderr);
      ++failed;
    }
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < jobs; ++t) {
    try {
      pool.emplace_back(guardedWorker);
    } catch (const std::system_error&) { // out of threads: run with what we have
      break;
    }
  }
  guardedWorker();
  for (auto& t : pool)
    t.join();
  if (progress)
    std::fputs("\r\033[K", stderr);
  // Retroactive reclaim (sweep only; the scan above filled reclaimWork
  // only under `recompress_reclaim = full`): punch the v1 byte copy of entries
  // whose replica was built by an earlier pass. Only a replica that VALIDATES
  // against the entry's origin metadata licenses dropping the v1 bytes.
  uint64_t reclaimedB = 0;
  int reclaimedN = 0;
  if (!reclaimWork.empty()) {
    ReplicaStore rs(io, cfg, store.stats());
    for (size_t i = 0; i < reclaimWork.size(); ++i) {
      if (progress)
        std::fprintf(stderr, "\rreclaim: %zu/%zu — %s freed", i + 1, reclaimWork.size(),
                     human(reclaimedB).c_str());
      auto key = UrlKey::parse(reclaimWork[i].key, cfg.keepCgi);
      if (!key)
        continue;
      auto cm = MetaFile::load(io, key->metaPath(cfg.cacheDir));
      if (!cm)
        continue;
      if (auto view = rs.openView(*key, cm->fileSize, cm->originMtime, cm->cksumKind,
                                  cm->originCksum))
        if (auto entry = store.open(*key, cm->fileSize, cm->originMtime, cm->cksumKind,
                                    cm->originCksum))
          if (uint64_t b = punchPerReclaim(rs, *entry, view->meta(), cfg)) {
            reclaimedB += b;
            ++reclaimedN;
          }
    }
    if (progress)
      std::fputs("\r\033[K", stderr);
  }
  // Deferred for want of space: put the keys back, so a later pass builds them
  // once room appears (eviction, a manual `rm`, or `reclaim = full` handing back
  // v1 copies as it goes). Appended whole-line to the same queue the plugin
  // appends to. Losing them would be defensible — an explicit sweep scans the
  // whole cache anyway — but then background recompression would silently stop
  // making progress on a volume that later has room.
  // Whatever the deadline stopped us from reaching goes back too. Dropping it
  // and trusting a sweep to rescan was wrong twice over: the sweep can refuse
  // (capacity pre-flight on a non-tty, or the user answering no), and an emptied
  // queue also silences the `status`/`doctor` explanation, which keys off queue
  // depth — no builds AND no diagnosis.
  if (overBudget.load())
    for (size_t i = next.load(); i < work.size(); ++i)
      requeue.push_back(work[i].key);
  if (!requeue.empty()) {
    // One write of whole lines: the plugin appends to this same file with a
    // single O_APPEND write per line, and a buffered stream would flush at an
    // arbitrary byte boundary and let a key be torn in half.
    std::string blob;
    for (const auto& k : requeue)
      blob += k + "\n";
    const std::string pendingPath = cfg.cacheDir + "/recompress.pending";
    if (int qfd = ::open(pendingPath.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        qfd >= 0) {
      size_t off = 0;
      while (off < blob.size()) { // a short write would tear a line too
        ssize_t w = ::write(qfd, blob.data() + off, blob.size() - off);
        if (w <= 0)
          break;
        off += static_cast<size_t>(w);
      }
      ::close(qfd);
    }
  }
  if (deferred.load() && drainLog)
    drainLog << "recompress: deferred " << deferred.load() << " file(s) for want of space — "
             << "would add up to " << human(deferBytes.load()) << " with "
             << human(headroomToFloor(cfg, io))
             << " of headroom above the eviction floor now. Left queued; "
             << "free space (`ucache evict --to-size`, `ucache rm`) or set "
             << "recompress_reclaim = full to replace byte copies instead of adding to them.\n";
  if (decNs.load() > 0) // calibrate from what we actually decoded
    writeCalibration(cfg, static_cast<double>(decB.load()) * 1000.0 /
                              static_cast<double>(decNs.load()));
  std::string codecs;
  for (const auto& codec : cfg.recompressCodecs)
    codecs += (codecs.empty() ? "" : ",") + codec;
  // `incomplete` is reported apart from both buckets: unlike "nothing to do"
  // it may become work later, and unlike "failed" it is nobody's bug.
  std::string seenCodecs;
  for (const auto& c : declinedCodecs)
    seenCodecs += (seenCodecs.empty() ? "" : ",") + c;
  // Every state gets its own words. "nothing to do" used to absorb three
  // unrelated outcomes — already done, declined by codec policy, nothing
  // cached — so a dataset the policy declined outright read as a healthy cache.
  char declinedSeg[256] = {0};
  if (declined.load())
    std::snprintf(declinedSeg, sizeof declinedSeg,
                  ", %d declined (source codec %s, not in recompress_codecs = %s)",
                  declined.load(), seenCodecs.c_str(), codecs.c_str());
  char alreadySeg[64] = {0};
  if (already.load())
    std::snprintf(alreadySeg, sizeof alreadySeg, ", %d already recompressed", already.load());
  char incompleteSeg[96] = {0};
  if (incomplete.load())
    std::snprintf(incompleteSeg, sizeof incompleteSeg, ", %d incomplete (bytes not cached)",
                  incomplete.load());
  char deferredSeg[96] = {0};
  if (deferred.load())
    std::snprintf(deferredSeg, sizeof deferredSeg, ", %d deferred (no space)", deferred.load());
  char summary[1024];
  std::snprintf(summary, sizeof summary,
                "recompress: %d recompressed%s%s, %d nothing to do (nothing cached)%s%s, "
                "%d failed%s",
                done.load(), declinedSeg, alreadySeg, skipped.load(), incompleteSeg, deferredSeg,
                failed.load(),
                overBudget.load() ? " — batch time budget reached, remainder requeued" : "");
  if (!drain) {
    std::printf("%s\n", summary);
    // Declined everything and built nothing: that is a configuration mismatch,
    // not an optimal cache, and the user cannot act on it without being told
    // both codecs and the command that reconciles them.
    if (!done.load() && declined.load() && !seenCodecs.empty())
      std::printf("  nothing was recompressed: this cache holds %s content, but "
                  "recompress_codecs = %s.\n"
                  "  to transcode it:  ucache set recompress_codecs %s\n",
                  seenCodecs.c_str(), codecs.c_str(), seenCodecs.c_str());
    if (done.load())
      std::printf("  overlays on disk: %s; punched %s of %s\n",
                  human(totOverlay.load()).c_str(), human(totPunched.load()).c_str(),
                  reclaimFull ? "v1 bytes (reclaim full)" : "superseded v1 pages");
    if (reclaimedN)
      std::printf("  reclaimed %s from %d already-recompressed entr%s "
                  "(recompress_reclaim = full)\n",
                  human(reclaimedB).c_str(), reclaimedN, reclaimedN == 1 ? "y" : "ies");
  }
  if (drain && drainLog)
    drainLog << summary << "\n";
  if (drain) {
    // Deferred punch: reclaim the superseded v1 pages of every replica in the
    // cache, this pass's builds included — deferred to here, after the workers
    // have joined, so that no build in this pass can be reading the pages it
    // frees. Punching is crash-safe and v1 readers fail open to origin.
    uint64_t punched = 0;
    ReplicaStore rs(io, cfg, store.stats());
    for (const auto& e : store.listEntries()) {
      if (e.replicaBytes == 0)
        continue;
      auto key = UrlKey::parse(e.key, cfg.keepCgi);
      if (!key)
        continue;
      auto cm = MetaFile::load(io, key->metaPath(cfg.cacheDir));
      if (!cm)
        continue;
      if (auto view = rs.openView(*key, cm->fileSize, cm->originMtime, cm->cksumKind,
                                  cm->originCksum))
        if (auto entry = store.open(*key, cm->fileSize, cm->originMtime, cm->cksumKind,
                                    cm->originCksum))
          punched += punchPerReclaim(rs, *entry, view->meta(), cfg);
    }
    if (punched && drainLog)
      drainLog << "punched " << punched
               << (reclaimFull ? " v1 bytes (reclaim full)\n" : " superseded v1 bytes\n");
    closeFds();
    // One bounded batch per invocation, then exit. This used to recurse to pick
    // up whatever was queued while it built, which made the pass unbounded in
    // both wall time and stack depth (each level kept its work list and log
    // stream open, and nothing capped the descent), and it is what forced the
    // whole yield mechanism into existence. Continuity comes from the plugin
    // instead: it respawns a worker every 15 s while a job is closing files, and
    // an explicit `ucache recompress` covers anything left over — it scans the
    // cache rather than the queue.
  }
  closeFds();
  // Any build failure is a non-zero exit, not just a total one. A field run
  // finished 1416 of 1456 entries with 40 deterministic `build failed` lines
  // and exited 0, so a script had no way to know the dataset was 97.3% covered
  // -- and partial coverage buys almost nothing, because the unreplicated
  // remainder paces the next read pass. Declined (codec) and incomplete (bytes
  // not cached) are legitimate outcomes and stay silent in the exit code.
  if (!drain && failed.load())
    std::printf("  %d entr%s failed to build: coverage is INCOMPLETE. A warm pass "
                "over this cache is a mixture of tiers, not a replica measurement.%s\n",
                failed.load(), failed.load() == 1 ? "y" : "ies",
                strict ? "" : "  (--strict to exit non-zero on this)");
  // A partial build failure is a WARNING, not an error. Nothing is broken by it:
  // uCache fails open, so an entry without a replica simply serves from the byte
  // tier. Exiting non-zero would break a driver script over a condition the
  // cache handles gracefully. What the original complaint needed was to be able
  // to TELL that coverage stopped short -- that is the line above, and the
  // benchmark harness's coverage verdict. `--strict` is there for a caller that
  // genuinely wants coverage enforced.
  //
  // Unchanged without --strict: a sweep where nothing at all built still exits
  // non-zero, which is the long-standing behaviour.
  return (failed.load() && (strict || !done.load())) ? 1 : 0;
#endif
}

// `ucache branches <url>`: which branches the analysis actually
// read — the branch-level answer behind ls's COV%. For each branch of the
// cached entry: fully cached? source codec? bytes. Summary = K of N branches,
// share of branch bytes.
int cmdBranches(const Config& cfg, IOBackend& io, const char* url) {
#ifndef UCACHE_HAVE_TRANSPOSE
  (void)cfg;
  (void)io;
  (void)url;
  std::fputs("branches: built without the native transposer (codec libs missing)\n", stderr);
  return 2;
#else
  auto key = UrlKey::parse(url, cfg.keepCgi);
  if (!key) {
    std::fprintf(stderr, "%s: not a valid URL\n", url);
    return 2;
  }
  auto cm = MetaFile::load(io, key->metaPath(cfg.cacheDir));
  if (!cm) {
    std::fprintf(stderr, "branches: %s is not in the byte cache\n", key->key.c_str());
    return 1;
  }
  const std::string dataPath = key->dataPath(cfg.cacheDir);
  tp::FileMeta fm = tp::parseFile(dataPath, "Events");
  if (!fm.error.empty()) {
    std::fprintf(stderr, "branches: parse failed: %s\n", fm.error.c_str());
    return 1;
  }
  int fd = ::open(dataPath.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::fprintf(stderr, "branches: cannot open %s\n", dataPath.c_str());
    return 1;
  }
  CacheSource csrc;
  csrc.fd = fd;
  csrc.meta = &*cm;
  size_t hotN = 0;
  uint64_t hotBytes = 0, allBytes = 0;
  std::printf("%-7s %-9s %-6s  %s\n", "READ", "BYTES", "CODEC", "BRANCH");
  for (const auto& b : fm.branches) {
    uint64_t bytes = 0;
    for (int32_t i = 0; i < b.writeBasket; ++i)
      bytes += static_cast<uint64_t>(b.basketBytes[i]);
    allBytes += bytes;
    const bool hot = tp::fullyCached(b, csrc);
    if (hot) {
      ++hotN;
      hotBytes += bytes;
      std::printf("%-7s %-9s %-6s  %s\n", "yes", human(bytes).c_str(),
                  tp::branchCodec(fm, b, csrc).c_str(), b.name.c_str());
    }
  }
  ::close(fd);
  std::printf("%zu of %zu branches fully read (%.1f%% of branch bytes; %s of %s)\n", hotN,
              fm.branches.size(),
              allBytes ? 100.0 * static_cast<double>(hotBytes) / static_cast<double>(allBytes)
                       : 0.0,
              human(hotBytes).c_str(), human(allBytes).c_str());
  return 0;
#endif
}

int cmdUntranspose(CacheStore& store, const Config& cfg, IOBackend& io, const char* url) {
  auto key = UrlKey::parse(url, cfg.keepCgi); // SAME normalization as the plugin
  if (!key) {
    std::fprintf(stderr, "%s: not a valid URL\n", url);
    return 2;
  }
  ReplicaStore rs(io, cfg, store.stats());
  struct ::stat st;
  if (io.stat(ReplicaStore::tmetaPath(*key, cfg.cacheDir), &st) != 0) {
    std::fprintf(stderr, "%s: no replica (nothing to drop)\n", url);
    return 1;
  }
  rs.drop(*key);
  std::printf("dropped replica for %s (byte cache kept; punched pages refetch on demand)\n",
              key->key.c_str());
  return 0;
}

void printCleanup(const CacheStore::CleanupReport& rep, bool dryRun, const char* verb) {
  if (dryRun) {
    uint64_t now = static_cast<uint64_t>(::time(nullptr));
    for (const auto& v : rep.victims)
      std::printf("  would remove %-9s %-6s %s\n", human(v.bytes).c_str(),
                  humanAge(v.atime, now).c_str(), v.key.c_str());
    std::printf("dry run: %zu entr%s (%s) would be removed\n", rep.victims.size(),
                rep.victims.size() == 1 ? "y" : "ies", human(rep.bytes).c_str());
  } else {
    std::printf("%s %zu entr%s (%s freed)\n", verb, rep.victims.size(),
                rep.victims.size() == 1 ? "y" : "ies", human(rep.bytes).c_str());
  }
}

int cmdEvict(CacheStore& store, int argc, char** argv) {
  uint64_t olderThan = 0, newerThan = 0, toSize = 0;
  bool haveOlder = false, haveNewer = false, haveSize = false, dryRun = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--dry-run") {
      dryRun = true;
    } else if (a == "--older-than" || a.rfind("--older-than=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 12);
      if (!v || !parseDurationArg(v, olderThan)) {
        std::fputs("evict: --older-than needs a duration like 30d, 12h, 90m\n", stderr);
        return 2;
      }
      haveOlder = true;
    } else if (a == "--newer-than" || a.rfind("--newer-than=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 12);
      if (!v || !parseDurationArg(v, newerThan)) {
        std::fputs("evict: --newer-than needs a duration like 1h, 30m\n", stderr);
        return 2;
      }
      haveNewer = true;
    } else if (a == "--to-size" || a.rfind("--to-size=", 0) == 0) {
      const char* v = flagValue(argc, argv, i, 9);
      if (!v || !parseSizeArg(v, toSize)) {
        std::fputs("evict: --to-size needs a size like 20g, 500m\n", stderr);
        return 2;
      }
      haveSize = true;
    } else {
      std::fprintf(stderr, "evict: unknown option '%s'\n", a.c_str());
      return 2;
    }
  }
  if (haveOlder + haveNewer + haveSize > 1) {
    std::fputs("evict: use only one of --older-than / --newer-than / --to-size\n", stderr);
    return 2;
  }
  // Plain `evict`: the existing floor/cap pass (LRU to the configured budget).
  if (!haveOlder && !haveNewer && !haveSize) {
    if (dryRun) {
      std::fputs("evict: --dry-run applies to --older-than / --newer-than / --to-size\n", stderr);
      return 2;
    }
    int n = store.evictNow();
    if (n < 0) {
      std::fputs("evict: another process holds the eviction lock; try again\n", stderr);
      return 1;
    }
    std::printf("evicted %d entr%s\n", n, n == 1 ? "y" : "ies");
    return 0;
  }
  // Criterion-based cleanup. Pinned entries are protected (the point of a pin).
  auto mode = haveOlder ? CacheStore::CleanupMode::kOlderThan
              : haveNewer ? CacheStore::CleanupMode::kNewerThan
                          : CacheStore::CleanupMode::kToSize;
  auto rep = store.cleanup(mode, haveOlder ? olderThan : (haveNewer ? newerThan : toSize),
                           /*keepPinned=*/true, dryRun);
  if (!rep.locked) {
    std::fputs("evict: another process holds the eviction lock; try again\n", stderr);
    return 1;
  }
  printCleanup(rep, dryRun, "evicted");
  return 0;
}

int cmdRm(CacheStore& store, const Config& cfg, int argc, char** argv) {
  if (argc < 3) {
    std::fputs("rm: needs at least one <url>\n", stderr);
    return 2;
  }
  int missing = 0, bad = 0;
  for (int i = 2; i < argc; ++i) {
    auto key = UrlKey::parse(argv[i], cfg.keepCgi); // SAME normalization as the plugin
    if (!key) {
      std::fprintf(stderr, "%s: not a valid URL\n", argv[i]);
      ++bad;
      continue;
    }
    if (store.removeEntry(*key)) {
      std::printf("removed %s\n", key->key.c_str());
    } else {
      std::fprintf(stderr, "%s: not cached\n", argv[i]);
      ++missing;
    }
  }
  // Non-zero if any requested URL could not be acted on (like rm(1)): a
  // malformed URL is a usage error (2); an already-absent one is 1.
  if (bad)
    return 2;
  return missing ? 1 : 0;
}

int cmdClear(CacheStore& store, int argc, char** argv) {
  bool yes = false, keepPinned = false;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--yes" || a == "-y")
      yes = true;
    else if (a == "--keep-pinned")
      keepPinned = true;
    else {
      std::fprintf(stderr, "clear: unknown option '%s'\n", a.c_str());
      return 2;
    }
  }
  // Preview first, so the confirmation prompt can state exactly what goes.
  auto plan = store.cleanup(CacheStore::CleanupMode::kAll, 0, keepPinned, /*dryRun=*/true);
  if (!plan.locked) {
    std::fputs("clear: another process holds the eviction lock; try again\n", stderr);
    return 1;
  }
  if (plan.victims.empty()) {
    std::printf("cache already empty%s\n", keepPinned ? " (all remaining entries pinned)" : "");
    return 0;
  }
  const char* plural = plan.victims.size() == 1 ? "y" : "ies";
  if (!yes) {
    if (!::isatty(STDIN_FILENO)) {
      std::fprintf(stderr,
                   "clear: would remove %zu entr%s (%s)%s. Re-run with --yes to confirm.\n",
                   plan.victims.size(), plural, human(plan.bytes).c_str(),
                   keepPinned ? "; pinned kept" : "");
      return 1;
    }
    uint64_t repl = 0, cachedB = 0;
    for (const auto& e : store.listEntries()) {
      repl += e.replicaBytes;
      cachedB += e.cachedBytes;
    }
    std::printf("Remove ALL %zu entr%s (%s: %s cached + %s recompressed)%s? [y/N] ",
                plan.victims.size(), plural, human(plan.bytes).c_str(),
                human(cachedB).c_str(), human(repl).c_str(),
                keepPinned ? ", keeping pinned" : "");
    std::fflush(stdout);
    char buf[8] = {0};
    if (!std::fgets(buf, sizeof buf, stdin) || (buf[0] != 'y' && buf[0] != 'Y')) {
      std::printf("aborted (nothing removed)\n");
      return 0;
    }
  }
  auto rep = store.cleanup(CacheStore::CleanupMode::kAll, 0, keepPinned, /*dryRun=*/false);
  if (!rep.locked) {
    std::fputs("clear: another process holds the eviction lock; try again\n", stderr);
    return 1;
  }
  std::printf("cleared %zu entr%s (%s freed)%s\n", rep.victims.size(),
              rep.victims.size() == 1 ? "y" : "ies", human(rep.bytes).c_str(),
              keepPinned ? "; pinned kept" : "");
  return 0;
}

int cmdPin(CacheStore& store, const Config& cfg, const char* url, bool pin) {
  auto key = UrlKey::parse(url, cfg.keepCgi); // SAME normalization as the plugin
  if (!key) {
    std::fprintf(stderr, "%s: not a valid URL\n", url);
    return 2;
  }
  if (!store.setPinnedByKey(*key, pin)) {
    std::fprintf(stderr, "%s: not cached (nothing to %s)\n", url, pin ? "pin" : "unpin");
    return 1;
  }
  std::printf("%s %s\n", pin ? "pinned" : "unpinned", key->key.c_str());
  return 0;
}

int cmdVerify(CacheStore& store, const Config& cfg, IOBackend& io, const char* url) {
  auto key = UrlKey::parse(url, cfg.keepCgi);
  if (!key) {
    std::fprintf(stderr, "%s: not a valid URL\n", url);
    return 2;
  }
  // CRITICAL: take size AND mtime/cksum from the entry's OWN sidecar. Passing
  // defaults for mtime/cksum would make open()'s §7 validation (under
  // UCACHE_VALIDATE=size+mtime or cksum) treat the entry as stale and
  // truncate/wipe it instead of scrubbing.
  auto meta = MetaFile::load(io, key->metaPath(cfg.cacheDir));
  if (!meta) {
    std::fprintf(stderr, "%s: not cached\n", url);
    return 1;
  }
  auto r = store.verify(*key, meta->fileSize, meta->originMtime, meta->cksumKind,
                        meta->originCksum);
  std::printf("verified %s: checked=%llu bad=%llu\n", key->key.c_str(),
              (unsigned long long)r.checked, (unsigned long long)r.bad);
  return r.bad ? 1 : 0; // bad pages were quarantined; refetch heals on next read
}

// ---- setup / doctor / enable / disable (zero-admin activation) ----------------

std::string homeDir() {
  if (const char* h = ::getenv("HOME"))
    return h;
  if (struct passwd* pw = ::getpwuid(::getuid()))
    return pw->pw_dir;
  return "/tmp";
}

// Absolute path to the installed plugin .so: UCACHE_PLUGIN_SO override, else
// derived from this binary (bin/ucache -> ../lib{64}), with the build tree as a
// dev fallback.
std::string pluginSoPath() {
  if (const char* v = ::getenv("UCACHE_PLUGIN_SO"))
    return v;
  std::string exe = selfExePath();
  auto dir = [](const std::string& p) {
    auto s = p.rfind('/');
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
  };
  std::string bindir = dir(exe), root = dir(bindir);
  std::vector<std::string> cands = {root + "/lib64/libXrdClUCache.so",
                                    root + "/lib/libXrdClUCache.so",
                                    bindir + "/../plugin/libXrdClUCache.so"};
  struct ::stat st;
  for (auto& c : cands)
    if (::stat(c.c_str(), &st) == 0)
      return c;
  return "";
}

bool writeFileStr(const std::string& path, const std::string& content) {
  std::ofstream f(path, std::ios::trunc);
  f << content;
  return static_cast<bool>(f);
}
bool fileExists(const std::string& p) {
  struct ::stat st;
  return ::stat(p.c_str(), &st) == 0;
}
void mkdirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if ((path[i] == '/' && i > 0) || i == path.size() - 1)
      ::mkdir(cur.c_str(), 0700);
  }
}
// Append `line` to `rc` under a marker (idempotent). True if it added it.
// Does a system conf claim the '*' slot (would shadow our url = *)?
bool systemStarSlotClaimed() {
  DIR* d = ::opendir("/etc/xrootd/client.plugins.d");
  if (!d)
    return false;
  bool claimed = false;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n.size() < 5 || n.compare(n.size() - 5, 5, ".conf") != 0)
      continue;
    std::ifstream in(std::string("/etc/xrootd/client.plugins.d/") + n);
    std::string line;
    while (std::getline(in, line))
      if (line.find("url") != std::string::npos && line.find('=') != std::string::npos &&
          line.find('*') != std::string::npos) {
        claimed = true;
        break;
      }
  }
  ::closedir(d);
  return claimed;
}

const char* kConfSubdir = "/.config/ucache/plugins";

// The home directory as XrdCl resolves it for ~/.xrootd/client.plugins.d:
// the passwd database ONLY — $HOME is never consulted (verified in 5.8.3
// PlugInManager::ProcessEnvironmentSettings and empirically on 5.9.6).
// $HOME is used only if the passwd lookup itself fails.
std::string xrdHome() {
  if (struct passwd* pw = ::getpwuid(::getuid()))
    if (pw->pw_dir && *pw->pw_dir)
      return pw->pw_dir;
  return homeDir();
}

// Value of `key` in a flat `key = value` conf file ("" if absent).
std::string confKey(const std::string& file, const std::string& key) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    auto trim = [](std::string s) {
      auto b = s.find_first_not_of(" \t");
      auto e = s.find_last_not_of(" \t");
      return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    if (trim(line.substr(0, eq)) == key)
      return trim(line.substr(eq + 1));
  }
  return "";
}

// Path of the ucache conf in `dir` (any *.conf naming our plugin lib), or "".
std::string ucacheConfIn(const std::string& dir) {
  DIR* d = ::opendir(dir.c_str());
  if (!d)
    return "";
  std::string found;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n.size() < 5 || n.compare(n.size() - 5, 5, ".conf") != 0)
      continue;
    std::ifstream in(dir + "/" + n);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (content.find("libXrdClUCache") != std::string::npos) {
      found = dir + "/" + n;
      break;
    }
  }
  ::closedir(d);
  return found;
}

// The governing ucache plugin conf — the one config file. Searched
// in XrdCl's precedence order, highest first: $XRD_PLUGINCONFDIR (processed
// last by XrdCl, so it wins), the default user dir ~/.xrootd/client.plugins.d
// (passwd home, then $HOME), then the system dir. "" = none found.
std::string findUCacheConf() {
  if (const char* env = ::getenv("XRD_PLUGINCONFDIR")) {
    std::string p = ucacheConfIn(env);
    if (!p.empty())
      return p;
  }
  {
    std::string p = ucacheConfIn(xrdHome() + "/.xrootd/client.plugins.d");
    if (!p.empty())
      return p;
  }
  return ucacheConfIn("/etc/xrootd/client.plugins.d");
}

// ---- Settings model: conf = defaults, state = current, env = job -----------

// Ordered key/value pairs of the CLI-managed state file (machine-written;
// comments/garbage are dropped on rewrite).
std::vector<std::pair<std::string, std::string>> readStateEntries(const std::string& path) {
  std::vector<std::pair<std::string, std::string>> out;
  std::ifstream in(path);
  std::string line;
  auto trim = [](std::string s) {
    auto b = s.find_first_not_of(" \t");
    auto e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
  };
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos)
      line = line.substr(0, hash);
    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (!k.empty() && !v.empty())
      out.emplace_back(k, v);
  }
  return out;
}

int writeStateFile(const std::string& path,
                   const std::vector<std::pair<std::string, std::string>>& kv) {
  if (kv.empty()) { // nothing current: no file at all beats an empty husk
    ::unlink(path.c_str());
    return 0;
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      std::fprintf(stderr, "cannot write %s: %s\n", tmp.c_str(), std::strerror(errno));
      return 1;
    }
    out << "# ucache CURRENT values — managed by `ucache set`/`ucache unset`.\n"
           "# Do not edit by hand: defaults belong in the plugin conf (ucache.conf).\n";
    for (const auto& [k, v] : kv)
      out << k << " = " << v << "\n";
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    std::fprintf(stderr, "cannot publish %s: %s\n", path.c_str(), std::strerror(errno));
    return 1;
  }
  return 0;
}

int cmdSettings(const Config& cfg) {
  std::string conf = findUCacheConf();
  std::printf("defaults : %s\n",
              conf.empty() ? "(no plugin conf found — built-ins only)" : conf.c_str());
  if (cfg.cacheDir.empty())
    std::printf("current  : (cache dir not set — no state file)\n");
  else {
    const std::string sp = Config::statePath(cfg.cacheDir);
    struct ::stat st;
    std::printf("current  : %s%s\n", sp.c_str(),
                ::stat(sp.c_str(), &st) == 0 ? "" : " (nothing set — values from defaults/env)");
  }
  std::printf("\n%-22s %-28s %s\n", "key", "value", "source");
  for (const auto& k : Config::knownKeys()) {
    if (std::strcmp(k.key, "log") == 0)
      continue; // write-only: configures the logger
    auto it = cfg.sources.find(k.key);
    std::printf("%-22s %-28s %s\n", k.key, cfg.valueOf(k.key).c_str(),
                it == cfg.sources.end() ? "default" : it->second.c_str());
  }
  std::printf("\ncurrent values: `ucache set <key> <value>` / `ucache unset <key>` "
              "(state file, no conf edit);\ndefaults: edit the conf file by hand; "
              "UCACHE_* env overrides everything for one job.\n");
  return 0;
}

int cmdSet(const Config& cfg, const char* key, const char* value) {
  const std::string k = key;
  if (!Config::stateSettable(k)) {
    if (k == "dir")
      std::fputs("set: 'dir' cannot live in the state file (the file sits inside the "
                 "cache dir) — set it in ucache.conf or UCACHE_DIR\n",
                 stderr);
    else
      std::fprintf(stderr, "set: unknown key '%s' — `ucache settings` lists the vocabulary\n",
                   key);
    return 2;
  }
  if (cfg.cacheDir.empty()) {
    std::fputs("set: no cache dir configured — set `dir =` in ucache.conf (USER_GUIDE §2) "
               "or UCACHE_DIR first\n",
               stderr);
    return 2;
  }
  mkdirs(cfg.cacheDir);
  const std::string sp = Config::statePath(cfg.cacheDir);
  auto kv = readStateEntries(sp);
  bool replaced = false;
  for (auto& e : kv)
    if (e.first == k) {
      e.second = value;
      replaced = true;
    }
  if (!replaced)
    kv.emplace_back(k, value);
  if (int rc = writeStateFile(sp, kv))
    return rc;
  std::printf("%s = %s   (current value, %s)\n"
              "  applies to processes started from now on; `ucache unset %s` reverts to "
              "your defaults\n",
              key, value, sp.c_str(), key);
  const std::string v = value;
  if (k == "recompress" && (v == "on" || v == "1" || v == "true"))
    std::printf("  already-cached data is not queued retroactively — run `ucache recompress` "
                "once to transcode it now\n");
  return 0;
}

int cmdUnset(const Config& cfg, const char* key) {
  if (cfg.cacheDir.empty()) {
    std::fputs("unset: no cache dir configured — set `dir =` in ucache.conf or UCACHE_DIR\n",
               stderr);
    return 2;
  }
  const std::string sp = Config::statePath(cfg.cacheDir);
  auto kv = readStateEntries(sp);
  auto before = kv.size();
  kv.erase(std::remove_if(kv.begin(), kv.end(),
                          [&](const auto& e) { return e.first == key; }),
           kv.end());
  if (kv.size() == before) {
    std::printf("unset: '%s' has no current value (already from defaults/env)\n", key);
    return 0;
  }
  if (int rc = writeStateFile(sp, kv))
    return rc;
  std::printf("%s unset — back to your defaults (conf/built-in)\n", key);
  return 0;
}

// One file, nothing else: write the plugin conf — activation AND
// settings, with the cache dir explicit — into XrdCl's default user plugin
// dir, which every root:// process scans. No environment variable, no shell
// startup file edit, no second config.
int cmdSetup(int argc, char** argv) {
  std::string host = "*", dir;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc)
      host = argv[++i];
    else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc)
      dir = argv[++i];
  }
  std::string so = pluginSoPath();
  if (so.empty()) {
    std::fputs("setup: plugin library not found; set UCACHE_PLUGIN_SO\n", stderr);
    return 1;
  }
  if (dir.empty())
    dir = Config::fromEnv(findUCacheConf()).cacheDir; // honor UCACHE_DIR / an existing conf
  if (dir.empty()) {
    std::fputs("setup: no cache dir — pass --dir /local/disk/path (there is deliberately "
               "no default; use a LOCAL filesystem, not AFS/NFS)\n",
               stderr);
    return 2;
  }
  const std::string pdir = xrdHome() + "/.xrootd/client.plugins.d";
  mkdirs(pdir);
  const std::string conf = pdir + "/ucache.conf";
  if (!writeFileStr(conf, "# written by `ucache setup` — the only file uCache needs (USER_GUIDE §2)\n"
                          "url = " + host + "\n"
                          "lib = " + so + "\n"
                          "enable = true\n"
                          "\n"
                          "# ucache settings (UCACHE_* environment variables override; see\n"
                          "# USER_GUIDE §Configuration for all keys)\n"
                          "dir = " + dir + "\n")) {
    std::fprintf(stderr, "setup: cannot write %s\n", conf.c_str());
    return 1;
  }
  std::printf("ucache setup:\n  plugin : %s\n  conf   : %s (url = %s)\n  cache  : %s\n"
              "\nThat one file is the whole activation and configuration — every root://\n"
              "process picks it up (batch jobs included), no shell reload needed.\n"
              "Edit it to change settings; check with `ucache doctor`.\n",
              so.c_str(), conf.c_str(), host.c_str(), dir.c_str());
  if (host == "*" && systemStarSlotClaimed())
    std::fprintf(stderr, "\nWARNING: a /etc/xrootd/client.plugins.d conf claims the '*' slot and "
                         "shadows `url = *`. Re-run `ucache setup --host <host:port>`.\n");
  return 0;
}

int fsProbe(const std::string& dir) {
  int bad = 0;
  mkdirs(dir);
  std::string probe = dir + "/.ucache-doctor-probe";
  int fd = ::open(probe.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    std::printf("  [FAIL] cache dir not writable: %s\n", dir.c_str());
    return 1;
  }
  bool sparse = false;
  if (::ftruncate(fd, 1 << 20) == 0) {
    struct ::stat st;
    if (::fstat(fd, &st) == 0)
      sparse = static_cast<uint64_t>(st.st_blocks) * 512 < (1u << 20);
  }
  std::printf("  [%s] sparse files (cache stores sparse .data)\n", sparse ? " OK " : "WARN");
  bad += !sparse;
  bool flockOk = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
  if (flockOk)
    ::flock(fd, LOCK_UN);
  std::printf("  [%s] advisory locks (flock)\n", flockOk ? " OK " : "WARN");
  bad += !flockOk;
  ::close(fd);
  ::unlink(probe.c_str());
  return bad;
}
// Probe the library XrdCl will ACTUALLY load — the conf's `lib =` path when a
// conf exists (a stale lib path must fail here, not silently at runtime);
// falls back to the install-relative plugin for the pre-activation case.
int soProbe(const std::string& confLib) {
  std::string so = confLib.empty() ? pluginSoPath() : confLib;
  if (so.empty()) {
    std::printf("  [FAIL] plugin library not found (set UCACHE_PLUGIN_SO)\n");
    return 1;
  }
  const char* what = confLib.empty() ? "plugin loads" : "plugin loads (lib = from conf)";
  void* h = ::dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    std::printf("  [FAIL] plugin not loadable: %s\n", ::dlerror());
    return 1;
  }
  bool sym = ::dlsym(h, "XrdClGetPlugIn") != nullptr; // -z nodelete: no dlclose
  std::printf("  [%s] %s (%s)\n", sym ? " OK " : "WARN", what, so.c_str());
  return sym ? 0 : 1;
}

// Parse the first vX.Y.Z in a string into {major,minor,patch}; false if none.
bool parseXrdVersion(const std::string& s, int v[3]) {
  size_t i = 0;
  while ((i = s.find('v', i)) != std::string::npos) {
    if (std::sscanf(s.c_str() + i + 1, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3)
      return true;
    ++i;
  }
  return false;
}
// The handshake version the plugin declares (XrdVERSIONINFO), embedded in the
// .so as "@V:XrdClUCache vX.Y.Z" — the same string packaging validates. Read
// it straight from the file so the check adapts to whatever the plugin was
// built against (no build-time coupling to the CLI).
std::string pluginDeclaredVersion(const std::string& so) {
  std::ifstream f(so, std::ios::binary);
  if (!f)
    return "";
  std::string blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const std::string marker = "@V:XrdClUCache ";
  size_t p = blob.find(marker);
  if (p == std::string::npos)
    return "";
  size_t e = blob.find('\0', p);
  return blob.substr(p + marker.size(), (e == std::string::npos ? blob.size() : e) - p - marker.size());
}
// The XRootD client actually present in this environment — the one that would
// load (or refuse) the plugin. `xrdcp` ships with the client and tracks its
// version. "" if unavailable/unparseable.
std::string ambientClientVersion() {
  FILE* p = ::popen("xrdcp --version 2>&1", "r");
  if (!p)
    return "";
  char buf[256] = {0};
  size_t n = std::fread(buf, 1, sizeof buf - 1, p);
  ::pclose(p);
  buf[n] = '\0';
  int v[3];
  return parseXrdVersion(buf, v) ? std::string(buf).substr(0, std::string(buf).find_first_of("\r\n"))
                                 : "";
}
// Handshake compatibility: XrdCl loads a plugin only when the plugin's declared
// version is <= the client's — a NEWER-looking plugin is refused SILENTLY and
// the job runs uncached. Catch that here instead of letting it fail invisibly.
int handshakeProbe(const std::string& confLib) {
  std::string so = confLib.empty() ? pluginSoPath() : confLib;
  if (so.empty())
    return 0; // soProbe already reported the missing library
  std::string decl = pluginDeclaredVersion(so);
  std::string client = ambientClientVersion();
  int dv[3], cv[3];
  if (decl.empty() || !parseXrdVersion("v" + decl, dv)) {
    std::printf("  [WARN] could not read the plugin's handshake version from %s\n", so.c_str());
    return 0;
  }
  if (client.empty() || !parseXrdVersion(client, cv)) {
    std::printf("  [WARN] XRootD client version unknown (xrdcp not on PATH?) — cannot verify "
                "handshake; the plugin needs a client >= %s\n",
                decl.c_str());
    return 0;
  }
  bool clientNewerOrEqual = (cv[0] > dv[0]) || (cv[0] == dv[0] && (cv[1] > dv[1] ||
                            (cv[1] == dv[1] && cv[2] >= dv[2])));
  if (clientNewerOrEqual) {
    std::printf("  [ OK ] XRootD client %s >= plugin handshake floor %s\n", client.c_str(),
                decl.c_str());
    return 0;
  }
  if (cv[0] < 5 && dv[0] >= 5) {
    std::printf("  [FAIL] XRootD client %s predates the 5.x plugin ABI (needs libXrdCl.so.3); this "
                "plugin cannot load here AT ALL and jobs run uncached. Use a client/framework "
                "built against XRootD >= %s.\n",
                client.c_str(), decl.c_str());
  } else {
    std::printf("  [FAIL] XRootD client %s is OLDER than this plugin (built for %s); XrdCl "
                "silently refuses a plugin newer than the client, so jobs run UNCACHED. Use a "
                "client/framework built against XRootD >= %s (newer framework builds ship it).\n",
                client.c_str(), decl.c_str(), decl.c_str());
  }
  return 1;
}
// Activation = any plugin conf XrdCl will process that loads ucache
// (USER_GUIDE §2). Probe the dirs XrdCl scans — verified order (5.8.3 pin and
// host 5.9.6): /etc/xrootd/client.plugins.d, then ~/.xrootd/client.plugins.d
// (home resolved via passwd, NOT $HOME), then $XRD_PLUGINCONFDIR (last wins).
// The legacy `setup` layout is only reachable through the env var.
// Returns the number of problems (counted into doctor's verdict).
// Verdict for a FOUND conf: activation is only real if the conf is enabled
// and its url binding is not shadowed by a system '*' claim.
int confVerdict(const std::string& p, const char* how) {
  std::string en = confKey(p, "enable");
  if (!en.empty() && en != "true") {
    std::printf("  [WARN] conf found (%s) but enable = %s — run `ucache enable`\n", p.c_str(),
                en.c_str());
    return 1;
  }
  // A user conf binding '*' is skipped when a system conf claims that slot
  // (verified: PlugInManager registers '*' first-wins) — activation is dead.
  if (p.rfind("/etc/xrootd/", 0) != 0 &&
      confKey(p, "url").find('*') != std::string::npos && systemStarSlotClaimed()) {
    std::printf("  [FAIL] %s binds url = * but a system conf claims the '*' slot — your conf "
                "is shadowed; bind explicit hosts (url = host:port;host:port)\n",
                p.c_str());
    return 1;
  }
  // Name the bound hosts: plugin selection is per-URL-host, so a host missing
  // from this list is read UNCACHED, silently (fail-open) — the first thing to
  // check when a job's file never shows up in `ucache ls`.
  std::string hosts = confKey(p, "url");
  std::printf("  [ OK ] activation: %s (%s)\n           hosts: %s\n", p.c_str(), how,
              hosts.empty() ? "(none?)" : hosts.c_str());
  return 0;
}

int activationProbe() {
  const char* env = ::getenv("XRD_PLUGINCONFDIR");
  if (env) {
    std::string p = ucacheConfIn(env);
    if (!p.empty())
      return confVerdict(p, "via XRD_PLUGINCONFDIR");
  }
  {
    std::string p = ucacheConfIn(xrdHome() + "/.xrootd/client.plugins.d");
    if (!p.empty())
      return confVerdict(p, "default plugin dir — no env var needed");
  }
  {
    std::string p = ucacheConfIn("/etc/xrootd/client.plugins.d");
    if (!p.empty())
      return confVerdict(p, "system-wide");
  }
  const std::string pdir = homeDir() + kConfSubdir;
  if (fileExists(pdir + "/ucache.conf")) {
    std::printf("  [WARN] conf present (%s) but XRD_PLUGINCONFDIR not set in this shell — "
                "move it to ~/.xrootd/client.plugins.d (needs no env var) or export the "
                "variable in your shell startup file\n",
                pdir.c_str());
    return 1;
  }
  std::printf("  [FAIL] no plugin conf%s — write one yourself (USER_GUIDE §2) or run "
              "`ucache setup`\n",
              env ? " (XRD_PLUGINCONFDIR is set but holds no ucache conf)" : "");
  return 1;
}
int cmdDoctor(const Config& cfg) {
  std::string conf = findUCacheConf();
  std::printf("ucache doctor\n  cache dir: %s\n  settings : %s\n  freshness: %s\n",
              cfg.cacheDir.empty() ? "(not set)" : cfg.cacheDir.c_str(),
              conf.empty() ? "built-in defaults + UCACHE_* env (no plugin conf found)"
                           : (conf + " (+ UCACHE_* env overrides)").c_str(),
              freshnessSummary(cfg).c_str());
  // Current-values layer: never invisible — name every override.
  if (!cfg.cacheDir.empty()) {
    size_t n = 0;
    std::string keys;
    for (const auto& s : cfg.sources)
      if (s.second == "state") {
        ++n;
        keys += (keys.empty() ? "" : ", ") + s.first;
      }
    if (n)
      std::printf("  state    : %s — %zu current value%s overriding your defaults (%s); "
                  "`ucache settings` shows all\n",
                  Config::statePath(cfg.cacheDir).c_str(), n, n == 1 ? "" : "s", keys.c_str());
  }
  int problems = 0;
  if (cfg.cacheDir.empty()) {
    // Deliberately no default: an unset cache dir must be loud.
    std::printf("  [FAIL] cache dir not set — add `dir = /local/disk/path` to ucache.conf "
                "(USER_GUIDE §2) or set UCACHE_DIR; the plugin runs uncached until then\n");
    ++problems;
  } else
    problems += fsProbe(cfg.cacheDir);
  problems += soProbe(conf.empty() ? "" : confKey(conf, "lib")) + activationProbe();
  problems += handshakeProbe(conf.empty() ? "" : confKey(conf, "lib"));
  if (conf.empty() && systemStarSlotClaimed())
    std::printf("  [WARN] /etc/xrootd/client.plugins.d claims the '*' slot; bind explicit hosts "
                "(url = host:port) or use `setup --host`\n");
  // Recompression enabled, work queued, nothing built: the user's symptom is
  // silence, so this is where the reason belongs. `deep` — doctor may parse a
  // cached file to compare its codec against the policy; status may not.
  if (!cfg.cacheDir.empty() && cfg.recompress) {
    RealIO dio;
    size_t queued = 0;
    {
      std::ifstream pend(cfg.cacheDir + "/recompress.pending");
      std::string line;
      while (std::getline(pend, line))
        if (!line.empty())
          ++queued;
    }
    const size_t replicas = anyReplicaExists(cfg.cacheDir) ? 1 : 0;
    if (std::string why = recompressStall(cfg, dio, queued, replicas, /*deep=*/true);
        !why.empty()) {
      if (queued)
        std::printf("  [WARN] recompress = on but no replicas exist (%zu queued): %s\n", queued,
                    why.c_str());
      else
        std::printf("  [WARN] recompress = on but no replicas exist: %s\n", why.c_str());
      ++problems;
    }
  }
  std::printf(problems ? "\n%d problem(s) found.\n" : "\nall checks passed.\n", problems);
  return problems ? 1 : 0;
}
// ---- `ucache test <url>`: end-to-end self-test of the REAL setup -----------
// Contract (user, 2026-07-16): tests the configuration exactly as it stands —
// no changes, no temp confs, only the URL as input; byte-level whole-file
// passes with visible progress (xrdcp's own bar, passes announced); cleans up
// the entry it created — but an entry that existed BEFORE the test is the
// user's data and is kept (the test then verifies warm serving only).

// Run one pass through a real XrdCl client. The child gets
// XRD_CPUSEPGWRTRD=0 because plain xrdcp transfers with PgRead, which the
// cache deliberately passes through (§4.7) — everything else is inherited
// untouched: the point is to exercise the user's setup as-is.
uint64_t nowUs() {
  struct timespec ts;
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
}

int runTestPass(const std::string& url, std::vector<pid_t>& pids) {
  pid_t pid = ::fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    ::setenv("XRD_CPUSEPGWRTRD", "0", 1);
    ::execlp("xrdcp", "xrdcp", "-f", url.c_str(), "/dev/null", static_cast<char*>(nullptr));
    std::fprintf(stderr, "test: cannot run xrdcp: %s — XRootD client tools must be on PATH\n",
                 std::strerror(errno));
    ::_exit(127);
  }
  pids.push_back(pid);
  int st = 0;
  ::waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// The test's xrdcp children each leave a stats jsonl (normal per-process
// bookkeeping) — remove exactly those (matched by child pid) so the test
// doesn't skew the user's future `ucache stats` reading.
void removeTestStats(const std::string& statsDir, const std::vector<pid_t>& pids) {
  DIR* d = ::opendir(statsDir.c_str());
  if (!d)
    return;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    for (pid_t p : pids)
      if (n.find("-" + std::to_string(p) + "-") != std::string::npos) {
        ::unlink((statsDir + "/" + n).c_str());
        break;
      }
  }
  ::closedir(d);
}

double mb(uint64_t b) { return static_cast<double>(b) / 1e6; }

int cmdTest(CacheStore& store, const Config& cfg, int argc, char** argv) {
  if (argc < 3) {
    std::fputs("test: needs a <url> your conf intercepts, e.g. "
               "ucache test root://host:port//path/file.root\n",
               stderr);
    return 2;
  }
  const std::string url = argv[2];
  auto key = UrlKey::parse(url, cfg.keepCgi); // SAME normalization as the plugin
  if (!key) {
    std::fprintf(stderr, "test: not a valid root:// URL: %s\n", url.c_str());
    return 2;
  }
  const std::string conf = findUCacheConf();
  if (conf.empty()) {
    std::fputs("test: no plugin conf found — run `ucache doctor` and fix activation first\n",
               stderr);
    return 1;
  }
  // Binding sanity: warn up front if the conf can't intercept this URL.
  // key->key is "scheme://host:port/path" — extract host:port.
  const std::string binding = confKey(conf, "url");
  std::string hostport = key->key;
  if (auto ss = hostport.find("://"); ss != std::string::npos)
    hostport = hostport.substr(ss + 3);
  hostport = hostport.substr(0, hostport.find('/'));
  if (binding.find('*') == std::string::npos &&
      binding.find(hostport) == std::string::npos)
    std::printf("[WARN] conf binds url = %s — %s is not covered; expect no caching below\n",
                binding.c_str(), hostport.c_str());

  const std::string statsDir = cfg.cacheDir + "/stats";
  const bool existed = fileExists(key->objectDir(cfg.cacheDir) + "/" + key->hashHex + ".meta");
  if (existed)
    std::printf("note: already cached — verifying warm serving only; the entry is yours "
                "and will be KEPT.\n");
  int fails = 0;

  std::vector<pid_t> pids;
  std::printf("== pass 1/2: %s read (whole file via xrdcp) ==\n", existed ? "warm" : "cold");
  StatsTotals s0 = aggregateStats(statsDir);
  uint64_t t0 = nowUs();
  int rc = runTestPass(url, pids);
  double dt1 = static_cast<double>(nowUs() - t0) / 1e6;
  StatsTotals s1 = aggregateStats(statsDir);
  uint64_t origin1 = s1.originBytes - s0.originBytes, wrote1 = s1.pageWrites - s0.pageWrites,
           hit1 = s1.hitBytes - s0.hitBytes;
  if (rc != 0) {
    std::printf("FAIL: read failed (xrdcp rc=%d) — is the URL readable at all?\n", rc);
    if (!existed)
      store.removeEntry(*key); // drop any partial entry
    removeTestStats(statsDir, pids);
    return 1;
  }
  std::printf("PASS: read OK in %.1f s (origin %.1f MB, cache-hit %.1f MB)\n", dt1, mb(origin1),
              mb(hit1));
  if (!existed && origin1 == 0 && wrote1 == 0 && hit1 == 0) {
    std::printf("FAIL: the plugin did not engage (no cache traffic recorded) — check "
                "`ucache doctor` and that the conf's url binding covers %s\n",
                hostport.c_str());
    removeTestStats(statsDir, pids);
    return 1;
  }

  std::printf("== pass 2/2: warm read (must be served entirely from cache) ==\n");
  StatsTotals s2 = aggregateStats(statsDir);
  t0 = nowUs();
  rc = runTestPass(url, pids);
  double dt2 = static_cast<double>(nowUs() - t0) / 1e6;
  StatsTotals s3 = aggregateStats(statsDir);
  uint64_t origin2 = s3.originBytes - s2.originBytes, hit2 = s3.hitBytes - s2.hitBytes;
  if (rc != 0) {
    std::printf("FAIL: warm read failed (xrdcp rc=%d)\n", rc);
    ++fails;
  } else if (origin2 == 0 && hit2 > 0) {
    std::printf("PASS: warm read served %.1f MB from cache in %.1f s — zero origin contact\n",
                mb(hit2), dt2);
  } else {
    std::printf("FAIL: warm read touched the origin (origin %.1f MB, cache-hit %.1f MB) — "
                "caching is not effective for this URL\n",
                mb(origin2), mb(hit2));
    ++fails;
  }

  if (!existed) {
    if (store.removeEntry(*key))
      std::printf("cleaned up: test entry removed from the cache\n");
  }
  removeTestStats(statsDir, pids);
  std::printf(fails ? "\nucache test: FAILED\n" : "\nucache test: OK — caching works end to end\n");
  return fails ? 1 : 0;
}

int cmdEnableDisable(bool enable) {
  std::string conf = findUCacheConf();
  if (conf.empty()) // legacy setup layout not reachable via env? still try it
    conf = homeDir() + kConfSubdir + "/ucache.conf";
  std::ifstream in(conf);
  if (!in) {
    std::fprintf(stderr, "%s: no plugin conf found — run `ucache setup` or write one (USER_GUIDE §2)\n",
                 enable ? "enable" : "disable");
    return 1;
  }
  std::string line, out;
  bool flipped = false;
  while (std::getline(in, line)) {
    if (line.find("enable") != std::string::npos && line.find('=') != std::string::npos) {
      out += std::string("enable = ") + (enable ? "true" : "false") + "\n";
      flipped = true;
    } else
      out += line + "\n";
  }
  in.close();
  if (!flipped)
    out += std::string("enable = ") + (enable ? "true" : "false") + "\n";
  if (!writeFileStr(conf, out)) {
    std::fprintf(stderr, "cannot write %s\n", conf.c_str());
    return 1;
  }
  std::printf("caching %s\n", enable ? "enabled" : "disabled");
  return 0;
}

} // namespace

#ifndef UCACHE_VERSION
#define UCACHE_VERSION "unknown"
#endif
#ifndef UCACHE_BUILD_ID
#define UCACHE_BUILD_ID UCACHE_VERSION
#endif

int main(int argc, char** argv) {
  if (argc >= 2 && (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-V") == 0 ||
                    std::strcmp(argv[1], "version") == 0)) {
    // The build id is shown here, not just in benchmark records, because this is
    // where a reader looks to find out what they are running: the version alone
    // is a constant between releases and names many different binaries. When it
    // reads as a bare version rather than a tag description, the build could not
    // identify its own revision — deliberately visible instead of formatted away.
    std::printf("ucache %s (build %s)\n", UCACHE_VERSION, UCACHE_BUILD_ID);
    return 0;
  }
  if (argc < 2 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0 ||
      std::strcmp(argv[1], "help") == 0) {
    usage();
    return argc < 2 ? 2 : 0;
  }
  const std::string cmd = argv[1];
  // One config file: settings live in the same plugin conf XrdCl
  // processes; the CLI reads the one that governs, exactly as XrdCl would.
  Config cfg = Config::fromEnv(findUCacheConf());
  IOBackend& io = RealIO::instance();

  // setup/enable/disable manage conf files; doctor must diagnose a missing
  // cache dir — none need a store (avoids creating the cache dir to run them).
  if (cmd == "setup")
    return cmdSetup(argc, argv);
  if (cmd == "enable")
    return cmdEnableDisable(true);
  if (cmd == "disable")
    return cmdEnableDisable(false);
  if (cmd == "doctor")
    return cmdDoctor(cfg);
  // Settings model — none of these need (or may create) a store.
  if (cmd == "settings")
    return cmdSettings(cfg);
  if (cmd == "set") {
    if (argc != 4) {
      std::fputs("set: usage: ucache set <key> <value>\n", stderr);
      return 2;
    }
    return cmdSet(cfg, argv[2], argv[3]);
  }
  if (cmd == "unset") {
    if (argc != 3) {
      std::fputs("unset: usage: ucache unset <key>\n", stderr);
      return 2;
    }
    return cmdUnset(cfg, argv[2]);
  }
  // bench: storage self-test of the cache dir or any candidate dirs.
  // Explicit paths need no config at all; the default path is the cache dir.
  if (cmd == "bench")
    return cmdBench(cfg, argc, argv);
  if (cmd == "netbench")
    return cmdNetbench(argc, argv);
  // materialize's pure builder mode (--from-file + --overlay-out) is a
  // file->file transform: no cache touched, no cache dir required or created.
  if (cmd == "materialize") {
    bool ff = false, oo = false;
    for (int i = 2; i < argc; ++i) {
      if (!std::strcmp(argv[i], "--from-file"))
        ff = true;
      else if (!std::strcmp(argv[i], "--overlay-out"))
        oo = true;
    }
    if (ff && oo)
      return cmdMaterialize(nullptr, cfg, io, argc, argv);
  }

  // Everything else operates on the cache, and there is deliberately no
  // default location.
  if (cfg.cacheDir.empty()) {
    std::fprintf(stderr,
                 "ucache %s: no cache dir configured — set `dir =` in ucache.conf "
                 "(USER_GUIDE §2) or UCACHE_DIR\n",
                 cmd.c_str());
    return 2;
  }
  if (cmd == "stats") { // pure file reads, no store
    if (argc > 2 && !std::strcmp(argv[2], "--files")) {
      size_t top = 20;
      if (argc > 4 && !std::strcmp(argv[3], "--top"))
        top = static_cast<size_t>(std::max(1, ::atoi(argv[4])));
      return cmdStatsFiles(cfg, top);
    }
    if (argc > 2 && !std::strcmp(argv[2], "--reset")) {
      // Reset = delete the per-process jsonl dumps: the counters are pure
      // diagnostics, and a fresh window is how you measure ONE analysis run
      // against a specific cache state. A process still running keeps writing
      // to its (now unlinked) file and those numbers are lost — warn when a
      // file was written moments ago, since that usually means a live job.
      const std::string sdir = cfg.cacheDir + "/stats";
      DIR* d = ::opendir(sdir.c_str());
      if (!d) {
        std::puts("stats reset: nothing to remove");
        return 0;
      }
      int removed = 0, live = 0;
      const time_t now = ::time(nullptr);
      while (struct dirent* de = ::readdir(d)) {
        if (de->d_name[0] == '.')
          continue;
        const std::string p = sdir + "/" + de->d_name;
        struct ::stat st;
        if (::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
          if (now - st.st_mtime < 10)
            ++live;
          if (::unlink(p.c_str()) == 0)
            ++removed;
        }
      }
      ::closedir(d);
      std::printf("stats reset: removed %d file(s) — counters start fresh with the "
                  "next process\n",
                  removed);
      if (live)
        std::fprintf(stderr,
                     "warning: %d file(s) were written in the last 10 s — a job may "
                     "still be running; its counters keep accumulating in the deleted "
                     "file and will never be visible\n",
                     live);
      return 0;
    }
    printStats(aggregateStats(cfg.cacheDir + "/stats"));
    return 0;
  }
  if (cmd == "history") // pure file reads, no store
    return cmdHistory(cfg, argc, argv);

  CacheStore store(io, cfg);
  store.disableStatsDump(); // a CLI run must not litter stats/

  if (cmd == "status")
    return cmdStatus(store, io);
  if (cmd == "summary")
    return cmdSummary(store, argc, argv);
  if (cmd == "ls")
    return cmdLs(store, argc, argv);
  if (cmd == "evict")
    return cmdEvict(store, argc, argv);
  if (cmd == "rm")
    return cmdRm(store, cfg, argc, argv);
  if (cmd == "clear")
    return cmdClear(store, argc, argv);
  if (cmd == "pin" || cmd == "unpin") {
    if (argc < 3) {
      std::fprintf(stderr, "%s: needs a <url>\n", cmd.c_str());
      return 2;
    }
    return cmdPin(store, cfg, argv[2], cmd == "pin");
  }
  if (cmd == "verify") {
    if (argc < 3) {
      std::fputs("verify: needs a <url>\n", stderr);
      return 2;
    }
    return cmdVerify(store, cfg, io, argv[2]);
  }
  if (cmd == "branches") {
    if (argc < 3) {
      std::fputs("branches: needs a <url>\n", stderr);
      return 2;
    }
    return cmdBranches(cfg, io, argv[2]);
  }
  if (cmd == "test")
    return cmdTest(store, cfg, argc, argv);
  if (cmd == "untranspose") {
    if (argc < 3) {
      std::fputs("untranspose: needs a <url>\n", stderr);
      return 2;
    }
    return cmdUntranspose(store, cfg, io, argv[2]);
  }
  if (cmd == "materialize")
    return cmdMaterialize(&store, cfg, io, argc, argv);
  if (cmd == "recompress" || cmd == "transpose") { // "transpose --auto" = legacy alias
    // Default parallelism: half the cores (user decision — no artificial cap;
    // a laptop gets cores/2 too, floor 1).
    int jobs =
        static_cast<int>(std::max<unsigned>(1, std::thread::hardware_concurrency() / 2));
    RecompressMode mode = RecompressMode::kSweep; // explicit command = do it, visibly
    bool yes = false, strict = false;
    for (int i = 2; i < argc; ++i) {
      if (!std::strcmp(argv[i], "--jobs") && i + 1 < argc)
        jobs = std::atoi(argv[++i]);
      else if (!std::strcmp(argv[i], "--drain"))
        mode = RecompressMode::kDrain; // internal: the plugin-spawned worker
      else if (!std::strcmp(argv[i], "--auto"))
        ; // legacy no-op
      else if (!std::strcmp(argv[i], "--yes"))
        yes = true; // skip the capacity confirmation (scripts)
      else if (!std::strcmp(argv[i], "--strict"))
        strict = true; // exit non-zero if any entry failed to build
      else if (!std::strcmp(argv[i], "--estimate") || !std::strcmp(argv[i], "--force")) {
        std::fprintf(stderr,
                     "recompress: %s was removed with the evidence gate — bare "
                     "`ucache recompress` now transcodes everything the codec list allows, "
                     "in the foreground\n",
                     argv[i]);
        return 2;
      } else {
        std::fprintf(stderr, "%s: unknown option '%s'\n", cmd.c_str(), argv[i]);
        return 2;
      }
    }
    if (jobs < 1)
      jobs = 1;
    if (mode == RecompressMode::kDrain) {
      // The spawning plugin sizes this from its own thread count
      // (min(cores/2, threads/2)); a fixed clamp of 2 here was what limited
      // background coverage to 8-18% of a large dataset. Still bounded, so a
      // hand-typed --drain cannot oversubscribe the machine.
      unsigned hw = std::thread::hardware_concurrency();
      int cap = hw ? static_cast<int>(hw) : 4;
      jobs = std::min(jobs, cap);
    }
    return cmdRecompress(store, cfg, io, jobs, mode, yes, strict);
  }
  std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
  usage();
  return 2;
}
