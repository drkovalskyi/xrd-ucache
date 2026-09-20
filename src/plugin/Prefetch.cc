#include "Prefetch.h"

#include "Config.h"
#include "FileEntry.h"
#include "Log.h"
#include "OriginInFlight.h"
#include "ReadRounding.h"
#include "TreeMeta.h"
#include "UCacheFile.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ucache {

namespace {

constexpr double kConfirmCoverage = 0.90;  // a shadow prediction this good engages the process
constexpr double kGateNeverUsed = 0.25;    // never-used over issued that switches the process off
constexpr uint64_t kGateMinIssued = 64ull << 20; // ... once this much has been issued
constexpr size_t kTableCache = 8;          // basket tables kept across handles (RDF opens a file twice)
constexpr size_t kMaxReadvElems = 1024;    // the readv element cap, per wire request
constexpr int kParseTries = 3;             // a reader's metadata reads stage asynchronously: try again on a later fill

// The basket map of one file, compacted from the parser's FileMeta: per-branch
// arrays concatenated by branch (branch b owns [boff[b], boff[b] + nb[b])), and
// a seek-sorted index for looking a request's chunks up by offset.
struct BasketTable {
  std::vector<std::string> names;
  std::vector<uint32_t> nb, boff;
  std::vector<int64_t> seek, size, es; // per basket, global index
  std::vector<int64_t> sortedSeek;     // seek of sortedIdx[k]
  std::vector<uint32_t> sortedIdx;
  int64_t fileSize = 0;

  uint32_t branchOf(uint32_t g) const {
    auto it = std::upper_bound(boff.begin(), boff.end(), g);
    return static_cast<uint32_t>(it - boff.begin()) - 1;
  }
  uint32_t indexOf(uint32_t g) const { return g - boff[branchOf(g)]; }

  static std::shared_ptr<BasketTable> build(const transpose::FileMeta& fm, int64_t fileSize) {
    auto t = std::make_shared<BasketTable>();
    t->fileSize = fileSize;
    uint64_t total = 0;
    for (const auto& b : fm.branches) {
      t->names.push_back(b.name);
      t->boff.push_back(static_cast<uint32_t>(total));
      const uint32_t n = static_cast<uint32_t>(b.basketSeek.size());
      t->nb.push_back(n);
      total += n;
      for (uint32_t i = 0; i < n; ++i) {
        t->seek.push_back(b.basketSeek[i]);
        t->size.push_back(b.basketBytes[i]);
        t->es.push_back(i < b.basketEntry.size() ? b.basketEntry[i] : 0);
      }
    }
    t->sortedIdx.resize(total);
    for (uint32_t g = 0; g < total; ++g)
      t->sortedIdx[g] = g;
    std::sort(t->sortedIdx.begin(), t->sortedIdx.end(),
              [&](uint32_t a, uint32_t b) { return t->seek[a] < t->seek[b]; });
    t->sortedSeek.resize(total);
    for (uint32_t k = 0; k < total; ++k)
      t->sortedSeek[k] = t->seek[t->sortedIdx[k]];
    return t;
  }

  // A chunk [off, off+len) -> the run of baskets it is made of, appended to
  // `out`. True when the chunk starts on a basket and is covered by a
  // contiguous run (exactly or past its end); false for anything else, which
  // for a TTree reader is the file-open metadata.
  bool decompose(uint64_t off, uint64_t len, std::vector<uint32_t>& out) const {
    auto it = std::lower_bound(sortedSeek.begin(), sortedSeek.end(), static_cast<int64_t>(off));
    if (it == sortedSeek.end() || static_cast<uint64_t>(*it) != off)
      return false;
    size_t k = static_cast<size_t>(it - sortedSeek.begin());
    const uint64_t end = off + len;
    uint64_t cur = off;
    const size_t first = out.size();
    while (k < sortedSeek.size() && cur < end && static_cast<uint64_t>(sortedSeek[k]) == cur) {
      const uint32_t g = sortedIdx[k];
      out.push_back(g);
      cur += static_cast<uint64_t>(size[g]);
      ++k;
    }
    if (cur < end) { // a gap inside the chunk: not a basket run
      out.resize(first);
      return false;
    }
    return true;
  }
};

// The parser's byte source over a live entry: presence from the bitmap and
// the stage, bytes from the hit path without touching the serve counters.
struct EntrySource : transpose::Source {
  FileEntry& e;
  explicit EntrySource(FileEntry& entry) : e(entry) {}
  bool has(uint64_t off, uint64_t n) override { return e.hasRange(off, n); }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    return e.readCached(off, n, dst, /*account=*/false);
  }
};

} // namespace

struct PrefetchHandle {
  std::weak_ptr<HandleState> owner; // the handle this state belongs to; a reused address is not it
  std::shared_ptr<const BasketTable> table;
  int parseTries = 0;            // failed parses so far; the metadata may simply not be staged yet
  bool noTable = false;          // the file did not parse after kParseTries: never look again
  std::vector<int64_t> frontier; // per branch: highest basket index demanded, -1 = none
  std::vector<uint64_t> share;   // per branch: largest bytes drawn in one fill by this handle
  uint64_t fills = 0;
  std::vector<uint32_t> shadow;  // the prediction for the next fill, sorted (shadow or issued)
  struct Issued {
    uint32_t g;
    uint64_t off, len; // the basket's byte range (page rounding is FileEntry's)
  };
  std::vector<Issued> issued;    // speculative baskets not yet judged served or passed over
  std::atomic<bool> closed{false};
};

struct Prefetcher::Impl {
  // ---- the worker and its queue: one pending job per handle, oldest first --
  // The closing thread waits on this until the worker has processed the close
  // AND destroyed the job, so no reference to the handle outlives the plugin
  // object that owns it (the plugin destructor already waits for every other
  // task that holds its state; this keeps the rule).
  struct Sync {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
  };
  struct Job {
    std::shared_ptr<HandleState> st;
    std::shared_ptr<FileEntry> entry;
    std::vector<std::pair<uint64_t, uint32_t>> chunks;
    bool close = false;
    Sync* sync = nullptr;
  };
  std::mutex qmu_;
  std::condition_variable qcv_;
  std::deque<HandleState*> order_;
  std::unordered_map<HandleState*, Job> pending_;
  std::thread worker_;

  // ---- process state, owned by the worker thread ---------------------------
  std::atomic<bool> confirmed_{false};
  std::atomic<bool> disabled_{false};
  uint64_t maxFill_ = 0;
  std::unordered_map<std::string, uint64_t> shareByName_; // the memo across files
  uint64_t issued_ = 0, dropped_ = 0;
  std::list<std::pair<std::string, std::shared_ptr<BasketTable>>> tables_; // LRU, front = newest
  // Per-handle state, owned HERE and touched only by the worker thread. Keyed
  // by the handle's address and validated through the weak owner, so a handle
  // that went away without a close (or an address reused by a new handle)
  // never inherits another handle's frontier.
  std::unordered_map<HandleState*, std::shared_ptr<PrefetchHandle>> handles_;
  uint64_t jobs_ = 0;

  Impl() { worker_ = std::thread([this] { loop(); }); worker_.detach(); }

  void post(Job j) {
    Job superseded; // destroyed outside the lock, on the caller's thread
    {
      std::lock_guard<std::mutex> g(qmu_);
      HandleState* k = j.st.get();
      auto it = pending_.find(k);
      if (it == pending_.end()) {
        if (j.close)
          order_.push_front(k); // a closing handle waits on this: ahead of every fill
        else
          order_.push_back(k);
        pending_.emplace(k, std::move(j));
      } else if (j.close) {
        superseded = std::move(it->second); // a close supersedes any fill still queued
        it->second = std::move(j);
        order_.erase(std::find(order_.begin(), order_.end(), k));
        order_.push_front(k);
      } else if (!it->second.close) {
        superseded = std::move(it->second); // a newer fill supersedes an older one
        it->second = std::move(j);
      }
    }
    qcv_.notify_one();
  }

  void loop() {
    for (;;) {
      Sync* sync = nullptr;
      {
        Job job;
        {
          std::unique_lock<std::mutex> lk(qmu_);
          qcv_.wait(lk, [&] { return !order_.empty(); });
          HandleState* k = order_.front();
          order_.pop_front();
          auto it = pending_.find(k);
          job = std::move(it->second);
          pending_.erase(it);
        }
        sync = job.sync;
        try {
          if (job.close)
            handleClose(job);
          else
            handleFill(job);
        } catch (const std::exception& e) {
          UCACHE_WARN("prefetch: %s; switching off for this process", e.what());
          disabled_.store(true);
        }
      } // the job, and with it the last reference this thread holds, dies here
      if (sync) {
        std::lock_guard<std::mutex> g(sync->m);
        sync->done = true;
        sync->cv.notify_all();
      }
    }
  }

  Stats* stats(const Job& j) { return j.st->store ? &j.st->store->stats() : nullptr; }

  std::shared_ptr<PrefetchHandle> handle(const Job& j) {
    if (++jobs_ % 64 == 0) // handles that vanished without a close
      for (auto it = handles_.begin(); it != handles_.end();)
        it = it->second->owner.expired() ? handles_.erase(it) : std::next(it);
    auto it = handles_.find(j.st.get());
    if (it != handles_.end() && it->second->owner.lock() == j.st)
      return it->second;
    auto h = std::make_shared<PrefetchHandle>();
    h->owner = j.st;
    handles_[j.st.get()] = h;
    j.st->prefetchSeen.store(true, std::memory_order_release);
    return h;
  }
  std::shared_ptr<PrefetchHandle> existing(const Job& j) {
    auto it = handles_.find(j.st.get());
    if (it == handles_.end() || it->second->owner.lock() != j.st)
      return nullptr;
    return it->second;
  }

  // The file's basket table: from the cache of recently parsed files, else
  // parsed now from the bytes the reader already fetched into this entry.
  std::shared_ptr<BasketTable> tableFor(const Job& j) {
    const std::string& key = j.entry->key().key;
    for (auto it = tables_.begin(); it != tables_.end(); ++it)
      if (it->first == key) {
        auto t = it->second;
        tables_.splice(tables_.begin(), tables_, it);
        return t;
      }
    EntrySource src(*j.entry);
    const int64_t size = static_cast<int64_t>(j.entry->fileSize());
    transpose::FileMeta fm = transpose::parseFile(src, size, "Events");
    if (!fm.error.empty() && fm.error.find("not found") != std::string::npos) {
      // Not NanoAOD's tree name: take the first TTree the keys list names.
      transpose::ContainerMeta cm = transpose::parseContainer(src, size);
      for (const auto& k : cm.keys)
        if (k.cls == "TTree") {
          fm = transpose::parseFile(src, size, k.name);
          break;
        }
    }
    if (Stats* s = stats(j))
      s->prefetchParses.fetch_add(1, std::memory_order_relaxed);
    if (!fm.error.empty() || fm.branches.empty()) {
      UCACHE_INFO("prefetch: no basket map for %s yet (%s)", j.st->url.c_str(),
                  fm.error.empty() ? "no branches" : fm.error.c_str());
      return nullptr;
    }
    auto t = BasketTable::build(fm, size);
    tables_.emplace_front(key, t);
    while (tables_.size() > kTableCache)
      tables_.pop_back();
    return t;
  }

  void handleClose(const Job& j) {
    auto hp = existing(j);
    if (!hp)
      return;
    PrefetchHandle& h = *hp;
    h.closed.store(true, std::memory_order_release);
    if (j.entry)
      for (const auto& i : h.issued)
        dropped_ += j.entry->dropSpeculative(i.off, i.len);
    h.issued.clear();
    h.shadow.clear();
    handles_.erase(j.st.get()); // completions still in flight hold their own reference
  }

  void handleFill(const Job& j) {
    const Config& cfg = globalConfig();
    if (!cfg.prefetch || disabled_.load())
      return;
    auto hp = handle(j);
    PrefetchHandle& h = *hp;
    if (h.closed.load())
      return;
    ++h.fills;
    if (!h.table && !h.noTable) {
      // The parse waits for a handle's second fill while nothing in the
      // process has confirmed: a one-fill-per-open reader never pays for it.
      if (!confirmed_.load() && h.fills < 2)
        return;
      // Nothing to parse from until the reader has fetched the file header;
      // a vector read that arrives before it (another thread's, on a shared
      // handle) is not an attempt.
      if (!j.entry->hasRange(0, 512))
        return;
      h.table = tableFor(j);
      if (!h.table) {
        // The reader's own metadata reads are staged asynchronously after they
        // complete; a fast reader's second fill can arrive before the header is
        // resident. A failed parse is cheap, so it is retried on later fills.
        if (++h.parseTries >= kParseTries)
          h.noTable = true;
        return;
      }
      h.frontier.assign(h.table->nb.size(), -1);
      h.share.assign(h.table->nb.size(), 0);
    }
    if (!h.table)
      return;
    const BasketTable& t = *h.table;

    // What the reader asked for, as baskets.
    std::vector<uint32_t> gs;
    uint64_t fillBytes = 0;
    for (const auto& [off, len] : j.chunks) {
      fillBytes += len;
      t.decompose(off, len, gs);
    }
    if (gs.empty())
      return; // the file-open metadata reads: nothing to learn from yet
    std::sort(gs.begin(), gs.end());
    gs.erase(std::unique(gs.begin(), gs.end()), gs.end());

    // Frontier and shares: which branches, how far, how much per fill. The
    // frontier BEFORE this fill is kept for the passed-over judgement below:
    // this fill's own baskets are being served right now on another thread,
    // and judging them here would drop pages that serve is about to find.
    const std::vector<int64_t> prevFrontier = h.frontier;
    std::vector<uint64_t> perBranch(t.nb.size(), 0);
    for (uint32_t g : gs) {
      const uint32_t b = t.branchOf(g);
      const int64_t idx = static_cast<int64_t>(g - t.boff[b]);
      h.frontier[b] = std::max(h.frontier[b], idx);
      perBranch[b] += static_cast<uint64_t>(t.size[g]);
    }
    for (uint32_t b = 0; b < perBranch.size(); ++b)
      if (perBranch[b]) {
        h.share[b] = std::max(h.share[b], perBranch[b]);
        uint64_t& m = shareByName_[t.names[b]];
        m = std::max(m, perBranch[b]);
      }
    maxFill_ = std::max(maxFill_, fillBytes);

    // Shadow: did the previous prediction cover this fill?
    if (!h.shadow.empty() && fillBytes) {
      uint64_t hit = 0;
      std::vector<uint32_t> both;
      std::set_intersection(gs.begin(), gs.end(), h.shadow.begin(), h.shadow.end(),
                            std::back_inserter(both));
      for (uint32_t g : both)
        hit += static_cast<uint64_t>(t.size[g]);
      if (!confirmed_.load() && static_cast<double>(hit) >= kConfirmCoverage * fillBytes) {
        confirmed_.store(true);
        UCACHE_INFO("prefetch: prediction confirmed on %s (%.0f%% of a %.1f MB fill); "
                    "reading ahead from here on", j.st->url.c_str(),
                    100.0 * hit / fillBytes, fillBytes / 1e6);
      }
    }

    // Passed over: speculative baskets that EARLIER fills moved the frontier
    // beyond without the reader demanding them. Served ones drop nothing
    // (their mark is gone). A basket of this fill is judged at the next one.
    if (!h.issued.empty()) {
      std::vector<PrefetchHandle::Issued> keep;
      for (const auto& i : h.issued) {
        const uint32_t b = t.branchOf(i.g);
        if (static_cast<int64_t>(i.g - t.boff[b]) <= prevFrontier[b])
          dropped_ += j.entry->dropSpeculative(i.off, i.len);
        else
          keep.push_back(i);
      }
      h.issued.swap(keep);
    }
    if (issued_ >= kGateMinIssued && static_cast<double>(dropped_) > kGateNeverUsed * issued_) {
      disabled_.store(true);
      if (Stats* s = stats(j))
        s->prefetchDisabled.store(1, std::memory_order_relaxed);
      UCACHE_WARN("prefetch: %.0f MB of %.0f MB read ahead were never used; switching off "
                  "for this process", dropped_ / 1e6, issued_ / 1e6);
      for (const auto& i : h.issued)
        j.entry->dropSpeculative(i.off, i.len);
      h.issued.clear();
      return;
    }

    // Predict the next fill: per identified branch, the next baskets until
    // the largest share that branch has drawn in one fill (by name, so a new
    // file starts with what the previous file taught), in entry order, inside
    // the window and the RAM cap.
    struct Cand {
      uint32_t g;
      int64_t es, seek, size;
    };
    std::vector<Cand> cand;
    for (uint32_t b = 0; b < t.nb.size(); ++b) {
      uint64_t budget = h.share[b];
      if (auto it = shareByName_.find(t.names[b]); it != shareByName_.end())
        budget = std::max(budget, it->second);
      if (budget == 0)
        continue;
      uint64_t cum = 0;
      for (int64_t i = h.frontier[b] + 1; i < static_cast<int64_t>(t.nb[b]) && cum < budget; ++i) {
        const uint32_t g = t.boff[b] + static_cast<uint32_t>(i);
        cand.push_back({g, t.es[g], t.seek[g], t.size[g]});
        cum += static_cast<uint64_t>(t.size[g]);
      }
    }
    std::sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) {
      return a.es != b.es ? a.es < b.es : a.seek < b.seek;
    });
    uint64_t window = static_cast<uint64_t>(std::max(1, cfg.prefetchWindowMb)) << 20;
    const uint64_t ramCap = static_cast<uint64_t>(std::max(1, cfg.prefetchRamMb)) << 20;
    const uint64_t staged = FileEntry::speculativeTotal();
    window = std::min(window, ramCap > staged ? ramCap - staged : 0);
    uint64_t cum = 0;
    size_t take = 0;
    while (take < cand.size() && cum < window) {
      cum += static_cast<uint64_t>(cand[take].size);
      ++take;
    }
    cand.resize(take);
    h.shadow.clear();
    for (const auto& c : cand)
      h.shadow.push_back(c.g);
    std::sort(h.shadow.begin(), h.shadow.end());
    if (cand.empty() || !confirmed_.load())
      return; // shadow only: the prediction is judged when the next fill arrives
    issue(j, hp, cand);
  }

  struct Elem {
    uint64_t off, len;
    std::shared_ptr<std::vector<char>> buf;
  };

  // One wire vector read of speculative pages. Completion stages what
  // arrived (or drops it, if the handle has closed) and releases the inner
  // file; a failed read is dropped silently and never trips the breaker.
  class WireHandler : public XrdCl::ResponseHandler {
   public:
    WireHandler(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
                std::shared_ptr<PrefetchHandle> h, std::vector<Elem> elems)
        : st_(std::move(st)), entry_(std::move(entry)), h_(std::move(h)),
          elems_(std::move(elems)), stats_(st_->store ? &st_->store->stats() : nullptr),
          inflight_(stats_) {}
    void HandleResponseWithHosts(XrdCl::XRootDStatus* status, XrdCl::AnyObject* response,
                                 XrdCl::HostList* hosts) override {
      // Give the inner file back and let go of the handle in the same breath:
      // the plugin destructor waits for the former and must not find this
      // handler still holding the latter afterwards. The store outlives us.
      st_->releaseInner();
      st_.reset();
      inflight_.release();
      std::unique_ptr<XrdCl::XRootDStatus> s(status);
      std::unique_ptr<XrdCl::AnyObject> r(response);
      std::unique_ptr<XrdCl::HostList> hl(hosts);
      Stats* stats = stats_;
      uint64_t wire = 0;
      for (const auto& e : elems_)
        wire += e.len;
      if (!s || !s->IsOK()) {
        if (stats)
          stats->prefetchFetchErrors.fetch_add(1, std::memory_order_relaxed);
        delete this;
        return;
      }
      uint64_t late = 0, dropped = 0;
      for (auto& e : elems_) {
        if (h_->closed.load(std::memory_order_acquire)) {
          dropped += e.len; // the reader is gone: never used
          continue;
        }
        const uint64_t got = entry_->stageSpeculative(e.off, e.len, e.buf->data());
        late += e.len - got; // pages a demand read had already brought in
      }
      if (stats) {
        stats->originBytes.fetch_add(wire, std::memory_order_relaxed);
        stats->originReadvs.fetch_add(1, std::memory_order_relaxed);
        if (late)
          stats->prefetchLateBytes.fetch_add(late, std::memory_order_relaxed);
        if (dropped) {
          stats->prefetchDroppedUnread.fetch_add(dropped, std::memory_order_relaxed);
          entry_->obs().prefetchDropped.fetch_add(dropped, std::memory_order_relaxed);
        }
      }
      delete this;
    }

   private:
    std::shared_ptr<HandleState> st_;
    std::shared_ptr<FileEntry> entry_;
    std::shared_ptr<PrefetchHandle> h_;
    std::vector<Elem> elems_;
    Stats* stats_;
    OriginInFlight inflight_;
  };

  template <typename Cand>
  void issue(const Job& j, const std::shared_ptr<PrefetchHandle>& hp, const std::vector<Cand>& cand) {
    PrefetchHandle& h = *hp;
    FileEntry& e = *j.entry;
    // Page-round every predicted basket, merge, keep only what is absent.
    std::vector<std::pair<uint64_t, uint64_t>> ivals;
    ivals.reserve(cand.size());
    for (const auto& c : cand)
      ivals.push_back(roundSpan(e.pageSize(), e.fileSize(), static_cast<uint64_t>(c.seek),
                                static_cast<uint64_t>(c.size)));
    std::sort(ivals.begin(), ivals.end());
    std::vector<std::pair<uint64_t, uint64_t>> runs; // [start, end)
    for (auto [s, en] : ivals) {
      if (!runs.empty() && s <= runs.back().second)
        runs.back().second = std::max(runs.back().second, en);
      else
        runs.emplace_back(s, en);
    }
    std::vector<std::pair<uint64_t, uint64_t>> absent; // (off, len)
    for (auto [s, en] : runs)
      for (const auto& r : e.absentRuns(s, en - s))
        absent.push_back(r);
    if (absent.empty())
      return;
    // Cut into protocol-legal elements and into requests of at most the
    // element cap; every request gets its own handler and inner acquire.
    std::vector<Elem> elems;
    for (auto [s, len] : absent)
      for (uint64_t at = s, en = s + len; at < en;) {
        const uint64_t cut = readvElemEnd(at, en, e.pageSize());
        elems.push_back({at, cut - at, nullptr});
        at = cut;
      }
    uint64_t issuedBytes = 0;
    for (const auto& el : elems)
      issuedBytes += el.len;
    for (const auto& c : cand)
      h.issued.push_back({c.g, static_cast<uint64_t>(c.seek), static_cast<uint64_t>(c.size)});
    issued_ += issuedBytes;
    if (Stats* s = stats(j))
      s->prefetchIssuedBytes.fetch_add(issuedBytes, std::memory_order_relaxed);
    e.obs().prefetchIssued.fetch_add(issuedBytes, std::memory_order_relaxed);

    for (size_t at = 0; at < elems.size(); at += kMaxReadvElems) {
      std::vector<Elem> part(elems.begin() + static_cast<long>(at),
                             elems.begin() + static_cast<long>(std::min(elems.size(), at + kMaxReadvElems)));
      XrdCl::ChunkList wire;
      wire.reserve(part.size());
      for (auto& el : part) {
        el.buf = std::make_shared<std::vector<char>>(el.len);
        wire.emplace_back(el.off, static_cast<uint32_t>(el.len), el.buf->data());
      }
      XrdCl::File* f = j.st->acquireInner();
      if (!f)
        return; // no origin to read ahead from (cache-only handle): nothing lost
      auto* wh = new WireHandler(j.st, j.entry, hp, std::move(part));
      XrdCl::XRootDStatus s = f->VectorRead(wire, nullptr, wh, 0);
      if (!s.IsOK()) {
        delete wh;
        j.st->releaseInner();
        if (Stats* st = stats(j))
          st->prefetchFetchErrors.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
};

Prefetcher::Prefetcher() : impl_(new Impl()) {}

Prefetcher& Prefetcher::instance() {
  static Prefetcher* p = new Prefetcher(); // leaked on purpose: its thread outlives static teardown
  return *p;
}

void Prefetcher::onFill(const std::shared_ptr<HandleState>& st,
                        const std::shared_ptr<FileEntry>& entry, const XrdCl::ChunkList& chunks,
                        bool anyMiss) {
  if (!st || !entry || chunks.empty())
    return;
  if (!globalConfig().prefetch || impl_->disabled_.load(std::memory_order_relaxed))
    return;
  // A handle that has never missed is warm: nothing to read ahead of, and no
  // parse to pay. Once it has missed, every fill matters (a fill served from
  // the speculative stage has no misses, and it is exactly the one to follow).
  if (!anyMiss && !st->prefetchSeen.load(std::memory_order_acquire))
    return;
  Impl::Job j;
  j.st = st;
  j.entry = entry;
  j.chunks.reserve(chunks.size());
  for (const auto& c : chunks)
    j.chunks.emplace_back(c.offset, c.length);
  impl_->post(std::move(j));
}

void Prefetcher::onClose(const std::shared_ptr<HandleState>& st,
                         const std::shared_ptr<FileEntry>& entry) {
  if (!st || !st->prefetchSeen.load(std::memory_order_acquire))
    return;
  Impl::Sync sync;
  Impl::Job j;
  j.st = st;
  j.entry = entry;
  j.close = true;
  j.sync = &sync;
  impl_->post(std::move(j));
  // Closes go to the front of the queue, so this waits for at most the job
  // in progress (a parse, a quarter second on the largest files).
  std::unique_lock<std::mutex> lk(sync.m);
  sync.cv.wait(lk, [&] { return sync.done; });
}

bool Prefetcher::confirmed() const { return impl_->confirmed_.load(); }
bool Prefetcher::disabled() const { return impl_->disabled_.load(); }

} // namespace ucache
