#include "ReadRule.h"

#include "Log.h"
#include "OriginSource.h"
#include "RNTupleMeta.h"
#include "TreeMeta.h"

#include <algorithm>
#include <deque>
#include <iterator>
#include <unordered_map>

namespace ucache {

namespace tp = transpose;

namespace {

// Every file this process has asked the question of, for the process's life:
// a decision is never taken twice. Bounded like the other per-key maps; past
// the bound a new file simply has no rule.
constexpr size_t kMaxRules = 200000;

std::mutex& regMu() {
  static auto* m = new std::mutex;
  return *m;
}
std::unordered_map<std::string, std::shared_ptr<ReadRule>>& registry() {
  static auto* r = new std::unordered_map<std::string, std::shared_ptr<ReadRule>>();
  return *r;
}

// Parsed maps held by files still undecided: a reader that only ever reads one
// branch at a time never decides, and must not keep every file's map for the
// life of the process. Past the bound the oldest undecided file is cached as
// usual, for good, and its map freed.
constexpr size_t kMaxMaps = 64;
std::mutex& mapsMu() {
  static auto* m = new std::mutex;
  return *m;
}
std::deque<std::weak_ptr<ReadRule>>& maps() {
  static auto* d = new std::deque<std::weak_ptr<ReadRule>>();
  return *d;
}

std::atomic<bool> g_warned{false};

} // namespace

ReadRule::~ReadRule() = default;

void ReadRule::dropMap() {
  std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
  if (!g.owns_lock())
    return; // in use: freed at its decision
  if (map_ && state() == kUndecided) {
    UCACHE_DEBUG("read rule: %s undecided while %zu other files were read; cached as usual",
                 key_.c_str(), kMaxMaps);
    state_.store(kCache, std::memory_order_release);
  }
  map_.reset();
  seen_.clear();
  seen_.shrink_to_fit();
}

std::shared_ptr<ReadRule> ReadRule::forFile(const std::string& key, uint64_t originSize) {
  const int limit = globalConfig().maxReadFraction;
  if (limit >= 100 || originSize < kMinBytes)
    return nullptr;
  std::lock_guard<std::mutex> g(regMu());
  auto& r = registry();
  auto it = r.find(key);
  if (it != r.end() && it->second->size_ == originSize)
    return it->second;
  if (it == r.end() && r.size() >= kMaxRules)
    return nullptr;
  auto rule = std::make_shared<ReadRule>(key, originSize, limit);
  r[key] = rule; // a file of another size (it changed at the origin) decides again
  return rule;
}

bool readRuleDirect(const std::string& key) {
  std::lock_guard<std::mutex> g(regMu());
  auto it = registry().find(key);
  return it != registry().end() && it->second->direct();
}

ReadRule::State ReadRule::observe(const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
                                  OriginSource& src) {
  State s = state();
  if (s != kUndecided)
    return s;
  // Waited for, not skipped: the requests of one fill arrive together, and a
  // request not seen would leave its branches out of the share. The holder
  // parses once (the file's records, normally in the byte cache already) and
  // is otherwise a few lookups.
  std::lock_guard<std::mutex> g(mu_);
  s = state();
  if (s != kUndecided)
    return s;
  if (!parsed_) {
    parsed_ = true;
    tp::FileMeta fm = tp::parseReaderTree(src, static_cast<int64_t>(size_));
    if (fm.error.empty()) {
      map_ = std::make_unique<tp::ReadMap>(tp::ReadMap::fromTree(fm));
    } else if (fm.error.find("not found") != std::string::npos && src.fetchHead()) {
      tp::RNTupleMeta rm = tp::parseRNTuple(src, static_cast<int64_t>(size_), "");
      if (rm.error.empty())
        map_ = std::make_unique<tp::ReadMap>(tp::ReadMap::fromRNTuple(rm));
    }
    if (!map_ || map_->empty()) {
      UCACHE_DEBUG("read rule: %s has no basket or page map (%s); cached as usual", key_.c_str(),
                   fm.error.empty() ? "no units" : fm.error.c_str());
      map_.reset();
      state_.store(kNoRule, std::memory_order_release);
      return kNoRule;
    }
    holdMap();
  }
  // The branches read so far, weighed (ReadMap::step): direct as soon as they
  // hold more than the limit, cached once the reader comes back to them.
  tp::ReadMap::Share sh;
  const int d = map_->step(seen_, ranges, limit_, sh);
  if (d < 0)
    return kUndecided;
  seen_.clear();
  seen_.shrink_to_fit();
  const double pct = sh.total ? 100.0 * static_cast<double>(sh.asked) / sh.total : 0.0;
  if (d == 0) {
    UCACHE_DEBUG("read rule: %s: read %u %s holding %.1f%% of its data; cached", key_.c_str(),
                 sh.groups, map_->isRNTuple() ? "columns" : "branches", pct);
    map_.reset();
    state_.store(kCache, std::memory_order_release);
    return kCache;
  }
  runs_ = map_->dataRuns();
  const bool rnt = map_->isRNTuple();
  map_.reset();
  state_.store(kDirect, std::memory_order_release);
  if (src.st && src.st->store)
    src.st->store->stats().directReadFiles.fetch_add(1, std::memory_order_relaxed);
  if (!g_warned.exchange(true))
    UCACHE_WARN("%s: this job reads %u %s holding %.1f%% of the file's data, more than "
                "max_read_fraction = %d%%; it is read straight from the origin and what it "
                "fetches is not cached (raise max_read_fraction to cache such files; further "
                "files are counted in `ucache stats`, not reported)",
                key_.c_str(), sh.groups, rnt ? "columns" : "branches", pct, limit_);
  else
    UCACHE_INFO("%s: read %u %s holding %.1f%% of its data; read straight from the origin",
                key_.c_str(), sh.groups, rnt ? "columns" : "branches", pct);
  return kDirect;
}

void ReadRule::holdMap() {
  std::shared_ptr<ReadRule> oldest;
  {
    std::lock_guard<std::mutex> g(mapsMu());
    auto& d = maps();
    d.push_back(weak_from_this());
    if (d.size() > kMaxMaps) {
      oldest = d.front().lock();
      d.pop_front();
    }
  }
  if (oldest && oldest.get() != this)
    oldest->dropMap(); // never waits: another rule's lock is only tried
}

bool ReadRule::readsData(uint64_t off, uint64_t len) const {
  if (!direct() || len == 0)
    return false;
  const uint64_t end = off + len;
  auto it = std::upper_bound(runs_.begin(), runs_.end(), std::pair<uint64_t, uint64_t>(off, ~uint64_t{0}));
  if (it != runs_.begin() && std::prev(it)->second > off)
    return true;
  return it != runs_.end() && it->first < end;
}

} // namespace ucache
