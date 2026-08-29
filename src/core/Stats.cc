#include "Stats.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>

namespace ucache {
namespace {
// Extract an integer field from one flat stats JSON line: matches the exact
// `"<key>":<digits>` Stats::toJsonBody writes (arrays/strings are skipped).
uint64_t extractU64(const std::string& line, const char* key) {
  std::string needle = std::string("\"") + key + "\":";
  auto p = line.find(needle);
  if (p == std::string::npos)
    return 0;
  p += needle.size();
  uint64_t v = 0;
  while (p < line.size() && line[p] >= '0' && line[p] <= '9')
    v = v * 10 + static_cast<uint64_t>(line[p++] - '0');
  return v;
}

// Parse `"<key>":[n,n,...]` (a Histogram::toJson array) and add bucket-wise
// into `into`, growing it as needed. Absent key = no-op.
void extractHist(const std::string& line, const char* key, std::vector<uint64_t>& into) {
  std::string needle = std::string("\"") + key + "\":[";
  auto p = line.find(needle);
  if (p == std::string::npos)
    return;
  p += needle.size();
  size_t i = 0;
  while (p < line.size() && line[p] != ']') {
    uint64_t v = 0;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9')
      v = v * 10 + static_cast<uint64_t>(line[p++] - '0');
    if (into.size() <= i)
      into.resize(i + 1, 0);
    into[i++] += v;
    if (p < line.size() && line[p] == ',')
      ++p;
  }
}
} // namespace

std::string Histogram::toJson() const {
  int last = kBuckets - 1;
  while (last >= 0 && b[last].load(std::memory_order_relaxed) == 0)
    --last;
  std::ostringstream os;
  os << '[';
  for (int i = 0; i <= last; ++i) {
    if (i)
      os << ',';
    os << b[i].load(std::memory_order_relaxed);
  }
  os << ']';
  return os.str();
}

std::string Stats::toJsonBody() const {
  std::ostringstream os;
  auto f = [&](const char* name, const std::atomic<uint64_t>& v) {
    os << '"' << name << "\":" << v.load(std::memory_order_relaxed) << ',';
  };
  f("opens", opens);
  f("validations_failed", validationsFailed);
  f("hit_bytes", hitBytes);
  f("miss_bytes", missBytes);
  f("origin_bytes", originBytes);
  f("served_bytes", servedBytes);
  f("origin_reads", originReads);
  f("fetches_joined", fetchesJoined);
  f("origin_readvs", originReadvs);
  f("page_writes", pageWrites);
  f("crc_failures", crcFailures);
  f("meta_corrupt", metaCorrupt);
  f("evicted_entries", evictedEntries);
  f("evicted_bytes", evictedBytes);
  f("failopen_events", failopenEvents);
  f("admissions_bypassed", admissionsBypassed);
  f("open_retries", openRetries);
  f("open_retries_exhausted", openRetriesExhausted);
  f("disabled_handles", disabledHandles);
  f("replica_opens", replicaOpens);
  f("replica_published", replicaPublished);
  f("replica_invalid", replicaInvalid);
  f("replica_crc_failures", replicaCrcFailures);
  f("replica_punched_bytes", replicaPunchedBytes);
  f("replica_orphans_swept", replicaOrphansSwept);
  f("files_opened", filesOpened);
  f("ram_hit_bytes", ramHitBytes);
  f("first_touch_bytes", firstTouchBytes);
  f("hit_disk_reads", hitDiskReads);
  f("hit_disk_bytes", hitDiskBytes);
  f("hit_disk_seq", hitDiskSeq);
  f("replica_bytes_served", replicaBytesServed);
  f("replica_reads", replicaReads);
  f("replica_read_bytes", replicaReadBytes);
  f("relay_bytes", relayBytes);
  f("readv_chunks", readvChunks);
  f("readv_calls", readvCalls);
  f("readv_mixed", readvMixed);
  f("flush_runs", flushRuns);
  f("flush_run_bytes", flushRunBytes);
  f("buffer_stalls", bufferStalls);
  f("buffer_stall_us", bufferStallUs);
  f("handles_high_water", handlesHighWater);
  f("threads_high_water", threadsHighWater);
  f("reads_in_flight_high_water", readsInFlightHighWater);
  f("origin_reads_in_flight_high_water", originReadsInFlightHighWater);
  os << "\"hist_hit_read_us\":" << hitReadUs.toJson() << ',';
  os << "\"hist_miss_read_us\":" << missReadUs.toJson() << ',';
  os << "\"hist_origin_rt_us\":" << originRtUs.toJson() << ',';
  os << "\"hist_open_us\":" << openUs.toJson() << ',';
  os << "\"hist_replica_read_us\":" << replicaReadUs.toJson() << ',';
  os << "\"hist_flush_write_us\":" << flushWriteUs.toJson() << ',';
  os << "\"hist_meta_flush_us\":" << metaFlushUs.toJson() << ',';
  os << "\"hist_req_read_bytes\":" << reqReadBytes.toJson() << ',';
  os << "\"hist_hit_read_bytes\":" << hitReadSize.toJson() << ',';
  os << "\"hist_replica_read_bytes\":" << replicaReadSize.toJson();
  return os.str();
}

StatsTotals aggregateStats(const std::string& statsDir) {
  StatsTotals t;
  bool sawNewSchema = false;
  DIR* d = ::opendir(statsDir.c_str());
  if (!d)
    return t;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n.size() < 6 || n.compare(n.size() - 6, 6, ".jsonl") != 0)
      continue;
    // Per-file/trace companions live next to the counter files; they are not
    // cumulative counter lines and must not be summed (or counted as files).
    auto endsWith = [&](const char* suf) {
      size_t l = ::strlen(suf);
      return n.size() >= l && n.compare(n.size() - l, l, suf) == 0;
    };
    if (endsWith(".files.jsonl") || endsWith(".trace.jsonl"))
      continue;
    std::ifstream in(statsDir + "/" + n);
    std::string line, last;
    while (std::getline(in, line))
      if (!line.empty() && line.back() == '}')
        last = line; // final COMPLETE cumulative line (skip a torn/partial tail)
    if (last.empty())
      continue;
    ++t.files;
    t.opens += extractU64(last, "opens");
    t.validationsFailed += extractU64(last, "validations_failed");
    t.hitBytes += extractU64(last, "hit_bytes");
    t.missBytes += extractU64(last, "miss_bytes");
    t.originBytes += extractU64(last, "origin_bytes");
    t.servedBytes += extractU64(last, "served_bytes");
    t.originReads += extractU64(last, "origin_reads");
    t.fetchesJoined += extractU64(last, "fetches_joined");
    t.originReadvs += extractU64(last, "origin_readvs");
    t.pageWrites += extractU64(last, "page_writes");
    t.crcFailures += extractU64(last, "crc_failures");
    t.metaCorrupt += extractU64(last, "meta_corrupt");
    t.evictedEntries += extractU64(last, "evicted_entries");
    t.evictedBytes += extractU64(last, "evicted_bytes");
    t.failopenEvents += extractU64(last, "failopen_events");
    t.admissionsBypassed += extractU64(last, "admissions_bypassed");
    t.openRetries += extractU64(last, "open_retries");
    t.openRetriesExhausted += extractU64(last, "open_retries_exhausted");
    t.replicaOpens += extractU64(last, "replica_opens");
    t.replicaPublished += extractU64(last, "replica_published");
    t.replicaInvalid += extractU64(last, "replica_invalid");
    t.replicaCrcFailures += extractU64(last, "replica_crc_failures");
    t.replicaPunchedBytes += extractU64(last, "replica_punched_bytes");
    t.replicaOrphansSwept += extractU64(last, "replica_orphans_swept");
    t.filesOpened += extractU64(last, "files_opened");
    t.ramHitBytes += extractU64(last, "ram_hit_bytes");
    t.firstTouchBytes += extractU64(last, "first_touch_bytes");
    t.hitDiskReads += extractU64(last, "hit_disk_reads");
    t.hitDiskBytes += extractU64(last, "hit_disk_bytes");
    t.hitDiskSeq += extractU64(last, "hit_disk_seq");
    t.replicaBytesServed += extractU64(last, "replica_bytes_served");
    t.replicaReads += extractU64(last, "replica_reads");
    // Per FILE, not per total: a file written before this counter existed can
    // only offer served bytes, and mixing the two bases silently divides new
    // bytes by old-plus-new reads.
    const bool hasNew = last.find("\"replica_read_bytes\":") != std::string::npos;
    t.replicaReadBytes += hasNew ? extractU64(last, "replica_read_bytes")
                                 : extractU64(last, "replica_bytes_served");
    if (t.files > 1 && hasNew != sawNewSchema)
      t.schemaMixed = true;
    sawNewSchema = hasNew;
    t.relayBytes += extractU64(last, "relay_bytes");
    t.readvChunks += extractU64(last, "readv_chunks");
    t.readvCalls += extractU64(last, "readv_calls");
    t.readvMixed += extractU64(last, "readv_mixed");
    t.flushRuns += extractU64(last, "flush_runs");
    t.flushRunBytes += extractU64(last, "flush_run_bytes");
    t.bufferStalls += extractU64(last, "buffer_stalls");
    t.bufferStallUs += extractU64(last, "buffer_stall_us");
    // A high-water mark is a MAX, not a sum: adding two processes' peaks would
    // invent a concurrency neither reached.
    t.handlesHighWater = std::max(t.handlesHighWater, extractU64(last, "handles_high_water"));
    t.threadsHighWater = std::max(t.threadsHighWater, extractU64(last, "threads_high_water"));
    t.readsInFlightHighWater =
        std::max(t.readsInFlightHighWater, extractU64(last, "reads_in_flight_high_water"));
    t.originReadsInFlightHighWater =
        std::max(t.originReadsInFlightHighWater,
                 extractU64(last, "origin_reads_in_flight_high_water"));
    extractHist(last, "hist_hit_read_us", t.histHitRead);
    extractHist(last, "hist_origin_rt_us", t.histOriginRt);
    extractHist(last, "hist_open_us", t.histOpen);
    extractHist(last, "hist_replica_read_us", t.histReplicaRead);
    extractHist(last, "hist_flush_write_us", t.histFlushWrite);
    extractHist(last, "hist_req_read_bytes", t.histReqRead);
    extractHist(last, "hist_hit_read_bytes", t.histHitReadSize);
    extractHist(last, "hist_replica_read_bytes", t.histReplicaReadSize);
  }
  ::closedir(d);
  return t;
}

} // namespace ucache
