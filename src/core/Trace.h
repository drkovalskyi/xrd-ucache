// Opt-in sampled per-operation IO trace (`trace = io`).
//
// Enabled by `trace = io` (UCACHE_TRACE=io); records land as JSON lines in
// <stats stem>.trace.jsonl next to the process's stats file. Read-class ops
// (hit/ram/replica/wire/readv) are sampled 1-in-N (`trace_sample`, default
// 64) to bound volume; rare ops (open/flush/meta) are always recorded. Each
// key is announced once as a legend line {"op":"key","k":..,"url":..}; data
// lines carry only the 16-hex key hash and a `t` field: a small per-thread
// slot id, assigned on that thread's first record. Slot ids (not OS tids) are
// what make a trace replayable — reconstructing a thread's timeline needs the
// records grouped by worker, and the gaps between one worker's completion and
// its next issue ARE that worker's compute.
//
// Thread-safety: the sampling counter is a relaxed atomic; file writes and
// the legend set are guarded by an internal mutex. Trace loss on IO error is
// acceptable (fail-open); the tracer never throws and never blocks reads on
// anything but its own mutex.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

namespace ucache {

class IOBackend;

class Tracer {
 public:
  // sample < 1 is clamped to 1 (record everything).
  Tracer(IOBackend& io, std::string path, int sample);
  ~Tracer();
  Tracer(const Tracer&) = delete;
  Tracer& operator=(const Tracer&) = delete;

  // op: short verb ("open","hit","ram","replica","wire","readv","flush",
  // "meta"). us = operation span; off/len describe the byte range (for
  // "readv", off = first chunk offset, len = total bytes). sampled=false
  // bypasses sampling (rare, individually meaningful ops).
  void rec(const char* op, const std::string& key, uint64_t off, uint64_t len, uint64_t us,
           bool sampled = true);

  // This thread's slot id in THIS tracer, assigned on first use and stable for
  // the thread's life. Public so the fill/serve paths can attribute without a
  // second lookup.
  uint32_t slot();

 private:
  IOBackend& io_;
  const std::string path_;
  const int sample_;
  std::atomic<uint64_t> n_{0};
  std::mutex mu_;
  int fd_ = -1;       // opened lazily on first record
  uint64_t off_ = 0;  // append offset (single writer per path)
  std::unordered_set<uint64_t> legend_; // key hashes already announced
  std::atomic<uint32_t> nextSlot_{0};    // hands out per-thread slot ids
};

} // namespace ucache
