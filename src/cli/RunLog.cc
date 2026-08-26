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
  }
}

} // namespace

std::vector<Run> loadRuns(const std::string& statsDir) {
  std::vector<Run> runs;
  DIR* d = ::opendir(statsDir.c_str());
  if (!d)
    return runs;
  std::vector<std::string> stems;
  while (dirent* e = ::readdir(d)) {
    const std::string n = e->d_name;
    if (!endsWith(n, ".jsonl") || endsWith(n, ".files.jsonl") || endsWith(n, ".trace.jsonl"))
      continue;
    stems.push_back(n.substr(0, n.size() - 6));
  }
  ::closedir(d);
  for (const auto& base : stems) {
    Run r;
    if (!parseStem(base, r.host, r.pid, r.startS))
      continue; // not a name this writer produced; leave it alone
    r.stem = statsDir + "/" + base;
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
  // smoke test must not produce a speedup number.
  constexpr uint64_t kMinBytes = 64ull << 20;
  constexpr double kMinOverlap = 0.70;
  // Records carry whole seconds, so a short run cannot be timed well enough to
  // divide: a 20 s span carries up to 5% clock error on its own, and the ratio
  // carries twice that -- which is the whole error budget spent before any
  // measurement. Below this the answer is refused rather than rounded.
  constexpr uint64_t kMinDurationS = 30;

  // A run that fetched more than the cache served it is a FILL. Dividing two
  // fills' spans yields a real ratio -- how much slower this fill was than the
  // last -- but that is not what the cache bought anyone, and printing it under
  // "gain" invites exactly the wrong reading.
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

  // A reference is an EARLIER run that actually fetched these files from the
  // origin, healthily. Its rate is the only measurement of what the origin
  // costs for this data on this machine.
  const Run* best = nullptr;
  uint64_t bestMatchedWire = 0;
  double bestC = 0.0, bestD = 0.0;            // closest near-miss, for the refusal text
  double bestOverlapC = 0.0, bestOverlapD = 0.0; // the CHOSEN reference's overlap
  double bestRefRate = 0.0;
  std::vector<double> candidateRates;
  size_t candidates = 0;

  for (const auto& r : all) {
    if (r.stem == run.stem || r.startS > run.startS)
      continue;
    if (r.originBytes < kMinBytes || r.files.empty())
      continue;
    if (r.durationS() < kMinDurationS)
      continue; // same clock-resolution floor applies to the reference
    if (r.faults())
      continue; // a fill that hit faults is not a baseline for anything

    uint64_t refWireTotal = 0, matchedWire = 0, matchedServed = 0;
    for (const auto& [k, f] : r.files) {
      (void)k;
      refWireTotal += f.wireBytes;
    }
    if (refWireTotal == 0)
      continue;
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
      // Remember the closest near-miss so the refusal can name real numbers.
      if (c + d > bestC + bestD) {
        bestC = c;
        bestD = d;
      }
      continue;
    }
    const double rate =
        static_cast<double>(refWireTotal) / static_cast<double>(r.durationS());
    ++candidates;
    candidateRates.push_back(rate);
    // Among references covering the same files, take the FASTEST. A fill can be
    // slowed by things that are not the origin's doing -- a concurrent
    // recompression pass costs 28% of the fill on an LZMA dataset -- and using
    // that as the baseline credits the cache for our own interference. The
    // fastest observed fetch is the most conservative claim about what the
    // origin costs, which is the right direction for a number that must not
    // overclaim.
    if (rate > bestRefRate) {
      bestRefRate = rate;
      bestMatchedWire = matchedWire;
      best = &r;
      bestOverlapC = c;
      bestOverlapD = d;
    }
  }

  if (!best) {
    // Distinguish "nothing fetched these files" from "something did, but was not
    // doing the same work". They call for different actions, and the second is
    // the one that has misled us before.
    if (bestC > 0.0 || bestD > 0.0) {
      char buf[192];
      std::snprintf(buf, sizeof buf,
                    "the earlier fill and this run covered different files "
                    "(%.0f%% of the fill, %.0f%% of this run) — not comparable",
                    bestC * 100.0, bestD * 100.0);
      g.reason = buf;
    } else {
      g.reason = "no earlier run fetched these files from the origin, so there is "
                 "nothing to compare against";
    }
    return g;
  }

  // The one runtime check on whether the reference fill was itself typical:
  // with several fills to compare, an outlier is visible. With one, it is not,
  // and nothing here pretends otherwise.
  if (candidates >= 3) {
    std::vector<double> rates = candidateRates;
    std::sort(rates.begin(), rates.end());
    const double median = rates[rates.size() / 2];
    if (median > 0 && (bestRefRate > 2.0 * median || bestRefRate < 0.5 * median)) {
      g.reason = "the reference fill's origin rate is an outlier among the fills "
                 "recorded here, so it does not represent the origin";
      return g;
    }
  }

  const double refDur = static_cast<double>(best->durationS());
  const double runDur = static_cast<double>(run.durationS());
  const double originTime = refDur * bestOverlapC; // what the origin charged for these
  const double cacheTime = runDur * bestOverlapD;  // what they cost from the cache now

  g.valid = true;
  g.savedS = originTime - cacheTime;
  g.gain = (runDur + g.savedS) / runDur;
  g.originMBs = bestRefRate / 1e6;
  g.cacheMBs = static_cast<double>(runServedTotal) / runDur / 1e6;
  g.originEquivBytes = bestMatchedWire;
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
    const GainEstimate g = estimateGain(r, runs);
    if (!g.valid)
      continue;
    ++t.runsEstimated;
    t.savedS += g.savedS;
    t.estimatedDurationS += static_cast<double>(r.durationS());
  }
  if (t.runsEstimated && t.estimatedDurationS > 0) {
    t.haveGain = true;
    t.gain = (t.estimatedDurationS + t.savedS) / t.estimatedDurationS;
  }
  return t;
}

} // namespace ucache
