#include "RunLog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>

namespace ucache {
namespace {

// Match the exact `"<key>":<digits>` the counter writers emit. Strings and
// arrays are skipped rather than mis-parsed, which is what keeps this tolerant
// of fields it has never heard of.
uint64_t fieldU64(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  auto p = line.find(needle);
  if (p == std::string::npos)
    return 0;
  p += needle.size();
  uint64_t v = 0;
  while (p < line.size() && line[p] >= '0' && line[p] <= '9')
    v = v * 10 + static_cast<uint64_t>(line[p++] - '0');
  return v;
}

void fieldHist(const std::string& line, const char* key, std::vector<uint64_t>& out) {
  const std::string needle = std::string("\"") + key + "\":[";
  auto p = line.find(needle);
  if (p == std::string::npos)
    return;
  p += needle.size();
  out.clear();
  while (p < line.size() && line[p] != ']') {
    uint64_t v = 0;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9')
      v = v * 10 + static_cast<uint64_t>(line[p++] - '0');
    out.push_back(v);
    if (p < line.size() && line[p] == ',')
      ++p;
  }
}

std::string fieldStr(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  auto p = line.find(needle);
  if (p == std::string::npos)
    return {};
  p += needle.size();
  auto e = line.find('"', p);
  return e == std::string::npos ? std::string() : line.substr(p, e - p);
}

bool endsWith(const std::string& s, const char* suf) {
  const size_t l = std::strlen(suf);
  return s.size() >= l && s.compare(s.size() - l, l, suf) == 0;
}

// `<host>-<pid>-<start>-<seq>`, parsed from the RIGHT: a hostname may contain
// dashes (DESKTOP-NJ0BO90 does), so only the three trailing fields are fixed.
bool parseStem(const std::string& base, std::string& host, uint64_t& pid, uint64_t& startS) {
  std::vector<size_t> dashes;
  for (size_t i = 0; i < base.size(); ++i)
    if (base[i] == '-')
      dashes.push_back(i);
  if (dashes.size() < 3)
    return false;
  const size_t dSeq = dashes[dashes.size() - 1];
  const size_t dStart = dashes[dashes.size() - 2];
  const size_t dPid = dashes[dashes.size() - 3];
  auto num = [&](size_t from, size_t to, uint64_t& out) {
    if (to <= from)
      return false;
    uint64_t v = 0;
    for (size_t i = from; i < to; ++i) {
      if (base[i] < '0' || base[i] > '9')
        return false;
      v = v * 10 + static_cast<uint64_t>(base[i] - '0');
    }
    out = v;
    return true;
  };
  uint64_t seq = 0;
  if (!num(dPid + 1, dStart, pid) || !num(dStart + 1, dSeq, startS) ||
      !num(dSeq + 1, base.size(), seq))
    return false;
  host = base.substr(0, dPid);
  return !host.empty();
}

void loadCounters(const std::string& path, Run& r) {
  std::ifstream in(path);
  std::string line, last;
  while (std::getline(in, line))
    if (!line.empty() && line.back() == '}')
      last = line; // final COMPLETE line; a torn tail from a kill is skipped
  if (last.empty())
    return;
  r.complete = true;
  r.endS = fieldU64(last, "ts");
  r.disabled = fieldU64(last, "disabled") != 0;
  r.handlesHighWater = fieldU64(last, "handles_high_water");
  r.opens = fieldU64(last, "opens");
  r.filesOpened = fieldU64(last, "files_opened");
  r.servedBytes = fieldU64(last, "served_bytes");
  r.hitBytes = fieldU64(last, "hit_bytes");
  r.ramHitBytes = fieldU64(last, "ram_hit_bytes");
  r.replicaBytesServed = fieldU64(last, "replica_bytes_served");
  r.relayBytes = fieldU64(last, "relay_bytes");
  r.originBytes = fieldU64(last, "origin_bytes");
  r.originReads = fieldU64(last, "origin_reads");
  r.originReadvs = fieldU64(last, "origin_readvs");
  r.hitDiskReads = fieldU64(last, "hit_disk_reads");
  r.hitDiskBytes = fieldU64(last, "hit_disk_bytes");
  r.replicaReads = fieldU64(last, "replica_reads");
  r.replicaReadBytes = fieldU64(last, "replica_read_bytes");
  r.crcFailures = fieldU64(last, "crc_failures");
  r.replicaCrcFailures = fieldU64(last, "replica_crc_failures");
  r.replicaInvalid = fieldU64(last, "replica_invalid");
  r.failopenEvents = fieldU64(last, "failopen_events");
  r.metaCorrupt = fieldU64(last, "meta_corrupt");
  r.validationsFailed = fieldU64(last, "validations_failed");
  r.evictedEntries = fieldU64(last, "evicted_entries");
  r.evictedBytes = fieldU64(last, "evicted_bytes");
  r.pageWrites = fieldU64(last, "page_writes");
  r.flushRuns = fieldU64(last, "flush_runs");
  r.flushRunBytes = fieldU64(last, "flush_run_bytes");
  r.bufferStalls = fieldU64(last, "buffer_stalls");
  r.bufferStallUs = fieldU64(last, "buffer_stall_us");
  fieldHist(last, "hist_hit_read_us", r.histHitRead);
  fieldHist(last, "hist_replica_read_us", r.histReplicaRead);
  fieldHist(last, "hist_origin_rt_us", r.histOriginRt);
  fieldHist(last, "hist_open_us", r.histOpen);
}

void loadFiles(const std::string& path, Run& r) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line.back() != '}')
      continue;
    const std::string key = fieldStr(line, "key");
    if (key.empty())
      continue;
    Run::FileRec& f = r.files[key];
    // One record per entry per process, but a re-opened entry can produce a
    // second: accumulate rather than overwrite, so nothing is silently lost.
    f.ts = std::max(f.ts, fieldU64(line, "ts"));
    f.opens += fieldU64(line, "opens");
    f.servedBytes += fieldU64(line, "served_bytes");
    f.ramBytes += fieldU64(line, "ram_bytes");
    f.replicaBytes += fieldU64(line, "replica_bytes");
    f.diskReads += fieldU64(line, "disk_reads");
    f.diskSeq += fieldU64(line, "disk_seq");
    f.diskBytes += fieldU64(line, "disk_bytes");
    f.firstTouchBytes += fieldU64(line, "first_touch_bytes");
    f.wireBytes += fieldU64(line, "wire_bytes");
    f.spanUs += fieldU64(line, "span_us");
    const std::string m = fieldStr(line, "mode");
    // A re-opened entry emits a second record. Modes agree in practice (the
    // mode is a per-key property, not per-open; if they ever disagree, the
    // origin-served mode wins, because a file half-served from cache cannot
    // measure the origin and must not join that population.
    if (!m.empty() && (f.mode.empty() || m == "relay"))
      f.mode = m;
  }
}

} // namespace

std::vector<Run> loadRuns(const std::string& statsDir, const std::string& archiveDir) {
  std::vector<Run> runs;
  std::vector<std::pair<std::string, std::string>> stems; // (dir, stem)
  std::set<std::string> seen; // a stem name is unique per process; never count it twice
  for (const std::string& dir : {statsDir, archiveDir}) {
    if (dir.empty())
      continue;
    DIR* d = ::opendir(dir.c_str());
    if (!d)
      continue;
    while (dirent* e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (!endsWith(n, ".jsonl") || endsWith(n, ".files.jsonl") || endsWith(n, ".trace.jsonl"))
        continue;
      const std::string base = n.substr(0, n.size() - 6);
      if (seen.insert(base).second)
        stems.emplace_back(dir, base);
    }
    ::closedir(d);
  }
  for (const auto& [dir, base] : stems) {
    Run r;
    if (!parseStem(base, r.host, r.pid, r.startS))
      continue; // not a name this writer produced; leave it alone
    r.stem = dir + "/" + base;
    loadCounters(r.stem + ".jsonl", r);
    loadFiles(r.stem + ".files.jsonl", r);
    // A killed job leaves no cumulative line. Its per-file records still say
    // when it was last doing work, which is a better end than "unknown".
    for (const auto& [k, f] : r.files) {
      (void)k;
      r.endS = std::max(r.endS, f.ts);
    }
    if (!r.complete && r.files.empty())
      continue; // an empty claimed name, or a store that never served anything
    runs.push_back(std::move(r));
  }
  std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
    return a.startS != b.startS ? a.startS > b.startS : a.pid > b.pid;
  });
  return runs;
}

GainEstimate estimateGain(const Run& run, const std::vector<Run>& all) {
  GainEstimate g;
  g.runFiles = run.files.size();

  // Floors below which the arithmetic is noise rather than a measurement. A
  // smoke test must not produce a speedup number, and whole-second records
  // cannot time a short span well enough to divide (a 20 s run carries up to
  // 5% clock error before any measurement happens).
  constexpr uint64_t kMinBytes = 64ull << 20;
  constexpr uint64_t kMinDurationS = 30;
  constexpr double kMinOverlap = 0.70;

  if (run.disabled) {
    g.reason = "this run IS a baseline (cache disabled) — it is what others are "
               "measured against";
    return g;
  }
  // A run that fetched more than the cache served it is a FILL. Dividing two
  // walls tells how the fill compared to the baseline -- real, but not what
  // the cache bought anyone, and printing it under "gain" invites exactly the
  // wrong reading.
  if (run.originBytes > run.cacheBytes()) {
    g.reason = "this run filled the cache rather than being served by it";
    return g;
  }
  if (run.cacheBytes() == 0) {
    g.reason = "the cache served nothing in this run";
    return g;
  }
  if (run.cacheBytes() < kMinBytes) {
    g.reason = "too little data moved to measure (under 64 MiB served)";
    return g;
  }
  if (run.files.empty()) {
    g.reason = "no per-file records for this run";
    return g;
  }
  if (run.durationS() < kMinDurationS) {
    g.reason = "the run was too short to time at one-second resolution (under 30 s)";
    return g;
  }

  uint64_t runServedTotal = 0;
  for (const auto& [k, f] : run.files) {
    (void)k;
    runServedTotal += f.servedBytes;
  }
  if (runServedTotal == 0) {
    g.reason = "no served bytes attributed to files in this run";
    return g;
  }

  // The reference is a measured BASELINE: the same files read once with the
  // cache out of the loop. EITHER side of this run in time qualifies -- people
  // usually think to measure a baseline only after they have been running
  // cached for a while -- and among the ones that qualify the NEAREST in time
  // wins, because origin drift is the only thing that separates them (a
  // baseline has no cache machinery in its wall to distort).
  const Run* best = nullptr;
  double bestC = 0.0, bestD = 0.0;         // closest near-miss, for the refusal text
  double refC = 0.0, refD = 0.0, refWireTotalD = 0.0;
  bool sawBaseline = false;

  for (const auto& r : all) {
    if (r.stem == run.stem || !r.disabled)
      continue;
    if (r.files.empty() || r.durationS() < kMinDurationS || r.faults())
      continue;
    uint64_t refWireTotal = 0, matchedWire = 0, matchedServed = 0;
    for (const auto& [k, f] : r.files) {
      (void)k;
      refWireTotal += f.wireBytes;
    }
    if (refWireTotal < kMinBytes)
      continue;
    sawBaseline = true;
    for (const auto& [k, f] : run.files) {
      auto it = r.files.find(k);
      if (it == r.files.end() || it->second.wireBytes == 0)
        continue;
      matchedWire += it->second.wireBytes;
      matchedServed += f.servedBytes;
    }
    if (matchedWire == 0)
      continue;
    const double c = static_cast<double>(matchedWire) / static_cast<double>(refWireTotal);
    const double d = static_cast<double>(matchedServed) / static_cast<double>(runServedTotal);
    if (c < kMinOverlap || d < kMinOverlap) {
      if (c + d > bestC + bestD) { // remember the near-miss for the refusal text
        bestC = c;
        bestD = d;
      }
      continue;
    }
    auto dist = [&](const Run& x) {
      return x.startS > run.startS ? x.startS - run.startS : run.startS - x.startS;
    };
    if (!best || dist(r) < dist(*best)) {
      best = &r;
      refC = c;
      refD = d;
      refWireTotalD = static_cast<double>(refWireTotal);
      g.originEquivBytes = matchedWire;
    }
  }

  if (!best) {
    if (bestC > 0.0 || bestD > 0.0) {
      char buf[192];
      std::snprintf(buf, sizeof buf,
                    "the baseline and this run covered different files "
                    "(%.0f%% of the baseline, %.0f%% of this run) — not comparable",
                    bestC * 100.0, bestD * 100.0);
      g.reason = buf;
    } else if (sawBaseline) {
      g.reason = "no baseline covers these files — run this work once with "
                 "UCACHE_DISABLE=1 to measure one";
    } else {
      g.reason = "no baseline recorded — run the same work once with "
                 "UCACHE_DISABLE=1 (cache out of the loop) to measure what the "
                 "origin alone costs";
    }
    return g;
  }

  const double refDur = static_cast<double>(best->durationS());
  const double runDur = static_cast<double>(run.durationS());
  const double originTime = refDur * refC; // what these files cost with no cache
  const double cacheTime = runDur * refD;  // what they cost from the cache now

  g.valid = true;
  g.savedS = originTime - cacheTime;
  g.gain = (runDur + g.savedS) / runDur;
  g.originMBs = refWireTotalD / refDur / 1e6;
  g.cacheMBs = static_cast<double>(runServedTotal) / runDur / 1e6;
  g.matchedFiles = 0;
  for (const auto& [k, f] : run.files) {
    (void)f;
    if (best->files.count(k))
      ++g.matchedFiles;
  }
  g.referenceStartS = best->startS;
  return g;
}

Totals summarize(const std::vector<Run>& runs, size_t maxEstimates) {
  Totals t;
  std::set<std::string> keys;
  for (const auto& r : runs) {
    ++t.runs;
    t.durationS += r.durationS();
    t.hitBytes += r.hitBytes;
    t.replicaBytes += r.replicaBytesServed;
    t.relayBytes += r.relayBytes;
    t.originBytes += r.originBytes;
    t.faults += r.faults();
    if (t.firstStartS == 0 || r.startS < t.firstStartS)
      t.firstStartS = r.startS;
    if (r.endS > t.lastEndS)
      t.lastEndS = r.endS;
    for (const auto& [k, f] : r.files) {
      (void)f;
      keys.insert(k);
    }
  }
  t.distinctFiles = keys.size();

  // Runs are newest-first, so the cap keeps the recent ones -- the part of the
  // record anyone is actually asking about.
  size_t done = 0;
  for (const auto& r : runs) {
    if (done >= maxEstimates) {
      ++t.gainCapped;
      continue;
    }
    ++done;
    // Prefer what the run measured about itself: both sides of that comparison
    // ran in one process, so a thread-count change or a faster analysis loop
    // cannot leak into it. Fall back to a baseline comparison otherwise.
    const GainEstimate g = estimateGain(r, runs);
    if (!g.valid)
      continue;
    const double gain = g.gain;
    const double dur = static_cast<double>(r.durationS());
    ++t.runsEstimated;
    // saved = what no cache would have cost, minus what this took.
    t.savedS += dur * gain - dur;
    t.estimatedDurationS += dur;
  }
  if (t.runsEstimated && t.estimatedDurationS > 0) {
    t.haveGain = true;
    t.gain = (t.estimatedDurationS + t.savedS) / t.estimatedDurationS;
  }
  return t;
}

} // namespace ucache
