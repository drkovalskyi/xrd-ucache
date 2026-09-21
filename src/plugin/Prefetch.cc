#include "Prefetch.h"

#include "Config.h"
#include "FileEntry.h"
#include "Log.h"
#include "OriginInFlight.h"
#include "ReadRounding.h"
#include "Trace.h"
#include "TreeMeta.h"
#include "UCacheFile.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <pthread.h>
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
constexpr uint32_t kBarrenFills = 4;       // fills matching no basket before a table is given up on
constexpr uint64_t kTableCacheBytes = 64ull << 20; // ... and a byte ceiling over all of them
constexpr size_t kNoTableMemo = 64;        // files remembered as having no basket map for us

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

  // Roughly what this table costs in RAM, for the cache's byte ceiling.
  uint64_t bytes() const {
    return sizeof(int64_t) * (seek.size() + size.size() + es.size() + sortedSeek.size()) +
           sizeof(uint32_t) * (sortedIdx.size() + nb.size() + boff.size()) +
           names.size() * (sizeof(std::string) + 24);
  }

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
  std::weak_ptr<FileEntry> entry;   // so the sweep can drop what an abandoned handle staged
  std::shared_ptr<const BasketTable> table;
  int parseTries = 0;            // failed parses so far; the metadata may simply not be staged yet
  bool noTable = false;          // the file did not parse after kParseTries: never look again
  std::vector<int64_t> frontier; // per branch: highest basket index demanded, -1 = none
  std::vector<uint64_t> share;   // per branch: largest bytes drawn in one fill by this handle
  uint64_t fills = 0;
  uint32_t barren = 0;           // consecutive fills whose chunks were no basket of this table
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
  std::unordered_map<std::string, uint64_t> shareByName_; // the memo across files
  uint64_t issued_ = 0;
  std::atomic<uint64_t> inflight_{0}; // bytes on the wire, not yet staged: RAM the cap must see
  std::list<std::pair<std::string, std::shared_ptr<BasketTable>>> tables_; // LRU, front = newest
  std::list<std::string> noTables_; // files with no basket map for us, LRU, front = newest
  // Per-handle state, owned HERE and touched only by the worker thread. Keyed
  // by the handle's address and validated through the weak owner, so a handle
  // that went away without a close (or an address reused by a new handle)
  // never inherits another handle's frontier.
  std::unordered_map<HandleState*, std::shared_ptr<PrefetchHandle>> handles_;
  uint64_t jobs_ = 0;

  Impl() { worker_ = std::thread([this] { loop(); }); worker_.detach(); }

  void post(Job j) {
    Job superseded; // destroyed outside the lock, on the caller's thread
    Sync* orphaned = nullptr; // a waiter whose job we are about to throw away
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
        // ... but if what it supersedes is ANOTHER close, that close has a
        // thread waiting on it. Dropping the job would leave it waiting for a
        // completion that can never come: a permanent hang of an application
        // thread. It is woken below, outside the lock.
        orphaned = superseded.sync;
        it->second = std::move(j);
        order_.erase(std::find(order_.begin(), order_.end(), k));
        order_.push_front(k);
      } else if (!it->second.close) {
        superseded = std::move(it->second); // a newer fill supersedes an older one
        it->second = std::move(j);
      }
    }
    superseded = Job(); // free its references before waking anyone waiting on it
    if (orphaned) {
      std::lock_guard<std::mutex> g(orphaned->m);
      orphaned->done = true;
      orphaned->cv.notify_all();
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
          switchOff(stats(job)); // the counter too: read-ahead off and prefetch_disabled 0
                                 // said nothing had happened
        } catch (...) {
          UCACHE_WARN("prefetch: unknown failure; switching off for this process");
          switchOff(stats(job));
        }
      } // the job, and with it the last reference this thread holds, dies here
      if (sync) {
        std::lock_guard<std::mutex> g(sync->m);
        sync->done = true;
        sync->cv.notify_all();
      }
    }
  }

  Stats* stats(const Job& j) { return j.st && j.st->store ? &j.st->store->stats() : nullptr; }

  void switchOff(Stats* s) {
    disabled_.store(true);
    if (s)
      s->prefetchDisabled.store(1, std::memory_order_relaxed);
  }

  // Never-used bytes, from the entries themselves rather than from what this
  // thread happened to drop: pages dropped at close, by the sweep, by a punch,
  // by the entry's own destructor, and by a completion whose handle had gone
  // were all invisible here, and for a basket smaller than a page EVERY byte
  // was, because the drop rule only takes pages wholly inside a basket. The
  // breaker weighed a full numerator against a partial one.
  uint64_t neverUsed() const { return FileEntry::speculativeDroppedTotal(); }

  // Handles that went away without a close, and closed handles whose owner has
  // now been destroyed. Anything still marked speculative for them is dropped
  // here: erasing the state without dropping left the pages staged until the
  // entry itself died, counted against the process-wide speculative cap the
  // whole time, so a process that lost enough of them stopped reading ahead
  // with no counter and no log line to say so.
  void sweepDeadHandles() {
    for (auto it = handles_.begin(); it != handles_.end();) {
      if (!it->second->owner.expired()) {
        ++it;
        continue;
      }
      PrefetchHandle& h = *it->second;
      if (auto e = h.entry.lock())
        for (const auto& i : h.issued)
          e->dropSpeculative(i.off, i.len);
      it = handles_.erase(it);
    }
  }

  std::shared_ptr<PrefetchHandle> handle(const Job& j) {
    if (++jobs_ % 64 == 0)
      sweepDeadHandles();
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
    // A file with no basket map for us -- an RNTuple container, a nested tree
    // -- is remembered as such. `noTable` lives on the handle, so every new
    // handle on the same file used to pay the full three attempts again: a
    // 1453-file RNTuple dataset opened twice per file spent thousands of
    // futile parses on the one thread every Close waits behind.
    for (const auto& k : noTables_)
      if (k == key)
        return nullptr;
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
    if (!fm.error.empty() || fm.branches.empty()) {
      UCACHE_INFO("prefetch: no basket map for %s yet (%s)", j.st->url.c_str(),
                  fm.error.empty() ? "no branches" : fm.error.c_str());
      return nullptr;
    }
    // Counted on SUCCESS: a basket map parsed, which is what the counter is
    // documented to mean. Counting attempts made the number depend on how
    // much of the header happened to be staged when the second fill arrived.
    if (Stats* s = stats(j))
      s->prefetchParses.fetch_add(1, std::memory_order_relaxed);
    auto t = BasketTable::build(fm, size);
    tables_.emplace_front(key, t);
    // Trimmed by BYTES as well as by count: one 1500-branch file's map is
    // about 21 MB, so eight of them is 170 MB held for the life of the
    // process, outside every configured cap and reported nowhere.
    uint64_t held = 0;
    for (auto it = tables_.begin(); it != tables_.end();) {
      held += it->second->bytes();
      if (it != tables_.begin() &&
          (tables_.size() > kTableCache || held > kTableCacheBytes)) {
        held -= it->second->bytes();
        it = tables_.erase(it);
      } else {
        ++it;
      }
    }
    return t;
  }

  void rememberNoTable(const std::string& key) {
    noTables_.push_front(key);
    while (noTables_.size() > kNoTableMemo)
      noTables_.pop_back();
  }

  void handleClose(const Job& j) {
    auto hp = existing(j);
    if (!hp)
      return;
    PrefetchHandle& h = *hp;
    h.closed.store(true, std::memory_order_release);
    if (j.entry)
      for (const auto& i : h.issued)
        j.entry->dropSpeculative(i.off, i.len);
    h.issued.clear();
    h.issued.shrink_to_fit();
    h.shadow.clear();
    h.shadow.shrink_to_fit();
    h.frontier.clear();
    h.share.clear();
    h.table.reset(); // the table itself stays in tables_, keyed by the file
    // The state STAYS in the map as a tombstone until the sweep finds the
    // handle gone. Erasing it here let a fill that arrived after the close --
    // or one still queued when it happened -- create fresh state whose
    // `closed` was false, which is why the guard in handleFill could never
    // fire. Completions still in flight hold their own reference to it.
  }

  void handleFill(const Job& j) {
    const Config& cfg = globalConfig();
    if (!cfg.prefetch || disabled_.load())
      return;
    // No fill stage, no read-ahead. With fill_buffer_mb = 0 (the legacy
    // direct-write mode) stageSpeculative can hold nothing, so every predicted
    // byte was fetched from the origin and thrown away on arrival -- booked as
    // "late", which is the one outcome the breaker does not weigh, so it ran
    // for the life of the process.
    if (cfg.fillBufferMb <= 0)
      return;
    auto hp = handle(j);
    PrefetchHandle& h = *hp;
    if (h.closed.load())
      return;
    h.entry = j.entry;
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
        if (++h.parseTries >= kParseTries) {
          h.noTable = true;
          rememberNoTable(j.entry->key().key); // every later handle on this file skips it
        }
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
    if (gs.empty()) {
      // The file-open metadata reads look like this, and so does a table built
      // for the wrong tree: a multi-tree file whose keys list names a small
      // side tree first parsed fine and then matched nothing the reader ever
      // asked for, leaving read-ahead inert for that file with a parse counted
      // and nothing said. Give up after a few fills, and say why.
      if (h.table && ++h.barren >= kBarrenFills) {
        UCACHE_INFO("prefetch: the basket map of %s matches nothing this reader asks for "
                    "(another tree in the same file); not reading ahead for it",
                    j.st->url.c_str());
        h.table.reset();
        h.noTable = true;
      }
      return;
    }
    h.barren = 0;
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
          j.entry->dropSpeculative(i.off, i.len);
        else
          keep.push_back(i);
      }
      h.issued.swap(keep);
    }
    const uint64_t never = neverUsed();
    if (issued_ >= kGateMinIssued && static_cast<double>(never) > kGateNeverUsed * issued_) {
      switchOff(stats(j));
      UCACHE_WARN("prefetch: %.0f MB of %.0f MB read ahead were never used; switching off "
                  "for this process", never / 1e6, issued_ / 1e6);
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
    // Bytes ON THE WIRE count against the cap as well as bytes already staged.
    // The cap used to see only what had landed, so on a slow origin -- where
    // nothing lands before the next handle is served -- it read near zero for
    // every handle in turn, and 32 readers each took a full window: about a
    // gigabyte of buffers against a stated ceiling of half that.
    const uint64_t held = FileEntry::speculativeTotal() + inflight_.load(std::memory_order_relaxed);
    window = std::min(window, ramCap > held ? ramCap - held : 0);
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
                std::shared_ptr<PrefetchHandle> h, std::vector<Elem> elems, Impl* owner,
                uint64_t wireBytes)
        : st_(std::move(st)), entry_(std::move(entry)), h_(std::move(h)),
          elems_(std::move(elems)), stats_(st_->store ? &st_->store->stats() : nullptr),
          inflight_(stats_), owner_(owner), wireBytes_(wireBytes),
          issuedUs_(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count()) {}
    // The prefetcher outlives every handler (it is leaked with its thread), so
    // giving the bytes back here is safe on every exit, including the issue
    // that XrdCl refused.
    // Every exit runs this: completion, error status, and an issue XrdCl
    // refused. The registry must never keep a range no one is fetching, or a
    // demand read parked behind it waits for a landing that cannot come.
    ~WireHandler() override {
      owner_->inflight_.fetch_sub(wireBytes_, std::memory_order_relaxed);
      std::vector<std::pair<uint64_t, uint64_t>> rs;
      rs.reserve(elems_.size());
      for (const auto& e : elems_)
        rs.emplace_back(e.off, e.len);
      entry_->clearFetchInFlight(rs); // one withdrawal, one wake
    }
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
        // The speculative read's own round trip, as trace op "swire": the
        // demand path's "wire" records never include these, so without it the
        // origin time read-ahead spends is invisible to any profile.
        if (stats->tracer) {
          const uint64_t now = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
          stats->tracer->rec("swire", entry_->key().key, elems_.front().off, wire,
                             now - issuedUs_, /*sampled=*/false);
        }
        if (late)
          stats->prefetchLateBytes.fetch_add(late, std::memory_order_relaxed);
      }
      // Outside the `if (stats)`: bytes that arrived for a handle which has
      // gone are never-used whether or not this process has a stats file, and
      // the breaker reads them from the entry, not from here.
      if (dropped)
        entry_->noteSpeculativeDropped(dropped);
      delete this;
    }

   private:
    std::shared_ptr<HandleState> st_;
    std::shared_ptr<FileEntry> entry_;
    std::shared_ptr<PrefetchHandle> h_;
    std::vector<Elem> elems_;
    Stats* stats_;
    OriginInFlight inflight_;
    Impl* owner_;
    uint64_t wireBytes_;
    uint64_t issuedUs_;
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
    // Counted per request that the client ACCEPTED, never up front. Counting
    // the whole prediction and then returning on a failed issue put bytes in
    // the breaker's denominator that no page was ever staged for, so they
    // could never come back as never-used: a failing origin made the breaker
    // harder to trip, which is backwards.
    uint64_t sent = 0;
    for (size_t at = 0; at < elems.size(); at += kMaxReadvElems) {
      std::vector<Elem> part(elems.begin() + static_cast<long>(at),
                             elems.begin() + static_cast<long>(std::min(elems.size(), at + kMaxReadvElems)));
      uint64_t partBytes = 0;
      XrdCl::ChunkList wire;
      wire.reserve(part.size());
      for (auto& el : part) {
        el.buf = std::make_shared<std::vector<char>>(el.len);
        wire.emplace_back(el.off, static_cast<uint32_t>(el.len), el.buf->data());
        partBytes += el.len;
      }
      XrdCl::File* f = j.st->acquireInnerIfOpen();
      if (!f)
        break; // no origin open to read ahead from: the reader's own miss opens it
      inflight_.fetch_add(partBytes, std::memory_order_relaxed);
      // Announce the ranges BEFORE the request goes out, so a demand read that
      // arrives while it is in flight waits for this copy instead of sending
      // its own. The handler's destructor withdraws them on every path.
      for (const auto& el : part)
        j.entry->noteFetchInFlight(el.off, el.len);
      auto* wh = new WireHandler(j.st, j.entry, hp, std::move(part), this, partBytes);
      XrdCl::XRootDStatus s = f->VectorRead(wire, nullptr, wh, 0);
      if (!s.IsOK()) {
        delete wh; // its destructor gives the in-flight bytes back
        j.st->releaseInner();
        if (Stats* st = stats(j))
          st->prefetchFetchErrors.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      sent += partBytes;
    }
    if (!sent)
      return;
    for (const auto& c : cand)
      h.issued.push_back({c.g, static_cast<uint64_t>(c.seek), static_cast<uint64_t>(c.size)});
    issued_ += sent;
    if (Stats* s = stats(j))
      s->prefetchIssuedBytes.fetch_add(sent, std::memory_order_relaxed);
    e.obs().prefetchIssued.fetch_add(sent, std::memory_order_relaxed);
  }
};

Prefetcher::Prefetcher() : impl_(new Impl()) {}

namespace {
Prefetcher* gPrefetcher = nullptr;
} // namespace

Prefetcher& Prefetcher::instance() {
  static Prefetcher* p = [] {
    auto* q = new Prefetcher(); // leaked on purpose: its thread outlives static teardown
    gPrefetcher = q;
    // fork() copies the queue and its mutex but NOT the worker thread, so a
    // child that inherited an open handle would post a close and wait on a
    // thread that does not exist -- a permanent hang at file close, in
    // exactly the Python worker pools that fork. The child starts over with
    // an empty queue and a live thread; the old state is leaked, since the
    // only thread that could have been using it is gone.
    ::pthread_atfork(nullptr, nullptr, [] {
      if (gPrefetcher)
        gPrefetcher->impl_ = new Impl();
    });
    return q;
  }();
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
  // Mark the handle HERE, on the calling thread, before the job is queued.
  // The worker used to set it when it first built state for the handle, and
  // onClose reads it to decide whether there is anything to wait for: a handle
  // whose first fill was still in the queue therefore closed without posting a
  // close at all, and the worker then went on to build state for it, see an
  // open handle, and read ahead for a file the application had let go. Nothing
  // afterwards could drop those pages -- no further fill, no close, and the
  // sweep had no way to reach them -- so they sat in the speculative pool for
  // the life of the entry and shrank every other handle's window.
  st->prefetchSeen.store(true, std::memory_order_release);
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
