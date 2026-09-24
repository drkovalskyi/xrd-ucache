#include "ColdRun.h"

#include "CacheStore.h"
#include "Executor.h"
#include "FillLayout.h"
#include "RNTupleRewrite.h"
#include "Log.h"
#include "OriginInFlight.h"
#ifdef UCACHE_HAVE_PREFETCH
#include "Prefetch.h"
#endif
#include "PluginSupport.h"
#include "ReadRounding.h"
#include "ReplicaStore.h"
#include "SlotStore.h"
#include "Transposer.h"
#include "UCacheFile.h"
#include "XrdClTimeout.h"
#include "vendor/xxh3.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace ucache {

namespace tp = transpose;
using XrdCl::AnyObject;
using XrdCl::ChunkList;
using XrdCl::HostList;
using XrdCl::ResponseHandler;
using XrdCl::XRootDStatus;

namespace {

// Slots are this many times a basket's stored length. At 3 the ZSTD-1 record
// of all but ~2% of NanoAOD's LZMA baskets fits (0.1% of the bytes); the rest
// is served as it was stored. A warm pass reads 3x as fast as it does 4x and
// the file seen through the cache is smaller.
constexpr uint32_t kSlotFactor = 3;

// Bumped whenever the layout a store was made for could be computed
// differently: a store of another version is replaced, never served.
constexpr uint32_t kLayoutVersion = 1;

// Converted records wait in memory until this many bytes, the periodic
// checkpoint, or the process's last close of the file, then go to the store in
// one commit.
constexpr uint64_t kCommitBytes = 8ull << 20;

// A reader's fill is fetched as several vector reads of about this many bytes
// each, so conversion starts when the first of them lands rather than when the
// whole fill has. Smaller fills go as one request, exactly as they would
// without the cache. A part never carries more items than a vector read may.
constexpr uint64_t kPartBytes = 4ull << 20;
constexpr size_t kPartItems = 512;

// Conversions run here, never on the executor that serves hits: they are
// CPU-bound and as many as the reader has threads, and a hit must not queue
// behind them. Leaked like the executor (threads end with the process).
Executor& convertPool() {
  static Executor* pool = new Executor(std::max(2u, std::thread::hardware_concurrency()));
  return *pool;
}

constexpr uint64_t kHeadBlock = 128 * 1024;

// Commits posted and not yet run. A process that exits normally waits for them
// (bounded); a hard _exit() skips this, and loses at most what the periodic
// checkpoint had not yet committed.
std::mutex g_cmtMu;
std::condition_variable g_cmtCv;
int g_cmtPending = 0;

// The store's kind for a converted basket, and back.
uint8_t entryKind(uint8_t basketKind) {
  return basketKind == tp::ConvertedBasket::kZstd  ? SlotEntry::kZstd
         : basketKind == tp::ConvertedBasket::kRaw ? SlotEntry::kRaw
                                                   : SlotEntry::kKept;
}
uint8_t basketKind(uint8_t entryKind) {
  return entryKind == SlotEntry::kZstd  ? tp::ConvertedBasket::kZstd
         : entryKind == SlotEntry::kRaw ? tp::ConvertedBasket::kRaw
                                        : tp::ConvertedBasket::kOriginal;
}

} // namespace

class ColdFill : public std::enable_shared_from_this<ColdFill> {
 public:
  enum : uint8_t { kAbsent = 0, kFetching = 1, kReady = 2 };
  // What is known of a slot someone has touched. Guarded by ColdFill::mu.
  struct Info {
    uint8_t kind = 0;       // tp::ConvertedBasket::Kind, valid once kReady
    bool inStore = false;   // `e` is a committed entry (anyone's)
    bool persist = false;   // `mem` is to be committed
    bool fromCache = false; // converted from the byte cache's copy of the original
    bool transient = false; // `mem` is served, not to be committed (counted in transientBytes)
    SlotEntry e;
    std::shared_ptr<const std::vector<uint8_t>> mem; // the record, until committed
  };
  using Rec = std::shared_ptr<const std::vector<uint8_t>>;
  // Told when a slot it waited on is ready (with the record, when the one who
  // converted it has it in hand) or was given up.
  using Waiter = std::function<void(bool, Rec)>;
  // RNTuple only: what decoding a page needs, per slot.
  struct Page {
    uint32_t nbytes = 0;
    bool hasChecksum = false;
  };

  UrlKey key;
  std::string cacheDir;
  bool rnt = false;  // an RNTuple container: slots hold DECODED pages
  tp::FillLayout L;
  std::vector<Page> pages; // RNTuple, by slot
  // The original ranges the layout's relocated metadata stands for (the tree
  // record; or the page list and footer), for the read footprint.
  std::vector<std::pair<uint64_t, uint64_t>> metaOrigin;
  std::vector<std::string> codecs; // the store's: which baskets are converted
  uint32_t slotFactor = 0;         // the store's
  bool keepOriginals = false;
  std::shared_ptr<SlotStore> store;
  std::atomic<bool> storeGone{false}; // dropped or replaced: nothing more is committed
  std::shared_ptr<FileEntry> entry;
  std::unique_ptr<std::atomic<uint8_t>[]> state; // per slot: kAbsent / kFetching / kReady

  std::mutex mu; // guards `info` and everything below
  std::unordered_map<uint32_t, Info> info;
  std::unordered_map<uint32_t, std::vector<Waiter>> waiters;
  std::vector<uint32_t> pending;
  uint64_t pendingBytes = 0;
  // Records served but not to be committed (the store is gone, or the
  // process's pending cap is reached): kept for the requests that need them,
  // oldest dropped first past kTransientBytes.
  std::deque<uint32_t> transient;
  uint64_t transientBytes = 0;
  int handles = 0;
  // Original ranges of relocated baskets, sorted: bytes the reader never reads
  // in this layout, so never worth keeping (built on first need).
  std::vector<std::pair<uint64_t, uint64_t>> relocatedOrig;
  // Slot indices in the order of their original offsets (built on first need).
  std::vector<uint32_t> origOrder;
  // Is page `pg` wholly inside baskets that are committed and converted -- so
  // the byte cache's copy of it serves nothing? Caller holds mu.
  bool pageDeadLocked(uint64_t pg, uint64_t ps);

  std::mutex commitMu; // one commit of this file at a time in this process
  std::atomic<bool> commitQueued{false};
  std::atomic<bool> commitAgain{false}; // asked for while one was running

  std::atomic<uint64_t> nZstd{0}, nRaw{0}, nOrig{0}, inBytes{0}, outBytes{0}, convertUs{0},
      wireBytes{0};

  // A snapshot of where slot i's bytes are, if it is ready.
  struct Where {
    uint8_t kind = 0;
    bool inStore = false;
    SlotEntry e;
    std::shared_ptr<const std::vector<uint8_t>> mem;
  };
  bool where(uint32_t i, Where& w) {
    std::lock_guard<std::mutex> g(mu);
    if (state[i].load(std::memory_order_acquire) != kReady)
      return false;
    auto it = info.find(i);
    if (it == info.end())
      return false;
    w.kind = it->second.kind;
    w.inStore = it->second.inStore;
    w.e = it->second.e;
    w.mem = it->second.mem;
    return true;
  }

  // A committed kept-as-stored slot is served from the byte cache's copy of
  // its original; if that copy is gone (evicted), the slot must be fetched.
  bool keptOriginalGone(uint32_t i) {
    if (rnt || state[i].load(std::memory_order_acquire) != kReady)
      return false;
    std::lock_guard<std::mutex> g(mu);
    auto it = info.find(i);
    if (it == info.end() || it->second.kind != tp::ConvertedBasket::kOriginal || it->second.mem)
      return false;
    const tp::FillSlot& fs = L.slots[i];
    if (entry->hasRange(fs.origSeek, fs.origLen))
      return false;
    forgetLocked(i);
    return true;
  }

  // Slot i turned out unreadable (a record failing its CRC, an original gone):
  // absent again, so the next request fetches and converts it anew.
  void forget(uint32_t i) {
    std::lock_guard<std::mutex> g(mu);
    forgetLocked(i);
  }

  // Entries committed by anyone: the slots they name are ready from the store.
  void apply(const std::vector<SlotEntry>& es) {
    std::vector<Waiter> wake;
    {
      std::lock_guard<std::mutex> g(mu);
      for (const auto& e : es) {
        if (e.slot >= L.slots.size())
          continue;
        Info& s = info[e.slot];
        s.inStore = true;
        s.e = e;
        s.kind = basketKind(e.kind);
        // Committed by someone: the store's copy serves, ours is not kept.
        releaseMemLocked(s);
        s.persist = false;
        if (state[e.slot].load(std::memory_order_acquire) != kReady) {
          state[e.slot].store(kReady, std::memory_order_release);
          takeWaitersLocked(e.slot, wake);
        }
      }
    }
    for (auto& w : wake)
      w(true, nullptr);
  }

  // Pick up what other processes committed since we last looked -- unless a
  // commit is under way, in which case this is skipped, not waited for.
  void sync(bool wait = false) {
    if (store) {
      auto es = store->refresh(wait);
      if (!es.empty())
        apply(es);
    }
  }

  // A converted record for slot i: served from memory until it is committed.
  void stage(uint32_t i, uint8_t kind, Rec rec, bool fromCache);

  // A fetch that claimed slot i failed: give the claim back and tell whoever
  // waited on it, so they can fetch it themselves.
  void abandon(uint32_t i) {
    std::vector<Waiter> wake;
    {
      std::lock_guard<std::mutex> g(mu);
      uint8_t expect = kFetching;
      state[i].compare_exchange_strong(expect, kAbsent, std::memory_order_acq_rel);
      takeWaitersLocked(i, wake);
    }
    for (auto& w : wake)
      w(false, nullptr);
  }

  // Claim slot i for fetching (true), or register `cb` to hear when whoever
  // holds it is done (false). A slot found ready returns false with `ready`
  // set and registers nothing. Both looks are compare-and-swaps: a slot
  // released between them is claimed by exactly one caller.
  bool claimOrWait(uint32_t i, Waiter cb, bool& ready) {
    ready = false;
    uint8_t expect = kAbsent;
    if (state[i].compare_exchange_strong(expect, kFetching, std::memory_order_acq_rel))
      return true;
    std::lock_guard<std::mutex> g(mu);
    expect = kAbsent;
    if (state[i].compare_exchange_strong(expect, kFetching, std::memory_order_acq_rel))
      return true;
    if (expect == kReady) {
      ready = true;
      return false;
    }
    waiters[i].push_back(std::move(cb));
    return false;
  }

  // Original bytes worth keeping in the byte cache, of the page-aligned range
  // [rs, re): every page not wholly inside a relocated basket.
  std::vector<std::pair<uint64_t, uint64_t>> storableRuns(uint64_t rs, uint64_t re, uint64_t ps);

  void queueCommit();
  void commit();
  ~ColdFill();

 private:
  void forgetLocked(uint32_t i) {
    uint8_t expect = kReady;
    state[i].compare_exchange_strong(expect, kAbsent, std::memory_order_acq_rel);
    if (auto it = info.find(i); it != info.end()) {
      releaseMemLocked(it->second);
      info.erase(it);
    }
  }
  // Drop this process's copy of a record, and its share of the transient budget.
  void releaseMemLocked(Info& s);
  void takeWaitersLocked(uint32_t i, std::vector<Waiter>& out) {
    auto it = waiters.find(i);
    if (it != waiters.end()) {
      out.swap(it->second);
      waiters.erase(it);
    }
  }
  void dropRecords(const std::vector<SlotRecord>& recs, const std::string& why);
  // Records collected for a commit that will not happen: this process's copy
  // goes (a request that still needs one holds its own), counted as not kept.
  void releaseCollected(const std::vector<SlotRecord>& recs);
};

namespace {

std::mutex g_regMu;
// Leaked: executor tasks may detach during teardown.
std::unordered_map<std::string, std::shared_ptr<ColdFill>>& registry() {
  static auto* r = new std::unordered_map<std::string, std::shared_ptr<ColdFill>>();
  return *r;
}

// The parser's byte source at setup: the byte cache when it has the range,
// otherwise ONE synchronous page-rounded read from the origin, kept in the byte
// cache (these are the file's own records -- header, directory, keys list, tree
// record -- which is exactly what the byte cache holds).
struct SetupSource : tp::Source {
  std::shared_ptr<HandleState> st;
  std::shared_ptr<FileEntry> entry;
  bool has(uint64_t off, uint64_t n) override {
    return off + n >= off && off + n <= entry->fileSize();
  }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (n == 0)
      return true;
    if (entry->hasRange(off, n) && entry->readCached(off, n, dst, /*account=*/false))
      return true;
    auto [ws, we] = roundSpan(entry->pageSize(), entry->fileSize(), off, n);
    return fetch(ws, we, dst, off, n);
  }
  // The file head as ONE block of the size ROOT's RNTuple reader asks for it
  // in (128 KiB at offset 0): setup's piecemeal reads would otherwise leave
  // that read partly missing, and cost one more request. A TTree reader reads
  // only the first few hundred bytes, so this is for RNTuple files only.
  bool fetchHead() {
    const uint64_t n = std::min<uint64_t>(kHeadBlock, entry->fileSize());
    if (entry->hasRange(0, n))
      return true;
    std::vector<char> tmp(1);
    return fetch(0, n, tmp.data(), 0, 0);
  }

 private:
  bool fetch(uint64_t ws, uint64_t we, void* dst, uint64_t off, uint64_t n) {
    if (we - ws > UINT32_MAX)
      return false;
    std::vector<char> buf(we - ws);
    XrdCl::File* f = st->acquireInner();
    if (!f)
      return false;
    uint32_t got = 0;
    const uint64_t t0 = nowUs();
    XRootDStatus s = f->Read(ws, static_cast<uint32_t>(we - ws), buf.data(), got,
                             static_cast<ucache::XrdTimeout>(0));
    st->releaseInner();
    if (!s.IsOK() || got < off + n - ws)
      return false;
    if (st->store) {
      auto& stats = st->store->stats();
      stats.originBytes.fetch_add(got, std::memory_order_relaxed);
      stats.originReads.fetch_add(1, std::memory_order_relaxed);
      stats.originRtUs.add(nowUs() - t0);
    }
    entry->writePages(ws, got, buf.data());
    if (n)
      std::memcpy(dst, buf.data() + (off - ws), n);
    return true;
  }
};

// The tree a reader of this file would be reading: NanoAOD's by name, else the
// first TTree the keys list names.
tp::FileMeta parseTree(tp::Source& src, int64_t size) {
  tp::FileMeta fm = tp::parseFile(src, size, "Events");
  if (!fm.error.empty() && fm.error.find("not found") != std::string::npos) {
    tp::ContainerMeta cm = tp::parseContainer(src, size);
    for (const auto& k : cm.keys)
      if (k.cls == "TTree") {
        fm = tp::parseFile(src, size, k.name);
        break;
      }
  }
  return fm;
}

// Everything a reader could learn from the layout, hashed: two layouts that
// agree on it serve the same bytes at the same offsets.
uint64_t layoutHash(const tp::FillLayout& L, bool rnt) {
  std::vector<uint8_t> b;
  auto put64 = [&](uint64_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + 8);
  };
  put64(rnt ? 1 : 0);
  put64(L.originSize);
  put64(L.virtualSize);
  put64(L.metaSeek);
  put64(L.slotsBegin);
  put64(L.windows.size());
  for (const auto& w : L.windows) {
    put64(w.off);
    put64(w.bytes.size());
    b.insert(b.end(), w.bytes.begin(), w.bytes.end());
  }
  put64(L.metaRecord.size());
  b.insert(b.end(), L.metaRecord.begin(), L.metaRecord.end());
  put64(L.slots.size());
  for (const auto& s : L.slots) {
    put64(s.origSeek);
    put64(s.vSeek);
    put64((static_cast<uint64_t>(s.origLen) << 32) | s.vLen);
  }
  return xxh3_64(b.data(), b.size());
}

std::string joinCodecs(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& c : v)
    s += (s.empty() ? "" : ",") + c;
  return s;
}
std::vector<std::string> splitCodecs(const std::string& s) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : s + ",")
    if (c == ',') {
      if (!cur.empty())
        v.push_back(cur);
      cur.clear();
    } else
      cur += c;
  return v;
}

// ---- the stored layout: what the store's creator computed, as everyone serves it
//
// Serialized once, at creation, and never recomputed: parsing a large tree's
// metadata costs ~200 ms per open, and a layout recomputed by another build
// could differ in its compressed metadata bytes.

constexpr char kLayoutMagic[8] = {'U', 'C', 'L', 'A', 'Y', 'T', '0', '2'};

struct Writer {
  std::vector<uint8_t> b;
  template <typename T> void put(T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof v);
  }
  void bytes(const std::vector<uint8_t>& v) {
    put<uint64_t>(v.size());
    b.insert(b.end(), v.begin(), v.end());
  }
  void varint(uint64_t v) {
    while (v >= 0x80) {
      b.push_back(static_cast<uint8_t>(v | 0x80));
      v >>= 7;
    }
    b.push_back(static_cast<uint8_t>(v));
  }
  void svarint(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
};
struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  template <typename T> T get() {
    T v{};
    if (at + sizeof v > n) {
      ok = false;
      return v;
    }
    std::memcpy(&v, p + at, sizeof v);
    at += sizeof v;
    return v;
  }
  bool bytes(std::vector<uint8_t>& v) {
    const uint64_t len = get<uint64_t>();
    if (!ok || len > n - at)
      return ok = false;
    v.assign(p + at, p + at + len);
    at += len;
    return true;
  }
  uint64_t varint() {
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (at >= n) {
        ok = false;
        return 0;
      }
      const uint8_t c = p[at++];
      v |= static_cast<uint64_t>(c & 0x7f) << shift;
      if (!(c & 0x80))
        return v;
    }
    ok = false;
    return 0;
  }
  int64_t svarint() {
    const uint64_t u = varint();
    return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
  }
};

// A TTree slot's length when nothing capped it: k times the stored basket.
uint32_t plainSlotLen(uint32_t origLen, uint32_t k) {
  const uint64_t v = static_cast<uint64_t>(origLen) * k;
  return v > INT32_MAX ? static_cast<uint32_t>(INT32_MAX) : static_cast<uint32_t>(v);
}

std::vector<uint8_t> encodeLayout(const ColdFill& cf) {
  Writer w;
  w.b.insert(w.b.end(), kLayoutMagic, kLayoutMagic + 8);
  const tp::FillLayout& L = cf.L;
  w.put<uint8_t>(cf.rnt ? 1 : 0);
  w.put<uint64_t>(L.originSize);
  w.put<uint64_t>(L.virtualSize);
  w.put<uint64_t>(L.metaSeek);
  w.put<uint64_t>(L.slotsBegin);
  w.put<uint32_t>(static_cast<uint32_t>(L.windows.size()));
  for (const auto& win : L.windows) {
    w.put<uint64_t>(win.off);
    w.bytes(win.bytes);
  }
  w.bytes(L.metaRecord);
  w.put<uint32_t>(static_cast<uint32_t>(cf.metaOrigin.size()));
  for (const auto& [o, n] : cf.metaOrigin) {
    w.put<uint64_t>(o);
    w.put<uint64_t>(n);
  }
  w.put<uint32_t>(static_cast<uint32_t>(L.relocated.size()));
  for (uint32_t r : L.relocated)
    w.put<uint32_t>(r);
  // The slot table, delta-coded: slots follow one another from slotsBegin, a
  // branch's baskets are consecutive, and a TTree slot is k times its basket
  // unless capped -- so most of a slot is implied by the one before it.
  w.put<uint32_t>(static_cast<uint32_t>(L.slots.size()));
  uint64_t prevOrig = 0, nextV = L.slotsBegin;
  uint32_t prevBranch = 0, prevBasket = 0;
  for (const auto& s : L.slots) {
    w.svarint(static_cast<int64_t>(s.branch) - static_cast<int64_t>(prevBranch));
    w.svarint(static_cast<int64_t>(s.basket) - static_cast<int64_t>(prevBasket) - 1);
    w.svarint(static_cast<int64_t>(s.origSeek - prevOrig));
    w.varint(s.origLen);
    w.varint(!cf.rnt && s.vLen == plainSlotLen(s.origLen, cf.slotFactor) ? 0 : uint64_t(s.vLen) + 1);
    w.svarint(static_cast<int64_t>(s.vSeek - nextV)); // 0 when contiguous
    prevBranch = s.branch;
    prevBasket = s.basket;
    prevOrig = s.origSeek;
    nextV = s.vSeek + s.vLen;
  }
  if (cf.rnt)
    for (const auto& pg : cf.pages) {
      w.put<uint32_t>(pg.nbytes);
      w.put<uint8_t>(pg.hasChecksum ? 1 : 0);
    }
  // Stored compressed: the slot table of a large tree is tens of MB raw.
  std::vector<uint8_t> out(8);
  const uint64_t raw = w.b.size();
  std::memcpy(out.data(), &raw, 8);
  auto z = tp::encodeZstdFrames(w.b.data(), w.b.size(), 3);
  if (z.empty())
    out.insert(out.end(), w.b.begin(), w.b.end()); // stored raw (flagged by length)
  else
    out.insert(out.end(), z.begin(), z.end());
  return out;
}

bool decodeLayout(const std::vector<uint8_t>& blob, ColdFill& cf) {
  if (blob.size() < 8)
    return false;
  uint64_t raw = 0;
  std::memcpy(&raw, blob.data(), 8);
  std::vector<uint8_t> b;
  if (blob.size() - 8 == raw)
    b.assign(blob.begin() + 8, blob.end());
  else
    b = tp::decompressFrames(blob.data() + 8, blob.size() - 8, raw);
  if (b.size() != raw || raw < 8 || std::memcmp(b.data(), kLayoutMagic, 8) != 0)
    return false;
  Reader r{b.data(), b.size(), 8};
  tp::FillLayout& L = cf.L;
  cf.rnt = r.get<uint8_t>() != 0;
  L.originSize = r.get<uint64_t>();
  L.virtualSize = r.get<uint64_t>();
  L.metaSeek = r.get<uint64_t>();
  L.slotsBegin = r.get<uint64_t>();
  const uint32_t nw = r.get<uint32_t>();
  for (uint32_t i = 0; r.ok && i < nw; ++i) {
    tp::FillLayout::Window win;
    win.off = r.get<uint64_t>();
    r.bytes(win.bytes);
    L.windows.push_back(std::move(win));
  }
  r.bytes(L.metaRecord);
  const uint32_t nm = r.get<uint32_t>();
  for (uint32_t i = 0; r.ok && i < nm; ++i) {
    const uint64_t o = r.get<uint64_t>();
    cf.metaOrigin.emplace_back(o, r.get<uint64_t>());
  }
  const uint32_t nr = r.get<uint32_t>();
  if (!r.ok || nr > b.size())
    return false;
  L.relocated.resize(nr);
  for (uint32_t i = 0; r.ok && i < nr; ++i)
    L.relocated[i] = r.get<uint32_t>();
  const uint32_t ns = r.get<uint32_t>();
  if (!r.ok || static_cast<uint64_t>(ns) * 6 > b.size())
    return false;
  L.slots.resize(ns);
  uint64_t prevOrig = 0, nextV = L.slotsBegin;
  uint32_t prevBranch = 0, prevBasket = 0;
  for (uint32_t i = 0; r.ok && i < ns; ++i) {
    tp::FillSlot& s = L.slots[i];
    s.branch = static_cast<uint32_t>(prevBranch + r.svarint());
    s.basket = static_cast<uint32_t>(prevBasket + 1 + r.svarint());
    s.origSeek = prevOrig + static_cast<uint64_t>(r.svarint());
    s.origLen = static_cast<uint32_t>(r.varint());
    const uint64_t vl = r.varint();
    s.vLen = vl ? static_cast<uint32_t>(vl - 1) : plainSlotLen(s.origLen, cf.slotFactor);
    s.vSeek = nextV + static_cast<uint64_t>(r.svarint());
    prevBranch = s.branch;
    prevBasket = s.basket;
    prevOrig = s.origSeek;
    nextV = s.vSeek + s.vLen;
  }
  if (cf.rnt) {
    cf.pages.resize(ns);
    for (uint32_t i = 0; r.ok && i < ns; ++i) {
      cf.pages[i].nbytes = r.get<uint32_t>();
      cf.pages[i].hasChecksum = r.get<uint8_t>() != 0;
    }
  }
  // Checked, not trusted: every slot lies in the layout, in order.
  if (!r.ok || r.at != b.size() || L.virtualSize < L.slotsBegin || L.slotsBegin < L.metaSeek)
    return false;
  uint64_t prev = L.slotsBegin;
  for (const auto& s : L.slots) {
    // (An RNTuple slot is the page's DECODED size, which may be shorter than
    // the page as stored with its checksum; a TTree slot holds the record.)
    if (s.vSeek < prev || s.vSeek + s.vLen > L.virtualSize ||
        (!cf.rnt && s.vLen < s.origLen) || s.origSeek + s.origLen > L.originSize)
      return false;
    prev = s.vSeek + s.vLen;
  }
  for (const auto& win : L.windows)
    if (win.off + win.bytes.size() > L.originSize)
      return false;
  return true;
}

// Parse the file and compute its layout with the given parameters into cf.
// False when the file is not served this way; `declined` then says whether
// that is the file's nature (worth remembering) or a failed read (not).
bool computeLayout(ColdFill& cf, SetupSource& src, uint64_t size, uint32_t k, bool& declined) {
  declined = false;
  std::vector<uint8_t> header(100);
  cf.rnt = false;
  tp::FileMeta fm = parseTree(src, static_cast<int64_t>(size));
  if (!fm.error.empty() && fm.error.find("not found") != std::string::npos) {
    // No TTree: perhaps an RNTuple.
    if (!src.fetchHead())
      return false;
    tp::RNTupleMeta rm = tp::parseRNTuple(src, static_cast<int64_t>(size), "");
    if (!rm.error.empty() || !src.read(header.data(), header.size(), 0)) {
      UCACHE_DEBUG("slot run declined for %s: %s", cf.key.key.c_str(), fm.error.c_str());
      // Remembered only when the file has neither container: any other
      // parse failure may be a read that failed, and is tried again next time.
      declined = rm.error.find("not found") != std::string::npos;
      return false;
    }
    cf.rnt = true;
    cf.L = tp::layoutForRNTupleFill(rm, size, header, cf.codecs);
    cf.metaOrigin = {{rm.pageListOffset, rm.pageListNbytes},
                     {rm.anchor.seekFooter, rm.anchor.nbytesFooter}};
    if (cf.L.error.empty()) {
      cf.pages.resize(cf.L.slots.size());
      for (size_t i = 0; i < cf.L.slots.size(); ++i) {
        const auto& pg = rm.ranges[cf.L.slots[i].branch].pages[cf.L.slots[i].basket];
        cf.pages[i].nbytes = static_cast<uint32_t>(pg.nbytes);
        cf.pages[i].hasChecksum = pg.hasChecksum;
      }
    }
  } else {
    if (!fm.error.empty()) {
      UCACHE_DEBUG("slot run declined for %s: %s", cf.key.key.c_str(), fm.error.c_str());
      return false; // perhaps a failed read: tried again next time, not remembered
    }
    std::vector<uint8_t> treeKeyHeader(fm.treeKey.keylen), keysList;
    uint8_t klLen[4];
    if (!src.read(header.data(), header.size(), 0) ||
        !src.read(treeKeyHeader.data(), treeKeyHeader.size(),
                  static_cast<uint64_t>(fm.treeKey.seekkey)) ||
        !src.read(klLen, 4, static_cast<uint64_t>(fm.keyslistSeek)))
      return false;
    const int32_t kn = static_cast<int32_t>(static_cast<uint32_t>(klLen[0]) << 24 |
                                            static_cast<uint32_t>(klLen[1]) << 16 |
                                            static_cast<uint32_t>(klLen[2]) << 8 | klLen[3]);
    if (kn <= 0 || static_cast<uint64_t>(fm.keyslistSeek) + static_cast<uint64_t>(kn) > size)
      return false;
    keysList.resize(static_cast<size_t>(kn));
    if (!src.read(keysList.data(), keysList.size(), static_cast<uint64_t>(fm.keyslistSeek)))
      return false;
    cf.L = tp::layoutForFill(fm, size, header, treeKeyHeader, keysList, cf.codecs, k);
    cf.metaOrigin = {{static_cast<uint64_t>(fm.treeKey.seekkey),
                      static_cast<uint64_t>(fm.treeKey.nbytes)}};
  }
  if (!cf.L.error.empty()) {
    UCACHE_INFO("slot run declined for %s: %s; the file is served as stored", cf.key.key.c_str(),
                cf.L.error.c_str());
    declined = true; // decided by the file's own metadata
    return false;
  }
  return true;
}

// The replica tier's adoption rule: size always; mtime or checksum only when
// `validate` asks for them (some storage reports differing mtimes for a file
// that has not changed).
bool adoptable(const SlotStoreHeader& h, uint64_t size, uint64_t originMtime, uint8_t cksumKind,
               uint32_t originCksum) {
  const Config& cfg = globalConfig();
  if (h.layoutVersion != kLayoutVersion || h.originSize != size)
    return false;
  if (cfg.validate == ValidateMode::kSizeMtime && h.originMtime != originMtime)
    return false;
  if (cfg.validate == ValidateMode::kCksum && cksumKind != 0 &&
      (h.cksumKind != cksumKind || h.originCksum != originCksum))
    return false;
  return true;
}

// Set up the slot run of a file.
//
// An existing store is served as it is: its layout was computed once, by
// whoever created it, and is never recomputed. With kCreate and no store, the
// layout is computed and a store made (a file whose layout declines gets a
// store marked DECLINED, so the next open need not parse it to find that
// out). kMatch is for a process that has already shown this file's slot
// layout: only a layout with that hash will do.
std::shared_ptr<ColdFill> build(const std::shared_ptr<HandleState>& st,
                                const std::shared_ptr<FileEntry>& entry, const UrlKey& key,
                                uint64_t originMtime, uint8_t cksumKind, uint32_t originCksum,
                                AttachMode mode, uint64_t matchHash) {
  const Config& cfg = globalConfig();
  const uint64_t size = entry->fileSize();
  if (size < 100)
    return nullptr;
  const uint64_t tSetup = nowUs();
  IOBackend& io = RealIO::instance();
  const std::string dir = key.objectDir(cfg.cacheDir);

  auto cf = std::make_shared<ColdFill>();
  cf->key = key;
  cf->cacheDir = cfg.cacheDir;
  cf->entry = entry;
  cf->keepOriginals = cfg.recompressKeepOriginals;

  auto store = SlotStore::open(io, dir, key.hashHex);
  if (store && !adoptable(store->header(), size, originMtime, cksumKind, originCksum)) {
    UCACHE_INFO("slot store for %s was made for another version of the file; replaced",
                key.key.c_str());
    store->dropIfCurrent(); // busy: the stale store is found again below, and declined
    store.reset();
  }
  if (store && store->header().declined) {
    // Served as stored, as decided when the store was made -- unless the codec
    // list it was decided with has changed, which can change the decision.
    if (mode != AttachMode::kCreate || store->header().codecs == joinCodecs(cfg.recompressCodecs))
      return nullptr;
    store->dropIfCurrent(); // busy: found again below, and served as stored this time
    store.reset();
  }
  if (store && mode == AttachMode::kMatch && store->header().layoutHash != matchHash)
    return nullptr; // not the layout this process has shown: never mix two
  bool created = false;
  if (!store) {
    if (mode == AttachMode::kExisting)
      return nullptr;
    // A compact replica is served in its own layout, to readers who may hold
    // its offsets: a store is never made beside one.
    struct ::stat tst;
    if (io.stat(ReplicaStore::tmetaPath(key, cfg.cacheDir), &tst) == 0)
      return nullptr;
    SetupSource src;
    src.st = st;
    src.entry = entry;
    cf->codecs = cfg.recompressCodecs;
    cf->slotFactor = kSlotFactor;
    bool declined = false;
    const bool ok = computeLayout(*cf, src, size, kSlotFactor, declined);
    SlotStoreHeader want;
    want.layoutVersion = kLayoutVersion;
    want.slotFactor = static_cast<uint8_t>(kSlotFactor);
    want.codecs = joinCodecs(cf->codecs);
    want.originSize = size;
    want.originMtime = originMtime;
    want.cksumKind = cksumKind;
    want.originCksum = originCksum;
    std::vector<uint8_t> blob;
    if (!ok) {
      if (!declined || mode != AttachMode::kCreate)
        return nullptr;
      want.declined = true;
    } else {
      want.container = cf->rnt ? 1 : 0;
      want.virtualSize = cf->L.virtualSize;
      want.nSlots = static_cast<uint32_t>(cf->L.slots.size());
      want.layoutHash = layoutHash(cf->L, cf->rnt);
      if (mode == AttachMode::kMatch && want.layoutHash != matchHash)
        return nullptr; // this build cannot reproduce what was shown
      blob = encodeLayout(*cf);
      // Everyone after us serves what this blob decodes to: make sure it does.
      ColdFill check;
      check.slotFactor = cf->slotFactor;
      if (!decodeLayout(blob, check) || layoutHash(check.L, check.rnt) != want.layoutHash) {
        UCACHE_WARN("slot run declined for %s: its layout does not survive storing",
                    key.key.c_str());
        return nullptr;
      }
    }
    std::string err;
    store = SlotStore::openOrCreate(io, dir, key.hashHex, want, blob, created, err);
    if (!store) {
      UCACHE_INFO("slot run declined for %s: %s", key.key.c_str(), err.c_str());
      return nullptr;
    }
    if (store->header().declined)
      return nullptr;
    // A replica published while the layout was computed: one form per file.
    // Every publish checks for a store after its sidecar lands, so of two
    // racing, at least one sees the other.
    if (created && io.stat(ReplicaStore::tmetaPath(key, cfg.cacheDir), &tst) == 0) {
      store->dropIfCurrent();
      store.reset();
      UCACHE_INFO("slot store for %s withdrawn: a replica was published meanwhile",
                  key.key.c_str());
      return nullptr;
    }
    if (!created) { // someone else made it first: theirs is the layout
      if (!adoptable(store->header(), size, originMtime, cksumKind, originCksum) ||
          (mode == AttachMode::kMatch && store->header().layoutHash != matchHash))
        return nullptr;
    }
  }
  if (!created) {
    cf->L = tp::FillLayout(); // the stored layout, in place of anything computed above
    cf->slotFactor = store->header().slotFactor;
    cf->metaOrigin.clear();
    cf->pages.clear();
    if (!decodeLayout(store->layoutBlob(), *cf) ||
        cf->L.slots.size() != store->header().nSlots ||
        cf->L.virtualSize != store->header().virtualSize) {
      UCACHE_WARN("slot store for %s has an unreadable layout; the file is served as stored",
                  key.key.c_str());
      return nullptr;
    }
    cf->codecs = splitCodecs(store->header().codecs);
  }
  cf->store = store;
  cf->state.reset(new std::atomic<uint8_t>[cf->L.slots.size()]());
  // Every record anyone has committed so far -- without waiting, so a commit
  // held up in another process never holds up an open (what is missed now is
  // read at the next request, or converted again and deduplicated).
  cf->sync();
  if (created && st->store)
    st->store->stats().coldReplicaFiles.fetch_add(1, std::memory_order_relaxed);
  UCACHE_INFO("slot run for %s: %zu %s of %zu %s in slots (%s), virtual %llu bytes, "
              "set up in %.1f ms",
              key.key.c_str(), cf->L.slots.size(), cf->rnt ? "pages" : "baskets",
              cf->L.relocated.size(), cf->rnt ? "column ranges" : "branches",
              created ? "new store" : "existing store",
              static_cast<unsigned long long>(cf->L.virtualSize), (nowUs() - tSetup) / 1e3);
  return cf;
}

// ------------------------------------------------------------------ serving

struct ColdRequest;
void serveRequest(const std::shared_ptr<ColdRequest>& req);

struct ColdRequest : std::enable_shared_from_this<ColdRequest> {
  std::shared_ptr<HandleState> st;
  std::shared_ptr<FileEntry> entry;
  std::shared_ptr<ColdFill> cf;
  ChunkList chunks;
  bool isVRead = false;
  ResponseHandler* user = nullptr;
  int attempt = 0;
  bool mayPark = true; // may wait once for read-ahead already on the wire
  uint64_t t0 = 0;

  // Filled in by classify(), consumed when every fetch it started is done.
  struct SlotPiece {
    uint32_t slot;
    uint64_t from, len; // within the slot
    char* dest;
  };
  std::vector<SlotPiece> slotPieces;
  struct OrigPiece {
    uint64_t off, len; // original bytes the byte cache did not have
    char* dest;
  };
  std::vector<OrigPiece> origPieces;
  std::vector<uint32_t> claimed; // slots this request fetches
  // Slots whose records reach this reader from the origin, not from the cache:
  // fetched by this request, or by another one it waited on. Their bytes are
  // the fill; everything else in a slot is the replica tier.
  std::vector<uint32_t> fetched;
  // The records this request copies from, held from the moment they were
  // converted (or found ready) until it has copied them: the file's own copy
  // may be dropped meanwhile -- a commit refused at the free-space floor, the
  // transient budget -- and a request must not lose what it already has.
  std::unordered_map<uint32_t, ColdFill::Where> held; // guarded by emu
  std::vector<uint32_t> converted; // claimed slots this request staged; guarded by emu
  void hold(uint32_t i, ColdFill::Rec r) {
    ColdFill::Where w;
    w.mem = std::move(r);
    std::lock_guard<std::mutex> g(emu);
    held[i] = std::move(w);
  }
  // Slot i is ready now: keep where its bytes are -- the record in memory, or
  // the store entry (whose file stays readable), or the kept original.
  void holdReady(uint32_t i) {
    ColdFill::Where w;
    if (!cf->where(i, w))
      return;
    std::lock_guard<std::mutex> g(emu);
    held.emplace(i, std::move(w)); // a record already held is at least as good
  }

  std::atomic<int> outstanding{1}; // the issuing stage holds one
  std::atomic<bool> failed{false};
  std::atomic<bool> retry{false}; // a slot someone else was fetching fell through
  std::mutex emu;
  XRootDStatus err;

  void fail(const XRootDStatus& s) {
    std::lock_guard<std::mutex> g(emu);
    if (!failed.exchange(true))
      err = s;
  }
  // The last hold released finishes the request -- on the executor, whatever
  // thread let go (an XrdCl callback thread must not do disk reads).
  void done() {
    if (outstanding.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      auto self = shared_from_this();
      Executor::instance().post([self] { self->finish(); });
    }
  }
  void finish();
  // Serve the same chunks again from the start (a slot fell through).
  void again(bool mayParkAgain);
};

// Copy bytes [from, from+len) of slot i -- its record, then zeros -- to dest.
// False when the slot's bytes are not to be had after all: the slot is then
// absent again, and serving the request again fetches it.
bool copySlot(ColdFill& cf, uint32_t i, uint64_t from, uint64_t len, char* dest,
              uint64_t& recBytes, const ColdFill::Where* held) {
  recBytes = 0;
  ColdFill::Where w;
  if (held) {
    w = *held; // what the request took when the slot was ready: nothing else is consulted
  } else if (!cf.where(i, w)) {
    return false;
  }
  const tp::FillSlot& fs = cf.L.slots[i];
  if (from + len > fs.vLen)
    return false;
  std::shared_ptr<const std::vector<uint8_t>> rec = w.mem;
  if (!rec && w.kind == tp::ConvertedBasket::kOriginal && !cf.rnt) {
    // Kept as stored: the original record, from the byte cache, with its
    // fSeekKey pointing at the slot.
    auto buf = std::make_shared<std::vector<uint8_t>>(fs.origLen);
    if (!cf.entry->hasRange(fs.origSeek, fs.origLen) ||
        !cf.entry->readCached(fs.origSeek, fs.origLen, buf->data(), /*account=*/false) ||
        !tp::patchKeySeek(buf->data(), buf->size(), fs.vSeek)) {
      cf.forget(i);
      return false;
    }
    rec = buf;
  } else if (!rec) {
    auto buf = std::make_shared<std::vector<uint8_t>>();
    if (!w.inStore || !cf.store->readRecord(w.e, *buf)) {
      UCACHE_WARN("slot record for %s (slot %u) failed its check; converting it again",
                  cf.key.key.c_str(), i);
      cf.forget(i);
      return false;
    }
    if (auto cs = globalStore()) { // the replica tier's disk reads, as for a compact replica
      auto& stats = cs->stats();
      stats.replicaReads.fetch_add(1, std::memory_order_relaxed);
      stats.replicaReadBytes.fetch_add(buf->size(), std::memory_order_relaxed);
      stats.replicaReadSize.add(buf->size());
    }
    rec = buf;
  }
  if (cf.rnt) {
    // The store keeps the block; the reader is served the page decoded, which
    // is exactly the slot's length.
    if (rec->size() == fs.vLen) {
      std::memcpy(dest, rec->data() + from, len);
      recBytes = len;
      return true;
    }
    std::vector<uint8_t> raw = tp::decompressFrames(rec->data(), rec->size(), fs.vLen);
    if (raw.size() != fs.vLen) {
      cf.forget(i);
      return false;
    }
    std::memcpy(dest, raw.data() + from, len);
    recBytes = len;
    return true;
  }
  const uint64_t recLen = rec->size();
  uint64_t n = 0;
  if (from < recLen) {
    n = std::min<uint64_t>(len, recLen - from);
    std::memcpy(dest, rec->data() + from, n);
  }
  if (n < len)
    std::memset(dest + n, 0, len - n); // the slot's padding
  recBytes = n;
  return true;
}

void ColdRequest::again(bool mayParkAgain) {
  auto r2 = std::make_shared<ColdRequest>();
  r2->st = st;
  r2->entry = entry;
  r2->cf = cf;
  r2->chunks = chunks;
  r2->isVRead = isVRead;
  r2->user = user;
  r2->attempt = attempt + 1;
  r2->mayPark = mayParkAgain;
  r2->t0 = t0;
  Executor::instance().post([r2] { serveRequest(r2); });
}

void ColdRequest::finish() {
  if (failed.load()) {
    // Give back the claims this request still holds -- those it never staged.
    // A slot it staged may have been dropped and claimed by another request
    // since: that claim is not this request's to give back.
    std::vector<uint32_t> done;
    {
      std::lock_guard<std::mutex> g(emu);
      done = converted;
    }
    std::sort(done.begin(), done.end());
    for (uint32_t i : claimed)
      if (!std::binary_search(done.begin(), done.end(), i))
        cf->abandon(i);
    XRootDStatus s;
    {
      std::lock_guard<std::mutex> g(emu);
      s = err;
    }
    // NOT noteCacheError: that trips a handle to pass-through after a few
    // errors, and a pass-through handle would send the origin offsets that
    // exist only in the layout this reader was shown. The origin's error goes
    // to the reader, as it would have without the cache.
    complete(user, new XRootDStatus(s), nullptr);
    return;
  }
  bool lost = retry.load(); // another request's fetch failed
  uint64_t fillRec = 0, replicaRec = 0;
  std::sort(fetched.begin(), fetched.end());
  std::unordered_map<uint32_t, ColdFill::Where> mine;
  {
    std::lock_guard<std::mutex> g(emu);
    mine.swap(held);
  }
  if (!lost)
    for (const auto& p : slotPieces) {
      uint64_t rec = 0;
      auto h = mine.find(p.slot);
      if (!copySlot(*cf, p.slot, p.from, p.len, p.dest, rec,
                    h == mine.end() ? nullptr : &h->second)) {
        lost = true; // a record failed its check, or a kept original is gone
        break;
      }
      (std::binary_search(fetched.begin(), fetched.end(), p.slot) ? fillRec : replicaRec) += rec;
    }
  if (lost) {
    if (attempt < 2) {
      again(false);
      return;
    }
    complete(user, new XRootDStatus(XrdCl::stError, XrdCl::errDataError), nullptr);
    return;
  }
  if (st->store) {
    uint64_t total = 0, origFetched = 0;
    for (const auto& c : chunks)
      total += c.length;
    for (const auto& p : origPieces)
      origFetched += p.len;
    auto& stats = st->store->stats();
    // Every byte handed to the reader is served, as on every other path. Of
    // them, records already in the store (or converted from the byte cache)
    // are the replica tier's; records converted from what the origin just
    // sent, and original bytes fetched, are the fill. A slot's padding is
    // neither: zeros made here, which no tier holds.
    stats.servedBytes.fetch_add(total, std::memory_order_relaxed);
    stats.missBytes.fetch_add(fillRec + origFetched, std::memory_order_relaxed);
    stats.replicaBytesServed.fetch_add(replicaRec, std::memory_order_relaxed);
    entry->obs().replicaBytes.fetch_add(replicaRec, std::memory_order_relaxed);
  }
  entry->noteActivity();
  st->noteCacheOk();
  if (isVRead)
    complete(user, okStatus(), vreadResponse(chunks));
  else
    complete(user, okStatus(), chunkResponse(chunks[0].offset, chunks[0].length, chunks[0].buffer));
}

// Convert one claimed basket and stage it. `rounded` holds the page-rounded
// original range starting at `rStart`; the basket is [recOff, recOff+recLen)
// inside it. `fromCache` = the original came from the byte cache (its copy
// there is freed once the record is committed). Runs on the conversion pool.
void convertOne(const std::shared_ptr<ColdRequest>& req, uint32_t i,
                std::shared_ptr<std::vector<char>> rounded, uint64_t rStart, uint64_t recOff,
                uint32_t recLen, bool fromCache) try {
  ColdFill& cf = *req->cf;
  const tp::FillSlot& slot = cf.L.slots[i];
  const auto* rec = reinterpret_cast<const uint8_t*>(rounded->data() + recOff);
  const uint64_t t0 = nowUs();
  if (cf.rnt) {
    const ColdFill::Page& pg = cf.pages[i];
    tp::ConvertedPage p = tp::convertPage(rec, recLen, pg.nbytes, pg.hasChecksum, slot.vLen);
    const uint64_t us = nowUs() - t0;
    if (!p.error.empty()) {
      // A page whose checksum or decode fails must not be served decoded: the
      // reader could no longer detect it. Fail the read, as ROOT would.
      UCACHE_WARN("slot run for %s: page at %llu: %s", cf.key.key.c_str(),
                  static_cast<unsigned long long>(slot.origSeek), p.error.c_str());
      req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError));
      req->done();
      return;
    }
    const uint8_t kind = p.enc.size() == p.raw.size() ? tp::ConvertedBasket::kRaw
                                                      : tp::ConvertedBasket::kZstd;
    cf.convertUs.fetch_add(us, std::memory_order_relaxed);
    cf.inBytes.fetch_add(recLen, std::memory_order_relaxed);
    cf.outBytes.fetch_add(p.enc.size(), std::memory_order_relaxed);
    (kind == tp::ConvertedBasket::kZstd ? cf.nZstd : cf.nRaw).fetch_add(1, std::memory_order_relaxed);
    if (req->st->store) {
      auto& stats = req->st->store->stats();
      stats.coldReplicaConvertUs.fetch_add(us, std::memory_order_relaxed);
      stats.coldReplicaBaskets.fetch_add(1, std::memory_order_relaxed);
      stats.coldReplicaInBytes.fetch_add(recLen, std::memory_order_relaxed);
      stats.coldReplicaOutBytes.fetch_add(p.enc.size(), std::memory_order_relaxed);
    }
    if (cf.keepOriginals) {
      req->entry->writePages(rStart, rounded->size(), rounded->data());
      req->entry->flushMeta(false);
    } else if (!fromCache) {
      // Fetched for this file, kept only as its record: the file's origin tier
      // (staging counts what goes to the byte cache).
      req->entry->obs().wireBytes.fetch_add(recLen, std::memory_order_relaxed);
    }
    auto rec = std::make_shared<const std::vector<uint8_t>>(std::move(p.enc));
    req->hold(i, rec);
    cf.stage(i, kind, std::move(rec), fromCache);
    { // staged: the claim is no longer this request's to give back
      std::lock_guard<std::mutex> g(req->emu);
      req->converted.push_back(i);
    }
    req->done();
    return;
  }
  tp::ConvertedBasket c = tp::convertBasket(rec, recLen, slot.vLen, cf.codecs);
  if (!c.error.empty()) { // not a basket: the reader gets the origin's bytes, as it would have
    c.kind = tp::ConvertedBasket::kOriginal;
    c.record.assign(rec, rec + recLen);
  }
  const uint64_t us = nowUs() - t0;
  cf.convertUs.fetch_add(us, std::memory_order_relaxed);
  cf.inBytes.fetch_add(recLen, std::memory_order_relaxed);
  cf.outBytes.fetch_add(c.record.size(), std::memory_order_relaxed);
  (c.kind == tp::ConvertedBasket::kZstd  ? cf.nZstd
   : c.kind == tp::ConvertedBasket::kRaw ? cf.nRaw
                                         : cf.nOrig)
      .fetch_add(1, std::memory_order_relaxed);
  if (req->st->store) {
    auto& stats = req->st->store->stats();
    stats.coldReplicaConvertUs.fetch_add(us, std::memory_order_relaxed);
    if (c.kind == tp::ConvertedBasket::kOriginal) {
      stats.coldReplicaBasketsKept.fetch_add(1, std::memory_order_relaxed);
    } else {
      stats.coldReplicaBaskets.fetch_add(1, std::memory_order_relaxed);
      stats.coldReplicaInBytes.fetch_add(recLen, std::memory_order_relaxed);
      stats.coldReplicaOutBytes.fetch_add(c.record.size(), std::memory_order_relaxed);
    }
  }
  if (!tp::patchKeySeek(c.record.data(), c.record.size(), slot.vSeek)) {
    req->fail(XRootDStatus(XrdCl::stError, XrdCl::errDataError));
    req->done();
    return;
  }
  // A kept basket is served from the byte cache, so it is written there even
  // when it came from it: taken from read-ahead's stage, its pages left the
  // stage when used, or are still only speculative.
  if (c.kind == tp::ConvertedBasket::kOriginal || cf.keepOriginals) {
    // Cannot be converted: the one kind of basket the byte cache keeps --
    // unless keeping every original was asked for, to compare the tiers.
    req->entry->writePages(rStart, rounded->size(), rounded->data());
    req->entry->flushMeta(false);
  } else if (!fromCache) {
    // Fetched for this file, kept only as its record: the file's origin tier
    // (staging counts what goes to the byte cache).
    req->entry->obs().wireBytes.fetch_add(recLen, std::memory_order_relaxed);
  }
  auto held = std::make_shared<const std::vector<uint8_t>>(std::move(c.record));
  req->hold(i, held);
  // A kept original is not punched from the byte cache: it is the only copy.
  cf.stage(i, static_cast<uint8_t>(c.kind), std::move(held),
           fromCache && c.kind != tp::ConvertedBasket::kOriginal);
  { // staged: the claim is no longer this request's to give back
    std::lock_guard<std::mutex> g(req->emu);
    req->converted.push_back(i);
  }
  req->done();
} catch (const std::exception& e) {
  UCACHE_WARN("slot run for %s: conversion failed (%s)", req->cf->key.key.c_str(), e.what());
  req->fail(XRootDStatus(XrdCl::stError, XrdCl::errInternal));
  req->done();
}

// One part of a request's fetch: a vector read of the page-rounded original
// ranges of some claimed baskets and of original bytes the byte cache lacked.
struct PartItem {
  bool slot;          // a claimed basket, else an original-bytes piece
  uint32_t index;     // slot index, or index into origPieces
  uint64_t off, len;  // the exact original range
  uint64_t rs, re;    // page-rounded
};
struct WireElem {
  uint64_t off, len;
  std::shared_ptr<std::vector<char>> buf;
};

class PartHandler : public ResponseHandler {
 public:
  PartHandler(std::shared_ptr<ColdRequest> req, std::vector<PartItem> items,
              std::vector<WireElem> wire)
      : req_(std::move(req)), items_(std::move(items)), wire_(std::move(wire)),
        t0_(nowUs()), inflight_(req_->st->store ? &req_->st->store->stats() : nullptr) {}

  void HandleResponseWithHosts(XRootDStatus* status, AnyObject* response,
                               HostList* hostList) override {
    req_->st->releaseInner();
    inflight_.release();
    std::unique_ptr<XRootDStatus> s(status);
    std::unique_ptr<AnyObject> r(response);
    std::unique_ptr<HostList> h(hostList);
    if (!s || !s->IsOK()) {
      req_->fail(s ? *s : XRootDStatus(XrdCl::stError, XrdCl::errInternal));
      req_->done();
      delete this;
      return;
    }
    uint64_t bytes = 0;
    for (const auto& w : wire_)
      bytes += w.len;
    auto& st = req_->st;
    if (st->store) {
      auto& stats = st->store->stats();
      stats.originBytes.fetch_add(bytes, std::memory_order_relaxed);
      stats.originReadvs.fetch_add(1, std::memory_order_relaxed);
      stats.originRtUs.add(nowUs() - t0_);
    }
    req_->cf->wireBytes.fetch_add(bytes, std::memory_order_relaxed);
    for (const auto& it : items_) {
      auto buf = std::make_shared<std::vector<char>>(it.re - it.rs);
      if (!gather(it.rs, it.re - it.rs, buf->data())) {
        req_->fail(XRootDStatus(XrdCl::stError, XrdCl::errInternal));
        continue;
      }
      if (it.slot) {
        req_->outstanding.fetch_add(1, std::memory_order_relaxed);
        auto req = req_;
        const uint64_t recOff = it.off - it.rs;
        const uint32_t recLen = static_cast<uint32_t>(it.len);
        const uint32_t idx = it.index;
        const uint64_t rs = it.rs;
        convertPool().post(
            [req, idx, buf, rs, recOff, recLen] {
              convertOne(req, idx, buf, rs, recOff, recLen, /*fromCache=*/false);
            });
      } else {
        const auto& p = req_->origPieces[it.index];
        std::memcpy(p.dest, buf->data() + (p.off - it.rs), p.len);
        // Original bytes the reader asked for: the byte cache keeps them --
        // except pages wholly inside a relocated basket, which a reader of
        // this layout never needs (a copy tool reading everything would
        // otherwise store every basket a second time).
        auto runs = req_->cf->storableRuns(it.rs, it.re, req_->entry->pageSize());
        if (!runs.empty()) {
          st->beginPersist();
          auto entry = req_->entry;
          const uint64_t rs = it.rs;
          Executor::instance().post([st, entry, buf, rs, runs] {
            for (const auto& [a, b] : runs)
              entry->writePages(a, b - a, buf->data() + (a - rs));
            entry->flushMeta(false);
            st->endPersist();
          });
        }
      }
    }
    req_->done();
    delete this;
  }

 private:
  // Copy [off, off+len) out of the wire elements (sorted, covering it).
  bool gather(uint64_t off, uint64_t len, char* dst) const {
    uint64_t at = off, left = len;
    for (const auto& w : wire_) {
      if (!left)
        break;
      if (at < w.off || at >= w.off + w.len)
        continue;
      const uint64_t n = std::min<uint64_t>(left, w.off + w.len - at);
      std::memcpy(dst + (at - off), w.buf->data() + (at - w.off), n);
      at += n;
      left -= n;
    }
    return left == 0;
  }

  std::shared_ptr<ColdRequest> req_;
  std::vector<PartItem> items_;
  std::vector<WireElem> wire_;
  uint64_t t0_;
  OriginInFlight inflight_;
};

// Issue one part: coalesce its rounded ranges, cut them into legal elements,
// one vector read.
void issuePart(const std::shared_ptr<ColdRequest>& req, std::vector<PartItem> items) {
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (const auto& it : items) { // items arrive sorted by rs
    if (!runs.empty() && it.rs <= runs.back().second)
      runs.back().second = std::max(runs.back().second, it.re);
    else
      runs.emplace_back(it.rs, it.re);
  }
  std::vector<WireElem> wire;
  ChunkList chunks;
  const uint64_t ps = req->entry->pageSize();
  for (auto [s, e] : runs)
    for (uint64_t at = s; at < e;) {
      const uint64_t cut = readvElemEnd(at, e, ps);
      WireElem w{at, cut - at, std::make_shared<std::vector<char>>(cut - at)};
      chunks.emplace_back(w.off, static_cast<uint32_t>(w.len), w.buf->data());
      wire.push_back(std::move(w));
      at = cut;
    }
  req->outstanding.fetch_add(1, std::memory_order_relaxed);
  XrdCl::File* f = req->st->acquireInner();
  if (!f) {
    req->fail(req->st->missError());
    req->done();
    return;
  }
  auto* h = new PartHandler(req, std::move(items), std::move(wire));
  XRootDStatus s = f->VectorRead(chunks, nullptr, h, 0);
  if (!s.IsOK()) {
    delete h;
    req->st->releaseInner();
    req->fail(s);
    req->done();
  }
}

// Walk every chunk: answer what is here now, note what must be fetched.
void classify(const std::shared_ptr<ColdRequest>& req, std::vector<uint32_t>& need) {
  ColdFill& cf = *req->cf;
  const tp::FillLayout& L = cf.L;
  const uint64_t metaEnd = L.metaSeek + L.metaRecord.size();
  for (const auto& c : req->chunks) {
    char* base = static_cast<char*>(c.buffer);
    uint64_t pos = c.offset;
    const uint64_t end = c.offset + c.length;
    while (pos < end) {
      char* d = base + (pos - c.offset);
      if (pos < L.originSize) {
        const uint64_t segEnd = std::min(end, L.originSize);
        // A window, if one covers pos; else the original bytes up to the next one.
        uint64_t next = segEnd;
        bool inWindow = false;
        for (const auto& w : L.windows) {
          const uint64_t we = w.off + w.bytes.size();
          if (pos >= w.off && pos < we) {
            const uint64_t n = std::min(segEnd, we) - pos;
            std::memcpy(d, w.bytes.data() + (pos - w.off), n);
            pos += n;
            inWindow = true;
            break;
          }
          if (w.off > pos)
            next = std::min(next, w.off);
        }
        if (inWindow)
          continue;
        const uint64_t n = next - pos;
        if (!(req->entry->hasRange(pos, n) && req->entry->readCached(pos, n, d)))
          req->origPieces.push_back({pos, n, d});
        pos += n;
      } else if (pos < L.metaSeek) {
        const uint64_t n = std::min(end, L.metaSeek) - pos;
        std::memset(d, 0, n);
        pos += n;
      } else if (pos < metaEnd) {
        const uint64_t n = std::min(end, metaEnd) - pos;
        std::memcpy(d, L.metaRecord.data() + (pos - L.metaSeek), n);
        pos += n;
      } else if (pos < L.slotsBegin) {
        const uint64_t n = std::min(end, L.slotsBegin) - pos;
        std::memset(d, 0, n);
        pos += n;
      } else {
        auto it = std::upper_bound(
            L.slots.begin(), L.slots.end(), pos,
            [](uint64_t v, const tp::FillSlot& s) { return v < s.vSeek; });
        const uint32_t i = static_cast<uint32_t>((it - L.slots.begin()) - 1);
        const tp::FillSlot& s = L.slots[i];
        const uint64_t from = pos - s.vSeek;
        const uint64_t n = std::min<uint64_t>(end, s.vSeek + s.vLen) - pos;
        req->slotPieces.push_back({i, from, n, d});
        if (cf.state[i].load(std::memory_order_acquire) != ColdFill::kReady ||
            cf.keptOriginalGone(i)) {
          need.push_back(i);
        } else {
          req->holdReady(i); // may be dropped from the file's copy before it is copied
        }
        pos += n;
      }
    }
  }
}

void serveRequest(const std::shared_ptr<ColdRequest>& req) {
  ColdFill& cf = *req->cf;
  std::vector<uint32_t> need;
  classify(req, need);
  std::sort(need.begin(), need.end());
  need.erase(std::unique(need.begin(), need.end()), need.end());
  if (!need.empty()) {
    // Another process may have converted them since: use its records rather
    // than fetch and convert the same baskets again.
    cf.sync();
    need.erase(std::remove_if(need.begin(), need.end(),
                              [&](uint32_t i) {
                                if (cf.state[i].load(std::memory_order_acquire) !=
                                    ColdFill::kReady)
                                  return false;
                                req->holdReady(i);
                                return true;
                              }),
               need.end());
  }

#ifdef UCACHE_HAVE_PREFETCH
  const Config& cfg = globalConfig();
  if (cfg.prefetch && req->mayPark && !need.empty()) {
    // Read-ahead learns from this fill in the ORIGINAL file's coordinates --
    // a slot is its basket -- and predicts and fetches the next baskets into
    // the byte cache's speculative stage, from where the conversion below
    // takes them. Only the slots still to be converted: predicting from the
    // ones in the store would fetch originals nobody needs.
    XrdCl::ChunkList orig;
    for (uint32_t i : need)
      orig.emplace_back(cf.L.slots[i].origSeek, cf.L.slots[i].origLen, nullptr);
    Prefetcher::instance().onFill(req->st, req->entry, orig, true);
  }
  if (cfg.prefetch && cfg.prefetchJoin && req->mayPark && !need.empty()) {
    // A basket read ahead may be on the wire right now: wait for that copy
    // (once) instead of fetching it a second time.
    std::vector<std::pair<uint64_t, uint64_t>> want;
    for (uint32_t i : need) {
      const tp::FillSlot& s = cf.L.slots[i];
      if (!req->entry->hasRange(s.origSeek, s.origLen))
        want.emplace_back(s.origSeek, s.origLen);
    }
    if (!want.empty()) {
      auto fired = std::make_shared<std::atomic<bool>>(false);
      auto again = [req, fired] {
        if (fired->exchange(true))
          return;
        auto r2 = std::make_shared<ColdRequest>();
        r2->st = req->st;
        r2->entry = req->entry;
        r2->cf = req->cf;
        r2->chunks = req->chunks;
        r2->isVRead = req->isVRead;
        r2->user = req->user;
        r2->attempt = req->attempt;
        r2->mayPark = false;
        r2->t0 = req->t0;
        Executor::instance().post([r2] { serveRequest(r2); });
      };
      if (req->entry->waitForInFlight(want, again)) {
        Executor::instance().postAfter(10000, again); // a lost wakeup must not hang the read
        return;
      }
    }
  }
#endif

  // Claim what nobody is fetching; wait for what somebody is.
  for (uint32_t i : need) {
    bool ready = false;
    req->outstanding.fetch_add(1, std::memory_order_relaxed);
    auto cb = [req, i](bool ok, ColdFill::Rec r) {
      if (!ok)
        req->retry.store(true);
      else if (r)
        req->hold(i, std::move(r));
      req->done();
    };
    if (cf.claimOrWait(i, cb, ready)) {
      req->claimed.push_back(i);
      req->outstanding.fetch_sub(1, std::memory_order_relaxed); // counted per conversion instead
    } else if (ready) {
      req->holdReady(i); // converted meanwhile, as in classify
      req->outstanding.fetch_sub(1, std::memory_order_relaxed);
    } else {
      req->fetched.push_back(i); // converting elsewhere, from wherever that request got it
    }
  }

  // What to fetch: claimed baskets the byte cache does not hold, and original
  // bytes it did not have. A claimed basket already in the byte cache (a
  // file read before recompression was switched on) converts from there.
  std::vector<PartItem> items;
  const uint64_t ps = req->entry->pageSize(), fs = req->entry->fileSize();
  for (uint32_t i : req->claimed) {
    const tp::FillSlot& s = cf.L.slots[i];
    auto [rs, re] = roundSpan(ps, fs, s.origSeek, s.origLen);
    if (req->entry->hasRange(s.origSeek, s.origLen)) {
      // Its whole pages: a basket kept as stored is written back from them, so
      // none of its bytes stay only in read-ahead's stage.
      auto buf = std::make_shared<std::vector<char>>(re - rs);
      if (req->entry->readCached(rs, re - rs, buf->data(), /*account=*/false)) {
        // Read ahead into the stage: used now, so it leaves the stage as served
        // rather than being written or dropped as never used.
        req->entry->consumeSpeculative(s.origSeek, s.origLen);
        req->outstanding.fetch_add(1, std::memory_order_relaxed);
        const uint64_t rStart = rs;
        convertPool().post([req, i, buf, s, rStart] {
          convertOne(req, i, buf, rStart, s.origSeek - rStart, s.origLen, /*fromCache=*/true);
        });
        continue;
      }
    }
    req->fetched.push_back(i);
    items.push_back({true, i, s.origSeek, s.origLen, rs, re});
  }
  for (uint32_t k = 0; k < req->origPieces.size(); ++k) {
    const auto& p = req->origPieces[k];
    auto [rs, re] = roundSpan(ps, fs, p.off, p.len);
    items.push_back({false, k, p.off, p.len, rs, re});
  }
  std::sort(items.begin(), items.end(),
            [](const PartItem& a, const PartItem& b) { return a.rs < b.rs; });
  // Parts: consecutive items until a part holds kPartBytes, cut only where the
  // next item does not share a page with the part so far.
  std::vector<PartItem> part;
  uint64_t partBytes = 0, partEnd = 0;
  for (const auto& it : items) {
    // Cut by bytes, and by element count: a vector read carries at most 1024.
    if (!part.empty() && (partBytes >= kPartBytes || part.size() >= kPartItems) &&
        it.rs >= partEnd) {
      issuePart(req, std::move(part));
      part.clear();
      partBytes = 0;
    }
    partBytes += it.re - std::max(it.rs, std::min(partEnd, it.re));
    partEnd = std::max(partEnd, it.re);
    part.push_back(it);
  }
  if (!part.empty())
    issuePart(req, std::move(part));
  req->done(); // the issuing stage's own hold
}

} // namespace

// --------------------------------------------------------------- committing

namespace {

// Commits run here: they take the store's exclusive lock, which may wait for
// another process's commit, and nothing that serves reads may wait on that.
// Leaked like the other pools.
Executor& commitPool() {
  static Executor* pool = new Executor(2);
  return *pool;
}

// Records converted in this process and not yet committed, all files. Past
// the cap the thread that converts commits itself -- back-pressure instead of
// memory growth.
std::atomic<uint64_t> g_pendingTotal{0};
constexpr uint64_t kPendingCap = 512ull << 20;
// Records served but not committed (see ColdFill::transient): a cache for a
// reader that reads a slot again, nothing more -- every request holds the
// records it needs itself. Per file, and for the whole process.
constexpr uint64_t kTransientBytes = 16ull << 20;
std::atomic<uint64_t> g_transientTotal{0};
constexpr uint64_t kTransientCap = 256ull << 20;

// The file's first 128 KiB is never punched: ROOT reads it as one block, and
// a hole there sends every open to the origin.
constexpr uint64_t kKeepHead = 128 * 1024;

} // namespace

ColdFill::~ColdFill() {
  g_pendingTotal.fetch_sub(std::min(pendingBytes, g_pendingTotal.load()), std::memory_order_relaxed);
  g_transientTotal.fetch_sub(std::min(transientBytes, g_transientTotal.load()),
                             std::memory_order_relaxed);
  // Served, never committed (the store was gone): not kept.
  uint64_t lost = 0;
  for (const auto& [slot, s] : info)
    lost += s.transient && s.mem && !s.inStore;
  if (lost)
    if (auto store = globalStore())
      store->stats().coldReplicaDeclined.fetch_add(lost, std::memory_order_relaxed);
}

void ColdFill::releaseMemLocked(Info& s) {
  if (s.transient && s.mem) {
    const uint64_t b = std::min<uint64_t>(transientBytes, s.mem->size());
    transientBytes -= b;
    g_transientTotal.fetch_sub(std::min(b, g_transientTotal.load()), std::memory_order_relaxed);
  }
  s.transient = false;
  s.mem.reset();
}

void ColdFill::stage(uint32_t i, uint8_t kind, Rec rec, bool fromCache) {
  std::vector<Waiter> wake;
  bool kick = false;
  uint64_t lost = 0; // records no longer held anywhere: converted again if read
  {
    std::lock_guard<std::mutex> g(mu);
    Info& s = info[i];
    bool keep = true;
    if (!s.inStore) { // someone committed it meanwhile: theirs serves
      releaseMemLocked(s); // staged twice: the newer record replaces the older
      s.kind = kind;
      // A basket kept as stored is served from the byte cache's copy of its
      // original, just written; memory keeps it only if that write did not
      // land.
      const tp::FillSlot& fs = L.slots[i];
      const bool inCache = !rnt && kind == tp::ConvertedBasket::kOriginal &&
                           entry->hasRange(fs.origSeek, fs.origLen);
      const uint64_t b = inCache ? 0 : rec->size();
      s.fromCache = fromCache;
      // Committed, unless the store is gone or the process already holds its
      // cap of records waiting: then it is served to the requests that asked
      // for it (they hold it) and kept only while the transient budget allows
      // -- memory never grows to cope, and nothing that serves reads waits
      // for a commit.
      s.persist = !storeGone.load(std::memory_order_relaxed) &&
                  g_pendingTotal.load(std::memory_order_relaxed) + b <= kPendingCap;
      if (s.persist) {
        s.mem = inCache ? nullptr : rec;
        pending.push_back(i);
        pendingBytes += b;
        g_pendingTotal.fetch_add(b, std::memory_order_relaxed);
        kick = pendingBytes >= kCommitBytes || handles == 0;
      } else if (inCache) {
        // Served from the byte cache: nothing to hold. Committed the next time
        // this slot is staged with room.
      } else {
        // Oldest first, never the record being staged, until both budgets fit.
        while ((transientBytes + b > kTransientBytes ||
                g_transientTotal.load(std::memory_order_relaxed) + b > kTransientCap) &&
               !transient.empty()) {
          const uint32_t j = transient.front();
          transient.pop_front();
          if (j == i)
            continue;
          auto it = info.find(j);
          if (it != info.end() && it->second.transient && !it->second.inStore) {
            forgetLocked(j);
            ++lost;
          }
        }
        keep = transientBytes + b <= kTransientBytes &&
               g_transientTotal.load(std::memory_order_relaxed) + b <= kTransientCap;
        if (!keep)
          ++lost;
        if (keep) {
          s.mem = rec;
          s.transient = true;
          transient.push_back(i);
          transientBytes += b;
          g_transientTotal.fetch_add(b, std::memory_order_relaxed);
        }
        kick = !storeGone.load(std::memory_order_relaxed); // drain what waits
      }
    }
    takeWaitersLocked(i, wake);
    if (keep) {
      state[i].store(kReady, std::memory_order_release);
    } else { // served to whoever asked, and not kept: absent again
      info.erase(i);
      state[i].store(kAbsent, std::memory_order_release);
    }
  }
  for (auto& w : wake)
    w(true, rec);
  if (lost)
    if (auto store = globalStore())
      store->stats().coldReplicaDeclined.fetch_add(lost, std::memory_order_relaxed);
  if (kick)
    queueCommit();
}

bool ColdFill::pageDeadLocked(uint64_t pg, uint64_t ps) {
  if (origOrder.empty() && !L.slots.empty()) {
    origOrder.resize(L.slots.size());
    for (uint32_t k = 0; k < origOrder.size(); ++k)
      origOrder[k] = k;
    std::sort(origOrder.begin(), origOrder.end(), [this](uint32_t x, uint32_t y) {
      return L.slots[x].origSeek < L.slots[y].origSeek;
    });
  }
  const uint64_t a = pg * ps, b = std::min(a + ps, L.originSize);
  if (a < kKeepHead || a >= b)
    return false;
  // The first basket that may reach into the page, then each one in order:
  // every byte must be covered, with no gap, by committed converted baskets.
  auto it = std::lower_bound(origOrder.begin(), origOrder.end(), a, [this](uint32_t k, uint64_t v) {
    return L.slots[k].origSeek + L.slots[k].origLen <= v;
  });
  uint64_t covered = a;
  for (; it != origOrder.end() && covered < b; ++it) {
    const tp::FillSlot& fs = L.slots[*it];
    if (fs.origSeek > covered)
      return false; // bytes in no relocated basket: the file's own records
    auto in = info.find(*it);
    if (in == info.end() || !in->second.inStore || in->second.kind == tp::ConvertedBasket::kOriginal)
      return false; // not converted yet, or kept as stored: its original serves
    covered = std::max<uint64_t>(covered, fs.origSeek + fs.origLen);
  }
  return covered >= b;
}

std::vector<std::pair<uint64_t, uint64_t>> ColdFill::storableRuns(uint64_t rs, uint64_t re,
                                                                  uint64_t ps) {
  {
    std::lock_guard<std::mutex> g(mu);
    if (relocatedOrig.empty() && !L.slots.empty()) {
      std::vector<std::pair<uint64_t, uint64_t>> r;
      r.reserve(L.slots.size());
      for (const auto& s : L.slots)
        r.emplace_back(s.origSeek, s.origSeek + s.origLen);
      std::sort(r.begin(), r.end());
      // As merged runs: a page covered by two adjacent baskets is as dead as
      // one inside a single basket.
      for (const auto& [a, b] : r) {
        if (!relocatedOrig.empty() && a <= relocatedOrig.back().second)
          relocatedOrig.back().second = std::max(relocatedOrig.back().second, b);
        else
          relocatedOrig.emplace_back(a, b);
      }
    }
  }
  std::vector<std::pair<uint64_t, uint64_t>> out;
  for (uint64_t p = rs; p < re; p += ps) {
    const uint64_t pe = std::min(re, p + ps);
    // Is [p, pe) inside the relocated baskets? (The head is always kept: ROOT
    // reads it as one block, and a hole there sends every open to the origin.)
    auto it = std::upper_bound(relocatedOrig.begin(), relocatedOrig.end(),
                               std::make_pair(p, UINT64_MAX));
    const bool dead = p >= kKeepHead && it != relocatedOrig.begin() && std::prev(it)->second >= pe;
    if (dead)
      continue;
    if (!out.empty() && out.back().second == p)
      out.back().second = pe;
    else
      out.emplace_back(p, pe);
  }
  return out;
}

void ColdFill::queueCommit() {
  if (commitQueued.exchange(true))
    return;
  {
    std::lock_guard<std::mutex> g(g_cmtMu);
    ++g_cmtPending;
  }
  auto self = shared_from_this();
  commitPool().post([self] {
    self->commit();
    std::lock_guard<std::mutex> g(g_cmtMu);
    if (--g_cmtPending == 0)
      g_cmtCv.notify_all();
  });
}

void ColdFill::dropRecords(const std::vector<SlotRecord>& recs, const std::string& why) {
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    UCACHE_WARN("slot records for %s not kept (%s); they are converted again when read "
                "(further such cases are not reported)",
                key.key.c_str(), why.c_str());
  releaseCollected(recs);
}

void ColdFill::releaseCollected(const std::vector<SlotRecord>& recs) {
  {
    std::lock_guard<std::mutex> g(mu);
    for (const auto& r : recs) {
      auto it = info.find(r.slot);
      // Not if someone committed it meanwhile, or it was staged again since.
      if (it != info.end() && !it->second.inStore && !it->second.persist)
        forgetLocked(r.slot);
    }
  }
  if (auto store = globalStore())
    store->stats().coldReplicaDeclined.fetch_add(recs.size(), std::memory_order_relaxed);
}

// Commit what waits in memory. Under the store's lock, entries other processes
// committed meanwhile are applied first and their slots skipped -- so no slot
// is written twice -- then the rest goes to the store as one block.
void ColdFill::commit() try {
  // One commit of a file at a time, and a second never waits for the first on
  // a pool thread: the one running goes round again instead.
  // (Flag, then try again: a holder that unlocks after the flag is set sees
  // it; one that unlocked before leaves the lock to this try.)
  std::unique_lock<std::mutex> cg(commitMu, std::defer_lock);
  commitQueued.store(false);
  if (!cg.try_lock()) {
    commitAgain.store(true);
    if (!cg.try_lock())
      return;
  }
  struct Again {
    ColdFill* self;
    std::unique_lock<std::mutex>& lk;
    ~Again() {
      lk.unlock();
      if (self->commitAgain.exchange(false))
        self->queueCommit();
    }
  } again{this, cg};
  std::vector<SlotRecord> recs;
  {
    std::lock_guard<std::mutex> g(mu);
    for (uint32_t i : pending) {
      auto it = info.find(i);
      if (it == info.end())
        continue; // forgotten meanwhile
      Info& s = it->second;
      if (s.inStore) { // committed by someone meanwhile: ours is not kept
        s.persist = false;
        releaseMemLocked(s);
        continue;
      }
      if (!s.persist || state[i].load(std::memory_order_acquire) != kReady ||
          (!s.mem && s.kind != tp::ConvertedBasket::kOriginal))
        continue;
      s.persist = false; // collected once, even if the slot was staged twice
      SlotRecord r;
      r.slot = i;
      r.kind = entryKind(s.kind);
      if (r.kind != SlotEntry::kKept)
        r.bytes = *s.mem;
      recs.push_back(std::move(r));
    }
    pending.clear();
    g_pendingTotal.fetch_sub(pendingBytes, std::memory_order_relaxed);
    pendingBytes = 0;
    // Records served while there was no room to commit them go with this
    // batch, now that it is being written: out of the transient budget, kept
    // in memory until committed.
    if (!storeGone.load()) {
      for (uint32_t j : transient) {
        auto it = info.find(j);
        if (it == info.end() || !it->second.transient || !it->second.mem || it->second.inStore)
          continue;
        Info& s = it->second;
        SlotRecord r;
        r.slot = j;
        r.kind = entryKind(s.kind);
        r.bytes = *s.mem;
        auto keep = s.mem;
        releaseMemLocked(s); // out of the budget ...
        s.mem = std::move(keep); // ... still served until the commit lands
        recs.push_back(std::move(r));
      }
      transient.clear();
    }
  }
  if (recs.empty())
    return;
  if (!store || storeGone.load()) {
    releaseCollected(recs);
    return;
  }
  uint64_t bytes = 0;
  for (const auto& r : recs)
    bytes += r.bytes.size();
  auto cs = globalStore();
  if (cs && bytes > CacheStore::headroomToFloor(cs->config(), RealIO::instance())) {
    // Records are cached like any fill: make room the way a fill does, by
    // eviction (at most every 30 s per process: it scans the cache), and keep
    // them only if that found the room.
    static std::atomic<uint64_t> lastEvictUs{0};
    const uint64_t now = nowUs();
    uint64_t last = lastEvictUs.load(std::memory_order_relaxed);
    if (now - last > 30'000'000 && lastEvictUs.compare_exchange_strong(last, now))
      cs->evictNow();
    if (bytes > CacheStore::headroomToFloor(cs->config(), RealIO::instance())) {
      dropRecords(recs, "no room above the free-space floor");
      return;
    }
  }
  std::vector<SlotEntry> committed;
  const int64_t n = store->commit(
      recs, [this](const std::vector<SlotEntry>& es) { apply(es); },
      [this](uint32_t slot) {
        std::lock_guard<std::mutex> g(mu);
        auto it = info.find(slot);
        return it != info.end() && it->second.inStore;
      },
      globalConfig().fsync != FsyncMode::kOff, committed);
  if (n == -ESTALE) {
    // Dropped or replaced (evicted, removed, another build's): this process
    // keeps serving what it holds, and commits nothing more to it.
    if (!storeGone.exchange(true))
      UCACHE_INFO("slot store for %s was removed; nothing more is committed to it",
                  key.key.c_str());
    releaseCollected(recs);
    return;
  }
  if (n < 0) {
    dropRecords(recs, std::strerror(static_cast<int>(-n)));
    return;
  }
  std::vector<std::pair<uint64_t, uint64_t>> punch;
  std::vector<SlotEntry> forgotten;
  {
    std::lock_guard<std::mutex> g(mu);
    for (const auto& e : committed) {
      auto it = info.find(e.slot);
      if (it == info.end()) { // forgotten meanwhile: its new entry makes it ready
        forgotten.push_back(e);
        continue;
      }
      Info& s = it->second;
      s.inStore = true;
      s.e = e;
      s.kind = basketKind(e.kind);
      // A kept basket's original is the only copy: never punched.
      if (s.fromCache && !keepOriginals && e.kind != SlotEntry::kKept)
        punch.emplace_back(L.slots[e.slot].origSeek,
                           L.slots[e.slot].origSeek + L.slots[e.slot].origLen);
      s.fromCache = false;
      s.persist = false;
      releaseMemLocked(s);
    }
    for (const auto& r : recs) { // skipped: another process's record serves
      auto it = info.find(r.slot);
      if (it != info.end() && it->second.inStore) {
        it->second.persist = false;
        releaseMemLocked(it->second);
      }
    }
  }
  if (!forgotten.empty())
    apply(forgotten);
  // Converted baskets that touch are released as one range, so a page they
  // share goes too -- and so does a page one of them shares with a basket
  // committed earlier. The file's first 128 KiB never does.
  std::sort(punch.begin(), punch.end());
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (const auto& [a, b] : punch) {
    if (!runs.empty() && a <= runs.back().second)
      runs.back().second = std::max(runs.back().second, b);
    else
      runs.emplace_back(a, b);
  }
  punch.clear();
  if (!runs.empty() && entry) {
    const uint64_t ps = entry->pageSize();
    std::lock_guard<std::mutex> g(mu);
    for (auto& [a, b] : runs) {
      if (a % ps && pageDeadLocked(a / ps, ps))
        a -= a % ps;
      if (b % ps && b < L.originSize && pageDeadLocked(b / ps, ps))
        b = std::min<uint64_t>(b + (ps - b % ps), L.originSize);
    }
  }
  for (auto [a, b] : runs) {
    a = std::max<uint64_t>(a, kKeepHead);
    if (a < b)
      punch.emplace_back(a, b - a);
  }
  // Outside the store's lock: accounting may evict, punching takes the byte
  // cache's own lock.
  if (cs && n > 0)
    cs->noteStoredBytes(static_cast<uint64_t>(n));
  // The converted record is the copy now: the original's whole pages go.
  if (!punch.empty() && entry)
    entry->releaseRanges(punch);
} catch (const std::exception& e) {
  UCACHE_WARN("slot store commit for %s failed (%s)", key.key.c_str(), e.what());
}

namespace {

// Which layout this process has shown each file in, for the process's whole
// life: a reader may hold offsets from any open it made, and must never be
// shown a different address space later. Leaked.
std::mutex g_shownMu;
std::unordered_map<std::string, std::pair<ShownLayout, uint64_t>>& shownMap() {
  static auto* m = new std::unordered_map<std::string, std::pair<ShownLayout, uint64_t>>();
  return *m;
}

// At exit: every record still in memory is committed, with a bounded wait. A
// hard _exit() skips this, and loses at most what the periodic checkpoint had
// not yet committed.
void commitAtExit() {
  coldCheckpoint();
  std::unique_lock<std::mutex> lk(g_cmtMu);
  if (!g_cmtCv.wait_for(lk, std::chrono::minutes(2), [] { return g_cmtPending == 0; }))
    UCACHE_WARN("exiting with %d slot-store commit(s) unfinished after 2 min", g_cmtPending);
}

} // namespace

ShownLayout shownLayout(const std::string& key, uint64_t& hash) {
  std::lock_guard<std::mutex> g(g_shownMu);
  auto it = shownMap().find(key);
  if (it == shownMap().end())
    return ShownLayout::kNone;
  hash = it->second.second;
  return it->second.first;
}

ShownLayout noteShownLayout(const std::string& key, ShownLayout s, uint64_t hash,
                            uint64_t* winnerHash) {
  std::lock_guard<std::mutex> g(g_shownMu);
  auto& v = shownMap()[key];
  // Original may later become compact or slot (the original region reads the
  // same in both); nothing else changes once shown -- not even to another
  // compact replica or slot layout of the same file. The caller learns which
  // layout won, and must serve that one.
  if (v.first == ShownLayout::kNone || v.first == ShownLayout::kOriginal ||
      (v.first == s && v.second == hash))
    v = {s, hash};
  if (winnerHash)
    *winnerHash = v.second;
  return v.first;
}

// -------------------------------------------------------------------- API

std::shared_ptr<ColdFill> coldAttach(const std::shared_ptr<HandleState>& st,
                                     const std::shared_ptr<FileEntry>& entry, const UrlKey& key,
                                     uint64_t originMtime, uint8_t cksumKind, uint32_t originCksum,
                                     AttachMode mode, uint64_t matchHash) {
  {
    std::lock_guard<std::mutex> g(g_regMu);
    auto it = registry().find(key.key);
    // Joined only if it is the same file: an entry replaced at the origin
    // meanwhile gets a run of its own.
    if (it != registry().end() && it->second->L.originSize == entry->fileSize() &&
        it->second->entry == entry &&
        (mode != AttachMode::kMatch || it->second->store->header().layoutHash == matchHash)) {
      std::lock_guard<std::mutex> g2(it->second->mu);
      ++it->second->handles;
      return it->second;
    }
  }
  std::shared_ptr<ColdFill> cf;
  try { // network reads: outside the registry lock
    cf = build(st, entry, key, originMtime, cksumKind, originCksum, mode, matchHash);
  } catch (const std::exception& e) {
    UCACHE_WARN("slot run for %s not set up (%s); the file is served as stored", key.key.c_str(),
                e.what());
    return nullptr;
  }
  if (!cf)
    return nullptr;
  // Registered after the store exists, so it runs before the store is torn down.
  static std::once_flag once;
  std::call_once(once, [] { std::atexit(commitAtExit); });
  std::lock_guard<std::mutex> g(g_regMu);
  auto [it, inserted] = registry().emplace(key.key, cf);
  if (!inserted && (it->second->L.originSize != entry->fileSize() || it->second->entry != entry ||
                    (mode == AttachMode::kMatch &&
                     it->second->store->header().layoutHash != matchHash)))
    it->second = cf; // the old run keeps serving its own handles
  std::lock_guard<std::mutex> g2(it->second->mu);
  ++it->second->handles; // a racing handle built it first: join that one, drop ours
  return it->second;
}

void coldDetach(const std::shared_ptr<ColdFill>& cf) {
  if (!cf)
    return;
  bool last = false;
  {
    std::lock_guard<std::mutex> g(g_regMu);
    std::lock_guard<std::mutex> g2(cf->mu);
    if (--cf->handles == 0) {
      last = true;
      auto it = registry().find(cf->key.key);
      if (it != registry().end() && it->second == cf)
        registry().erase(it);
    }
  }
  if (last)
    cf->queueCommit(); // what this process converted goes to the store now
}

void coldCheckpoint() {
  std::vector<std::shared_ptr<ColdFill>> all;
  {
    std::lock_guard<std::mutex> g(g_regMu);
    for (const auto& [k, cf] : registry())
      all.push_back(cf);
  }
  for (const auto& cf : all)
    cf->queueCommit();
}

uint64_t coldVirtualSize(const ColdFill& cf) { return cf.L.virtualSize; }
uint64_t coldLayoutHash(const ColdFill& cf) { return cf.store->header().layoutHash; }

void coldOriginRanges(const ColdFill& cf, uint64_t off, uint64_t len,
                      std::vector<std::pair<uint64_t, uint64_t>>& out) {
  const tp::FillLayout& L = cf.L;
  const uint64_t end = off + len;
  if (off < L.originSize)
    out.emplace_back(off, std::min(end, L.originSize) - off);
  const uint64_t metaEnd = L.metaSeek + L.metaRecord.size();
  if (off < metaEnd && end > L.metaSeek)
    for (const auto& r : cf.metaOrigin)
      out.push_back(r);
  if (end > L.slotsBegin && !L.slots.empty()) {
    auto it = std::upper_bound(L.slots.begin(), L.slots.end(), std::max(off, L.slotsBegin),
                               [](uint64_t v, const tp::FillSlot& s) { return v < s.vSeek; });
    for (--it; it != L.slots.end() && it->vSeek < end; ++it)
      out.emplace_back(it->origSeek, it->origLen);
  }
}

void coldServe(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
               std::shared_ptr<ColdFill> cf, ChunkList chunks, bool isVRead,
               ResponseHandler* user) {
  auto req = std::make_shared<ColdRequest>();
  req->st = std::move(st);
  req->entry = std::move(entry);
  req->cf = std::move(cf);
  req->chunks = std::move(chunks);
  req->isVRead = isVRead;
  req->user = user;
  req->t0 = nowUs();
  serveRequest(req);
}

} // namespace ucache
