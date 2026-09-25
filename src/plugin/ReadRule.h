// The max_read_fraction rule. A process that asks for more than the limit of
// a ROOT file's data gains nothing from caching it -- it would fill the cache
// with the file -- so it reads that file straight from the origin: what it
// fetches is not kept, while bytes already cached are still served.
//
// Decided once per file per process, early, and not checked again. The share
// is the bytes the branches (RNTuple: columns) read so far hold in the file
// over the bytes of every branch -- the part of each entry's data the reader
// asks for, from the file's own metadata (ReadMap). Read directly as soon as
// it exceeds the limit; cached for good as soon as a request of two or more
// branches adds none (the reader has come back to branches it read before).
// Until then the file is cached as usual.
//
// The map is parsed only when a request needs the origin, so a warm pass
// never pays for it. The file's own records (header, keys list, tree record,
// RNTuple envelopes) are cached as usual whatever the decision. Files smaller
// than kMinBytes, files that are not TTree or RNTuple, and a limit of 100 have
// no rule: they are cached as before.
//
// Thread-safety: all methods are thread-safe. A rule is shared by every handle
// of the file in the process and lives for the process.
#pragma once

#include "ReadMap.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ucache {

struct OriginSource;

class ReadRule : public std::enable_shared_from_this<ReadRule> {
 public:
  enum State : uint8_t { kUndecided = 0, kCache = 1, kDirect = 2, kNoRule = 3 };
  static constexpr uint64_t kMinBytes = 64ull << 20;

  // The rule for a file, created on first use, or null when none applies.
  static std::shared_ptr<ReadRule> forFile(const std::string& key, uint64_t originSize);

  State state() const { return static_cast<State>(state_.load(std::memory_order_acquire)); }
  bool direct() const { return state() == kDirect; }

  // A request that needs the origin, as ranges of the ORIGINAL file. Parses
  // the file's map on the first call (reading through `src`: byte cache first,
  // origin otherwise, the metadata kept), then takes the request into the
  // share (ReadMap::step). Concurrent calls wait for each other.
  State observe(const std::vector<std::pair<uint64_t, uint64_t>>& ranges, OriginSource& src);

  // After a direct decision: does [off, off + len) read the file's data? The
  // file's own records are fetched and kept as usual.
  bool readsData(uint64_t off, uint64_t len) const;

  ReadRule(std::string key, uint64_t size, int limit)
      : key_(std::move(key)), size_(size), limit_(limit) {}
  ~ReadRule();

 private:
  void holdMap();  // counts this parse against the bound on maps held (caller holds mu_)
  void dropMap();  // the bound's victim: parsed again if it is needed again
  std::string key_;
  uint64_t size_;
  int limit_;
  std::atomic<uint8_t> state_{kUndecided};
  std::mutex mu_;              // one parse or step at a time
  bool parsed_ = false;        // guarded by mu_
  std::unique_ptr<transpose::ReadMap> map_; // until the decision; guarded by mu_
  std::vector<uint32_t> seen_; // branches (columns) requests touched so far; guarded by mu_
  // Written once, before state_ becomes kDirect (release), read after (acquire).
  std::vector<std::pair<uint64_t, uint64_t>> runs_;
};

// Whether this process decided to read `key` directly: such a file is never
// given a new slot store.
bool readRuleDirect(const std::string& key);

} // namespace ucache
