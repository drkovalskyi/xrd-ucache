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
bool hasField(const std::string& line, const char* key) {
  return line.find(std::string("\"") + key + "\":") != std::string::npos;
}

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
    const size_t start = p;
    uint64_t v = 0;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9')
      v = v * 10 + static_cast<uint64_t>(line[p++] - '0');
    if (p == start) {
      // Not a digit, not a separator, not the end: the field is malformed.
      // Consuming nothing here and going round again is an infinite loop that
      // also appends a bucket every pass, so one stray byte mid-line turns
      // into a hang and unbounded memory. A histogram we cannot read is worth
      // nothing, so drop it and leave the rest of the line to be parsed.
      out.clear();
      return;
    }
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
  r.threadsHighWater = fieldU64(last, "threads_high_water");
  r.peakCores = fieldU64(last, "peak_cores");
  r.originReadsInFlight = fieldU64(last, "origin_reads_in_flight_high_water");
  // Presence, separately from value. Both of these are legitimately ZERO -- a
  // warm run asks the origin for nothing, which is the whole point of the
  // counter -- and rendering a measured zero the same as a record too old to
  // carry the field made the one observation the column exists for
  // indistinguishable from no observation at all.
  r.havePeakCores = hasField(last, "peak_cores");
  r.haveOriginReadsInFlight = hasField(last, "origin_reads_in_flight_high_water");
  r.cpuUs = fieldU64(last, "cpu_us");
  r.instructions = fieldU64(last, "instructions");
  r.cycles = fieldU64(last, "cycles");
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
  fieldHist(last, "hist_flush_write_us", r.histFlushWrite);
  fieldHist(last, "hist_meta_flush_us", r.histMetaFlush);
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
    f.originSize = std::max(f.originSize, fieldU64(line, "origin_size"));
    // A footprint only grows, and a file opened twice emits a record per
    // close, so the LAST one is the complete answer -- taking the first would
    // record half of what the run read.
    {
      const uint64_t rb = fieldU64(line, "read_buckets");
      const std::string rs = fieldStr(line, "read_sig");
      if (!rs.empty() && rb >= f.readBuckets) {
        f.readSig = rs;
        f.readBuckets = rb;
      }
    }
    const std::string m = fieldStr(line, "mode");
    // A re-opened entry emits a second record. Modes agree in practice (the
    // mode is a per-key property, not per-open; if they ever disagree, the
    // origin-served mode wins, because a file half-served from cache cannot
    // measure the origin and must not join that population.
    // A file can be reported more than once in a run -- evicted and refetched,
    // or degraded to pass-through part way. Relay wins because it is the one
    // mode that says the cache did NOT serve those bytes, and a file the cache
    // stopped serving should not read as cached. Fill does not override cached
    // for the same reason in reverse: it did serve them, eventually.
    if (!m.empty() && (f.mode.empty() || m == "relay"))
      f.mode = m;
  }
}

} // namespace

namespace {
std::string signatureOf(const std::map<std::string, Run::FileRec>& files);
std::string readSignatureOf(const std::map<std::string, Run::FileRec>& files);
std::string hostOf(const std::string& url);
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
    // Identity: which files this run opened, and where they came from.
    r.sig = signatureOf(r.files);
    r.readSig = readSignatureOf(r.files);
    for (const auto& [k, f] : r.files) {
      (void)f;
      const std::string h = hostOf(k);
      if (!h.empty())
        ++r.originHosts[h];
    }
    runs.push_back(std::move(r));
  }
  std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) {
    return a.startS != b.startS ? a.startS > b.startS : a.pid > b.pid;
  });
  return runs;
}

std::vector<Run> withoutTrivial(std::vector<Run> runs) {
  runs.erase(std::remove_if(runs.begin(), runs.end(),
                            [](const Run& r) { return r.durationS() < kMinListedDurationS; }),
             runs.end());
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
  // Two runs of the same work must have read the same parts of the same
  // files, and that is settled PER FILE rather than by one signature for the
  // whole run. The reason is that the two sides do not always ask for their
  // bytes the same way: a replica is a rewritten container, so where only part
  // of a file was relocated, a read over the original layout can cross a
  // stretch the replica is never asked for, and one whole bucket lands inside
  // that stretch on a few percent of files. Requiring every file to agree
  // turned that into a refused measurement for the entire run.
  //
  // The threshold has room because the two populations are nowhere near each
  // other: the same work over two routes agreed on 94% of files, while two
  // DIFFERENT analyses over the same inputs share only about a sixth of their
  // buckets and so agree on essentially none. Anything from a half to nine
  // tenths separates them; nine tenths is taken because the honest
  // disagreement measured is a few percent, not tens.
  constexpr double kMinReadAgreement = 0.90;

  // ... and how many of the compared files must be checkABLE for that rule to
  // mean anything. Without a floor, ONE agreeing pair out of four hundred
  // matched files admitted the comparison -- which is the defect the agreement
  // rule exists to prevent, one level up: a verdict resting on files nobody
  // has evidence about. A majority is well clear of what the machinery
  // actually produces, which is every file signed in every recorded run.
  //
  // Records written before signatures existed sign NOTHING, on both sides.
  // Those are still compared, because refusing them would retire every
  // measurement taken before the feature -- but the estimate then says the
  // work was not verified instead of implying it was.
  constexpr double kMinSigCoverage = 0.50;

  if (run.disabled) {
    g.why = GainEstimate::Why::kIsBaseline;
    g.reason = "this run IS a baseline (cache disabled) — it is what others are "
               "measured against";
    return g;
  }
  // A run that fetched more than the cache served it is a FILL. Dividing two
  // walls tells how the fill compared to the baseline -- real, but not what
  // the cache bought anyone, and printing it under "gain" invites exactly the
  // wrong reading.
  if (run.originBytes > run.cacheBytes()) {
    g.why = GainEstimate::Why::kFilled;
    g.reason = "this run filled the cache rather than being served by it";
    return g;
  }
  if (run.cacheBytes() == 0) {
    g.why = GainEstimate::Why::kIncomplete;
    g.reason = "the cache served nothing in this run";
    return g;
  }
  if (run.cacheBytes() < kMinBytes) {
    g.why = GainEstimate::Why::kTooSmall;
    g.reason = "too little data moved to measure (under 64 MiB served)";
    return g;
  }
  if (run.files.empty()) {
    g.why = GainEstimate::Why::kIncomplete;
    g.reason = "no per-file records for this run";
    return g;
  }
  if (run.durationS() < kMinDurationS) {
    g.why = GainEstimate::Why::kTooShort;
    g.reason = "the run was too short to time at one-second resolution (under 30 s)";
    return g;
  }

  // Work this run did, over all three tiers, and the part of it the cache
  // itself delivered. Both are needed and they are not the same: coverage asks
  // what fraction of the run's WORK the baseline also covers, while the rate
  // the cache sustained may only count bytes the cache actually handed over.
  // Neither may be built from servedBytes alone -- that is the byte tier only,
  // so a run served from replicas would report a thousandth of its own work
  // and be refused for having done none.
  uint64_t runWorkTotal = 0, runCacheTotal = 0;
  for (const auto& [k, f] : run.files) {
    (void)k;
    runWorkTotal += f.deliveredBytes();
    runCacheTotal += f.cacheDeliveredBytes();
  }
  if (runWorkTotal == 0) {
    g.why = GainEstimate::Why::kIncomplete;
    g.reason = "no bytes attributed to files in this run";
    return g;
  }

  // The reference is a measured BASELINE: the same files read once with the
  // cache out of the loop. EITHER side of this run in time qualifies -- people
  // usually think to measure a baseline only after they have been running
  // cached for a while -- and among the ones that qualify the LATEST wins,
  // because origin drift is the only thing that separates them and the most
  // recent measurement is the best estimate of conditions now (a baseline has
  // no cache machinery in its wall to distort). One consequence, deliberate:
  // recording a new reference re-computes the gain of every earlier run
  // against it, so a printed figure can change with no new data of its own.
  const Run* best = nullptr;
  double bestC = 0.0, bestD = 0.0;         // closest near-miss, for the refusal text
  double refC = 0.0, refD = 0.0, refWireTotalD = 0.0;
  bool sawBaseline = false;

  // Files both runs read, and how many of them they read the same way. Where
  // either side has no footprint for a file -- a replica built before the map
  // existed -- that file is UNKNOWN and counts for neither side, so the
  // file-set match stands on its own rather than refusing outright.
  size_t readSigMismatch = 0;
  double bestAgree = -1.0; // closest agreement seen, for the refusal text
  for (const auto& r : all) {
    // A reference is a run the cache did not appreciably help: one with the
    // cache switched off, or one that QUALIFIES -- the origin did nearly all
    // the work and the cache's own writing cost little. The second kind is
    // what an ordinary first run over new data looks like, so a measurement
    // arrives without anyone having to know to record a special run.
    if (r.stem == run.stem || !(r.disabled || r.baselineQualified()))
      continue;
    if (r.files.empty() || r.durationS() < kMinDurationS || r.faults())
      continue;
    uint64_t refWireTotal = 0, matchedWire = 0, matchedWork = 0;
    size_t sigPairs = 0, sigAgree = 0, matchedCount = 0;
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
      matchedWork += f.deliveredBytes();
      ++matchedCount;
      if (!f.readSig.empty() && !it->second.readSig.empty()) {
        ++sigPairs;
        if (f.readSig == it->second.readSig)
          ++sigAgree;
      }
    }
    if (matchedWire == 0)
      continue;
    // Checked before the coverage arithmetic so that "it read different work"
    // is never reported as "it covered different files": they are different
    // problems with different remedies.
    //
    // Enough of the compared files must carry a footprint on BOTH sides before
    // the agreement rule means anything: agreement measured over the one file
    // that happens to be checkable says nothing about the other three hundred.
    //
    // Below that line the comparison is REPORTED AS UNVERIFIED, not refused.
    // Refusing was worse than the problem: it made the answer non-monotone in
    // evidence. With no signatures anywhere -- every record written before the
    // footprint existed -- the comparison went ahead; adding ONE signature to
    // ONE file of a thousand then destroyed every measurement in the directory.
    // That is reachable without any mixed-era archive, because the emitter
    // decides per FILE whether it knows the size a signature is expressed in.
    // More evidence must never produce a worse answer.
    //
    // So absence of evidence is reported, and only EVIDENCE OF DIFFERENT WORK
    // refuses.
    // Expressed THROUGH the constant, not beside it: the first version
    // hardcoded the comparison and left kMinSigCoverage used only in a message,
    // so changing the constant changed the text and not the rule.
    //
    // KNOWN LIMITATION, measured and left in place deliberately. This floor is
    // a cliff: exhaustive search of the predicate finds cases where adding one
    // more AGREEING pair takes coverage over the line, switches the rule on,
    // and a sub-threshold ratio then refuses a comparison that stood a
    // signature earlier. More evidence FOR the runs matching makes the answer
    // worse, which the paragraph above says must not happen.
    //
    // An attempt to close it (refuse below the floor once some absolute number
    // of files disagree) was reverted, because every way of measuring it made
    // things worse:
    //
    //   - It broke the mixed-era archive this floor was written for. On the
    //     committed 439-file record, stripping signatures from 200 files put
    //     coverage at 44% with 25 disagreements, and the new clause refused a
    //     comparison the old rule accepted -- LESS evidence producing a
    //     refusal, the mirror image of the defect being fixed, on the exact
    //     data that motivated the floor.
    //   - The threshold had no evidence behind it. This project's own measured
    //     benign disagreement rate on identical work is 26 files in 439 (~6%),
    //     so at twenty signed pairs a three-file bar fires on noise about one
    //     time in eight, refusing outright where the tool had been reporting a
    //     gain with "not verified" printed beside it.
    //   - It did not even close the cliff: 50 reachable cases survived, all
    //     with one or two disagreements on file sets under 39.
    //
    // So the cliff stays, bounded and written down, rather than being traded
    // for a refusal that fires on noise. The affected region is narrow -- it
    // needs a sub-90% ratio sitting just under the coverage line -- and the
    // failure is a refusal, which costs a measurement rather than producing a
    // wrong one. If it is ever worth closing, the way through is more evidence
    // per file, not a second threshold on top of this one.
    const bool checkable =
        sigPairs > 0 &&
        static_cast<double>(sigPairs) >= kMinSigCoverage * static_cast<double>(matchedCount);
    if (checkable) {
      const double agree = static_cast<double>(sigAgree) / static_cast<double>(sigPairs);
      if (agree < kMinReadAgreement) {
        ++readSigMismatch;
        if (agree > bestAgree)
          bestAgree = agree;
        continue;
      }
    }
    const double c = static_cast<double>(matchedWire) / static_cast<double>(refWireTotal);
    const double d = static_cast<double>(matchedWork) / static_cast<double>(runWorkTotal);
    if (c < kMinOverlap || d < kMinOverlap) {
      if (c + d > bestC + bestD) { // remember the near-miss for the refusal text
        bestC = c;
        bestD = d;
      }
      continue;
    }
    // A run with the cache switched off beats a qualifying fill, however much
    // more recent the fill is: the fill carries its own writing in its wall
    // -- bounded, but not zero -- while the switched-off run carries none.
    // It also keeps admitting fills from being a silent revision: a dataset
    // that already had a clean baseline reports exactly what it reported
    // before, and fills only ever supply an answer where there was none.
    //
    // Within one kind, the LATEST wins. The conditions a reference describes
    // are the thing that drifts -- the same job against the same origin
    // measured 173 s and 1025 s within one evening -- and the most recent
    // measurement is the best available estimate of the conditions now. There
    // is no way to know the true value during any given run, so this is an
    // estimator, not a tie-break: any rule that leaned toward the larger or
    // the smaller wall would be choosing a bias instead.
    const bool better =
        !best || (r.disabled != best->disabled ? r.disabled : r.startS > best->startS);
    if (better) {
      best = &r;
      refC = c;
      refD = d;
      refWireTotalD = static_cast<double>(refWireTotal);
      g.originEquivBytes = matchedWire;
      g.sigPairs = sigPairs;
      g.comparedFiles = matchedCount;
      // Verified means the work-identity check actually RAN on at least half
      // the compared files, not merely that it did not refuse.
      g.workVerified = checkable;
    }
  }

  if (!best) {
    // Checked FIRST: a baseline rejected for reading different parts never
    // reaches the coverage arithmetic, so its counters look like "no baseline
    // at all" — which is the one thing this refusal must not be mistaken for.
    if (readSigMismatch) {
      g.why = GainEstimate::Why::kReadDifferent;
      char buf[224];
      std::snprintf(buf, sizeof buf,
                    "a baseline exists for these files but the two runs read the same "
                    "parts of only %.0f%% of them (%.0f%% required) — it measured "
                    "different work, so comparing the walls would not mean anything",
                    (bestAgree < 0.0 ? 0.0 : bestAgree) * 100.0, kMinReadAgreement * 100.0);
      g.reason = buf;
    } else if (bestC > 0.0 || bestD > 0.0) {
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
                 "origin alone costs; a first run over data the cache does not "
                 "have yet can serve as one too";
    }
    return g;
  }

  const double refDur = static_cast<double>(best->durationS());
  const double runDur = static_cast<double>(run.durationS());
  const double originTime = refDur * refC; // what these files cost with no cache
  const double cacheTime = runDur * refD;  // what they cost from the cache now

  g.why = GainEstimate::Why::kMeasured;
  g.valid = true;
  g.savedS = originTime - cacheTime;
  g.gain = (runDur + g.savedS) / runDur;
  g.originMBs = refWireTotalD / refDur / 1e6;
  g.cacheMBs = static_cast<double>(runCacheTotal) / runDur / 1e6;
  g.matchedFiles = 0;
  for (const auto& [k, f] : run.files) {
    (void)f;
    if (best->files.count(k))
      ++g.matchedFiles;
  }
  g.referenceStartS = best->startS;
  g.referenceDisabled = best->disabled;
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


// ---------------------------------------------------------------------------
// Identity and baseline qualification.
// ---------------------------------------------------------------------------

namespace {
// FNV-1a over the sorted key set. Order-independent by construction (the keys
// are visited in map order, which is sorted), so two runs that opened the same
// files agree whatever order they opened them in.
std::string signatureOf(const std::map<std::string, Run::FileRec>& files) {
  if (files.empty())
    return std::string();
  uint64_t h = 1469598103934665603ull;
  for (const auto& [k, _] : files) {
    for (unsigned char c : k) {
      h ^= c;
      h *= 1099511628211ull;
    }
    h ^= '\n';
    h *= 1099511628211ull;
  }
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06llx", static_cast<unsigned long long>(h & 0xffffffull));
  return std::string(buf);
}

// Combine the per-file read footprints into one identity for the run, shown
// in `history` so two runs can be told apart at a glance. Visited in sorted
// key order, so the order files were opened in cannot change the answer, and
// empty if ANY file lacked a footprint, so a partial answer is never mistaken
// for a whole one. NOT what decides whether two runs may be compared -- that
// is settled per file, because one odd file out of hundreds should not veto a
// measurement (see estimateGain).
std::string readSignatureOf(const std::map<std::string, Run::FileRec>& files) {
  if (files.empty())
    return std::string();
  uint64_t h = 1469598103934665603ull;
  for (const auto& [k, f] : files) {
    (void)k;
    if (f.readSig.empty())
      return std::string();
    for (unsigned char c : f.readSig) {
      h ^= c;
      h *= 1099511628211ull;
    }
  }
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06llx", static_cast<unsigned long long>(h & 0xffffffull));
  return std::string(buf);
}

// Host out of `root://host:port//path`; empty when the key is not a URL.
std::string hostOf(const std::string& url) {
  const auto p = url.find("://");
  if (p == std::string::npos)
    return std::string();
  const auto start = p + 3;
  const auto end = url.find('/', start);
  std::string hp = url.substr(start, end == std::string::npos ? end : end - start);
  const auto colon = hp.find(':');
  return colon == std::string::npos ? hp : hp.substr(0, colon);
}
} // namespace

std::string Run::topOriginHost() const {
  const std::string* best = nullptr;
  uint64_t n = 0;
  for (const auto& [h, c] : originHosts)
    if (c > n) {
      n = c;
      best = &h;
    }
  return best ? *best : std::string();
}

double Run::originShare() const {
  // Weighted by size AT THE ORIGIN: the replica tier serves recompressed
  // bytes, so served-byte counters are not in comparable units.
  //
  // WITHIN each file the split is by bytes, not a verdict on the file. A real
  // run is a mixture -- some files relayed straight through, some filled from
  // the origin, some served warm, and single files that were partly each,
  // because a file can be evicted, refetched, or fall back to the origin mid
  // run. Calling a file wholly origin because most of it was made a run that
  // was half cache read as entirely direct, which is the one error this test
  // exists to prevent.
  double fromOrigin = 0.0;
  uint64_t total = 0;
  for (const auto& [k, f] : files) {
    (void)k;
    const uint64_t w = f.originSize ? f.originSize : f.deliveredBytes();
    if (!w)
      continue;
    // Bytes handed to the application for this file, summed over the three
    // DISJOINT tiers that can deliver one. It must be the sum and never the
    // larger of a pair: served, replica and wire are written in three
    // different places and a warm replica read touches only the replica
    // counter, so comparing served against wire scores such a file as
    // entirely origin -- and a warm replica run then qualifies as a run the
    // cache did not help, i.e. as the no-cache baseline every other run is
    // measured against. A true gain reported as a loss is the failure that
    // costs the most to believe, so the arithmetic here names all three.
    const uint64_t delivered = f.deliveredBytes();
    if (!delivered)
      continue; // opened, nothing read: no evidence either way
    total += w;
    fromOrigin += static_cast<double>(w) *
                  (static_cast<double>(f.wireBytes) / static_cast<double>(delivered));
  }
  if (!total)
    return disabled ? 1.0 : 0.0; // a disabled run with no records is still direct
  return fromOrigin / static_cast<double>(total);
}

namespace {
// A log2-microsecond histogram's total, taking each bucket at 1.5x its lower
// edge (the mid-point of a log2 bin). Coarse by construction; the ratio of two
// such totals is what is used, and the coarseness largely cancels.
double histTotalS(const std::vector<uint64_t>& h) {
  double us = 0.0;
  for (size_t i = 0; i < h.size(); ++i)
    us += static_cast<double>(h[i]) * 1.5 * static_cast<double>(1ull << i);
  return us / 1e6;
}
} // namespace

double Run::originWaitS() const { return histTotalS(histOriginRt); }

double Run::writeWaitS() const {
  return histTotalS(histFlushWrite) + histTotalS(histMetaFlush) +
         static_cast<double>(bufferStallUs) / 1e6;
}

double Run::fillCost() const {
  if (!bufferStallUs)
    return 0.0;
  const double stall = static_cast<double>(bufferStallUs);
  const double cpu = static_cast<double>(cpuUs);
  // Without cpu_us there is nothing to compare against; treat the run as
  // unqualified rather than silently passing it on a missing field.
  if (cpu <= 0.0)
    return 1.0;
  return stall / (cpu + stall);
}

double Run::coresBusy() const {
  if (!cpuUs)
    return 0.0;
  return (static_cast<double>(cpuUs) / 1e6) / static_cast<double>(durationS());
}




std::string Dataset::topHost() const {
  const std::string* best = nullptr;
  uint64_t n = 0;
  for (const auto& [h, c] : hosts)
    if (c > n) {
      n = c;
      best = &h;
    }
  return best ? *best : std::string();
}

std::vector<Dataset> byDataset(const std::vector<Run>& runs, size_t maxEstimates) {
  std::map<std::string, Dataset> by;
  size_t estimated = 0;
  for (const auto& r : runs) {
    if (r.sig.empty())
      continue;
    Dataset& d = by[r.sig];
    d.sig = r.sig;
    ++d.runs;
    if (!r.complete)
      ++d.incomplete;
    if (r.disabled || r.baselineQualified())
      ++d.baselines;
    d.readBytes += r.servedBytes;
    if (r.files.size() > d.files) {
      // Take the identity from the most complete run: a truncated run has its
      // own signature, so anything grouped here saw the same set.
      d.files = r.files.size();
      d.hosts = r.originHosts;
      std::set<std::string> dirs;
      uint64_t osz = 0;
      for (const auto& [k, f] : r.files) {
        osz += f.originSize;
        const auto slash = k.rfind('/');
        dirs.insert(slash == std::string::npos ? k : k.substr(0, slash));
      }
      d.dirs = dirs.size();
      d.originSize = osz;
    }
    if (r.disabled || estimated >= maxEstimates)
      continue;
    ++estimated;
    const GainEstimate g = estimateGain(r, runs);
    if (!g.valid)
      continue;
    ++d.measured;
    d.measuredBytes += r.servedBytes;
    // Which tier did the work? The replica tier serves recompressed bytes, so
    // a run that used it at all is not comparable with a byte-tier run.
    if (r.replicaBytesServed > r.hitBytes) {
      d.replWall += static_cast<double>(r.durationS());
      d.replSaved += g.savedS;
      ++d.replRuns;
    } else {
      d.byteWall += static_cast<double>(r.durationS());
      d.byteSaved += g.savedS;
      ++d.byteRuns;
    }
  }
  std::vector<Dataset> out;
  out.reserve(by.size());
  for (auto& [k, d] : by) {
    (void)k;
    out.push_back(std::move(d));
  }
  std::sort(out.begin(), out.end(),
            [](const Dataset& a, const Dataset& b) { return a.readBytes > b.readBytes; });
  return out;
}

} // namespace ucache
