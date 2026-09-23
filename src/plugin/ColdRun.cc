#include "ColdRun.h"

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
#include "Transposer.h"
#include "UCacheFile.h"
#include "XrdClTimeout.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

namespace ucache {

namespace tp = transpose;
using XrdCl::AnyObject;
using XrdCl::ChunkList;
using XrdCl::HostList;
using XrdCl::ResponseHandler;
using XrdCl::XRootDStatus;

namespace {

// Slots are this many times a basket's stored length: enough for the ZSTD-1
// record of >= 99% of what LZMA and ZLIB baskets decode to, measured on
// NanoAOD. A basket whose record does not fit is served as it was stored.
constexpr uint32_t kSlotFactor = 4;

// A reader's fill is fetched as several vector reads of about this many bytes
// each, so conversion starts when the first of them lands rather than when the
// whole fill has. Smaller fills go as one request, exactly as they would
// without the cache.
constexpr uint64_t kPartBytes = 4ull << 20;

// Conversions run here, never on the executor that serves hits: they are
// CPU-bound and as many as the reader has threads, and a hit must not queue
// behind them. Leaked like the executor (threads end with the process).
Executor& convertPool() {
  static Executor* pool = new Executor(std::max(2u, std::thread::hardware_concurrency()));
  return *pool;
}

std::atomic<uint64_t> g_stagingSeq{0};

constexpr uint64_t kHeadBlock = 128 * 1024;

// Publishes still running. A process that exits normally waits for them: the
// stage is an unlinked file, so a publish cut short by exit is a replica lost,
// and a short job reading a few files would otherwise never leave one. A hard
// _exit() skips this and loses them -- which is also all it can lose.
std::mutex g_pubMu;
std::condition_variable g_pubCv;
int g_pubPending = 0;

void waitForPublishes() {
  std::unique_lock<std::mutex> lk(g_pubMu);
  if (g_pubPending == 0)
    return;
  const int pending = g_pubPending;
  if (!g_pubCv.wait_for(lk, std::chrono::minutes(10), [] { return g_pubPending == 0; }))
    UCACHE_WARN("exiting with %d cold-run replica publish(es) unfinished after 10 min", pending);
}

} // namespace

class ColdFill {
 public:
  enum : uint8_t { kAbsent = 0, kFetching = 1, kReady = 2 };
  struct Slot {
    std::atomic<uint8_t> state{kAbsent};
    uint8_t kind = 0;  // tp::ConvertedBasket::Kind, valid once kReady
    uint64_t off = 0;  // in the staging file
    uint32_t len = 0;  // the record's length
  };

  UrlKey key;
  std::string cacheDir;
  bool rnt = false;  // an RNTuple container: slots hold DECODED pages
  tp::FileMeta fm;   // TTree
  tp::RNTupleMeta rm; // RNTuple
  tp::FillLayout L;
  // The original ranges the layout's relocated metadata stands for (the tree
  // record; or the page list and footer), for the read footprint.
  std::vector<std::pair<uint64_t, uint64_t>> metaOrigin;
  std::vector<uint8_t> treeKeyHeader, keysList;
  std::vector<std::string> codecs;
  bool keepOriginals = false;
  uint64_t originMtime = 0;
  uint8_t cksumKind = 0;
  uint32_t originCksum = 0;
  std::unique_ptr<Slot[]> slots;
  int fd = -1;
  std::atomic<uint64_t> appendOff{0};

  std::mutex mu; // guards everything below
  std::unordered_map<uint32_t, std::vector<std::function<void(bool)>>> waiters;
  std::unordered_map<uint32_t, std::vector<uint8_t>> mem; // records the staging file refused
  int handles = 0;

  std::atomic<uint64_t> nZstd{0}, nRaw{0}, nOrig{0}, inBytes{0}, outBytes{0}, convertUs{0},
      wireBytes{0};

  ~ColdFill() {
    if (fd >= 0)
      ::close(fd);
  }

  // Bytes [from, from+n) of slot i's staged record into dst.
  bool readRecord(uint32_t i, uint8_t* dst, uint64_t from, uint64_t n) {
    const Slot& s = slots[i];
    if (from + n > s.len)
      return false;
    {
      std::lock_guard<std::mutex> g(mu);
      auto it = mem.find(i);
      if (it != mem.end()) {
        std::memcpy(dst, it->second.data() + from, n);
        return true;
      }
    }
    uint64_t at = 0;
    while (at < n) {
      ssize_t r = ::pread(fd, dst + at, n - at, static_cast<off_t>(s.off + from + at));
      if (r <= 0) {
        if (r < 0 && errno == EINTR)
          continue;
        return false;
      }
      at += static_cast<uint64_t>(r);
    }
    return true;
  }

  // Stage a converted record for slot i and publish it to readers.
  void stage(uint32_t i, uint8_t kind, std::vector<uint8_t>&& rec) {
    Slot& s = slots[i];
    s.kind = kind;
    s.len = static_cast<uint32_t>(rec.size());
    s.off = appendOff.fetch_add(rec.size(), std::memory_order_relaxed);
    bool ok = true;
    uint64_t at = 0;
    while (at < rec.size()) {
      ssize_t w = ::pwrite(fd, rec.data() + at, rec.size() - at, static_cast<off_t>(s.off + at));
      if (w <= 0) {
        if (w < 0 && errno == EINTR)
          continue;
        ok = false;
        break;
      }
      at += static_cast<uint64_t>(w);
    }
    std::vector<std::function<void(bool)>> wake;
    {
      std::lock_guard<std::mutex> g(mu);
      if (!ok) // disk full or similar: keep it in memory rather than fail the reader
        mem.emplace(i, std::move(rec));
      s.state.store(kReady, std::memory_order_release);
      auto it = waiters.find(i);
      if (it != waiters.end()) {
        wake.swap(it->second);
        waiters.erase(it);
      }
    }
    for (auto& w : wake)
      w(true);
  }

  // A fetch that claimed slot i failed: give the claim back and tell whoever
  // waited on it, so they can fetch it themselves.
  void abandon(uint32_t i) {
    std::vector<std::function<void(bool)>> wake;
    {
      std::lock_guard<std::mutex> g(mu);
      slots[i].state.store(kAbsent, std::memory_order_release);
      auto it = waiters.find(i);
      if (it != waiters.end()) {
        wake.swap(it->second);
        waiters.erase(it);
      }
    }
    for (auto& w : wake)
      w(false);
  }

  // Claim slot i for fetching (true), or register `cb` to hear when whoever
  // holds it is done (false). A slot found ready returns false with `ready`
  // set and registers nothing.
  bool claimOrWait(uint32_t i, std::function<void(bool)> cb, bool& ready) {
    ready = false;
    uint8_t expect = kAbsent;
    if (slots[i].state.compare_exchange_strong(expect, kFetching, std::memory_order_acq_rel))
      return true;
    std::lock_guard<std::mutex> g(mu);
    const uint8_t now = slots[i].state.load(std::memory_order_acquire);
    if (now == kReady) {
      ready = true;
      return false;
    }
    if (now == kAbsent) { // released between the two looks: take it
      slots[i].state.store(kFetching, std::memory_order_release);
      return true;
    }
    waiters[i].push_back(std::move(cb));
    return false;
  }

  void publish();
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
// record -- which is exactly what the byte cache holds on a cold run).
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
    // The file head is fetched as ONE block of the size ROOT itself asks for
    // it in (128 KiB at offset 0): setup's piecemeal reads would otherwise
    // leave the reader's own head read partly missing, and one more request.
    if (ws < kHeadBlock)
      we = std::max<uint64_t>(we, std::min<uint64_t>(kHeadBlock, entry->fileSize()));
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

std::shared_ptr<ColdFill> build(const std::shared_ptr<HandleState>& st,
                                const std::shared_ptr<FileEntry>& entry, const UrlKey& key) {
  const Config& cfg = globalConfig();
  const uint64_t size = entry->fileSize();
  if (size < 100)
    return nullptr;
  SetupSource src;
  src.st = st;
  src.entry = entry;
  auto cf = std::make_shared<ColdFill>();
  cf->codecs = cfg.recompressCodecs;
  cf->keepOriginals = cfg.recompressKeepOriginals;
  cf->fm = parseTree(src, static_cast<int64_t>(size));
  std::vector<uint8_t> header(100);
  if (!cf->fm.error.empty() && cf->fm.error.find("not found") != std::string::npos) {
    // No TTree: perhaps an RNTuple.
    cf->rm = tp::parseRNTuple(src, static_cast<int64_t>(size), "");
    if (!cf->rm.error.empty() || !src.read(header.data(), header.size(), 0)) {
      UCACHE_DEBUG("cold run declined for %s: %s", key.key.c_str(), cf->fm.error.c_str());
      return nullptr;
    }
    cf->rnt = true;
    cf->L = tp::layoutForRNTupleFill(cf->rm, size, header, cf->codecs);
    cf->metaOrigin = {{cf->rm.pageListOffset, cf->rm.pageListNbytes},
                      {cf->rm.anchor.seekFooter, cf->rm.anchor.nbytesFooter}};
  }
  if (!cf->rnt && !cf->fm.error.empty()) {
    UCACHE_DEBUG("cold run declined for %s: %s", key.key.c_str(), cf->fm.error.c_str());
    return nullptr;
  }
  if (!cf->rnt) {
    cf->treeKeyHeader.resize(cf->fm.treeKey.keylen);
    uint8_t klLen[4];
    if (!src.read(header.data(), header.size(), 0) ||
        !src.read(cf->treeKeyHeader.data(), cf->treeKeyHeader.size(),
                  static_cast<uint64_t>(cf->fm.treeKey.seekkey)) ||
        !src.read(klLen, 4, static_cast<uint64_t>(cf->fm.keyslistSeek)))
      return nullptr;
    const int32_t kn = static_cast<int32_t>(static_cast<uint32_t>(klLen[0]) << 24 |
                                            static_cast<uint32_t>(klLen[1]) << 16 |
                                            static_cast<uint32_t>(klLen[2]) << 8 | klLen[3]);
    if (kn <= 0 || static_cast<uint64_t>(cf->fm.keyslistSeek) + static_cast<uint64_t>(kn) > size)
      return nullptr;
    cf->keysList.resize(static_cast<size_t>(kn));
    if (!src.read(cf->keysList.data(), cf->keysList.size(), static_cast<uint64_t>(cf->fm.keyslistSeek)))
      return nullptr;
    cf->L = tp::layoutForFill(cf->fm, size, header, cf->treeKeyHeader, cf->keysList, cf->codecs,
                              kSlotFactor);
    cf->metaOrigin = {{static_cast<uint64_t>(cf->fm.treeKey.seekkey),
                       static_cast<uint64_t>(cf->fm.treeKey.nbytes)}};
  }
  if (!cf->L.error.empty()) {
    UCACHE_INFO("cold run declined for %s: %s; the file is served as stored", key.key.c_str(),
                cf->L.error.c_str());
    return nullptr;
  }
  // The stage: created and unlinked at once, so a crash leaves nothing behind.
  const std::string dir = key.objectDir(cfg.cacheDir);
  const std::string path = dir + "/" + key.hashHex + ".cold." + std::to_string(::getpid()) + "." +
                           std::to_string(g_stagingSeq.fetch_add(1));
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    UCACHE_WARN("cold run declined for %s: cannot create its stage (%s)", key.key.c_str(),
                std::strerror(errno));
    return nullptr;
  }
  ::unlink(path.c_str());
  cf->fd = fd;
  cf->key = key;
  cf->cacheDir = cfg.cacheDir;
  cf->slots.reset(new ColdFill::Slot[cf->L.slots.size()]);
  if (st->store)
    st->store->stats().coldReplicaFiles.fetch_add(1, std::memory_order_relaxed);
  UCACHE_INFO("cold run for %s: %zu %s of %zu %s in slots, virtual %llu bytes", key.key.c_str(),
              cf->L.slots.size(), cf->rnt ? "pages" : "baskets", cf->L.relocated.size(),
              cf->rnt ? "column ranges" : "branches",
              static_cast<unsigned long long>(cf->L.virtualSize));
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
};

// Copy bytes [from, from+len) of slot i -- its record, then zeros -- to dest.
bool copySlot(ColdFill& cf, uint32_t i, uint64_t from, uint64_t len, char* dest) {
  if (cf.rnt) {
    // The stage keeps the block the replica will hold; the reader of this pass
    // is served the page decoded, which is exactly the slot's length.
    const uint32_t vLen = cf.L.slots[i].vLen;
    std::vector<uint8_t> enc(cf.slots[i].len);
    if (from + len > vLen || !cf.readRecord(i, enc.data(), 0, enc.size()))
      return false;
    if (enc.size() == vLen) {
      std::memcpy(dest, enc.data() + from, len);
      return true;
    }
    std::vector<uint8_t> raw = tp::decompressFrames(enc.data(), enc.size(), vLen);
    if (raw.size() != vLen)
      return false;
    std::memcpy(dest, raw.data() + from, len);
    return true;
  }
  const uint32_t recLen = cf.slots[i].len;
  uint64_t n = 0;
  if (from < recLen) {
    n = std::min<uint64_t>(len, recLen - from);
    if (!cf.readRecord(i, reinterpret_cast<uint8_t*>(dest), from, n))
      return false;
  }
  if (n < len)
    std::memset(dest + n, 0, len - n);
  return true;
}

void ColdRequest::finish() {
  if (failed.load()) {
    for (uint32_t i : claimed)
      if (cf->slots[i].state.load(std::memory_order_acquire) != ColdFill::kReady)
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
  if (retry.load()) { // another request's fetch failed: serve again, fetching ourselves
    if (attempt < 2) {
      auto again = std::make_shared<ColdRequest>();
      again->st = st;
      again->entry = entry;
      again->cf = cf;
      again->chunks = std::move(chunks);
      again->isVRead = isVRead;
      again->user = user;
      again->attempt = attempt + 1;
      again->mayPark = false;
      again->t0 = t0;
      Executor::instance().post([again] { serveRequest(again); });
      return;
    }
    complete(user, new XRootDStatus(XrdCl::stError, XrdCl::errDataError), nullptr);
    return;
  }
  for (const auto& p : slotPieces)
    if (!copySlot(*cf, p.slot, p.from, p.len, p.dest)) {
      complete(user, new XRootDStatus(XrdCl::stError, XrdCl::errOSError), nullptr);
      return;
    }
  if (st->store) {
    uint64_t total = 0;
    for (const auto& c : chunks)
      total += c.length;
    st->store->stats().servedBytes.fetch_add(total, std::memory_order_relaxed);
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
// inside it. Runs on the conversion pool.
void convertOne(const std::shared_ptr<ColdRequest>& req, uint32_t i,
                std::shared_ptr<std::vector<char>> rounded, uint64_t rStart, uint64_t recOff,
                uint32_t recLen) {
  ColdFill& cf = *req->cf;
  const tp::FillSlot& slot = cf.L.slots[i];
  const auto* rec = reinterpret_cast<const uint8_t*>(rounded->data() + recOff);
  const uint64_t t0 = nowUs();
  if (cf.rnt) {
    const auto& pg = cf.rm.ranges[slot.branch].pages[slot.basket];
    tp::ConvertedPage p = tp::convertPage(rec, recLen, pg.nbytes, pg.hasChecksum, pg.uncompressedBytes);
    const uint64_t us = nowUs() - t0;
    if (!p.error.empty()) {
      // A page whose checksum or decode fails must not be served decoded: the
      // reader could no longer detect it. Fail the read, as ROOT would.
      UCACHE_WARN("cold run for %s: page at %llu: %s", cf.key.key.c_str(),
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
    }
    cf.stage(i, kind, std::move(p.enc));
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
  if (c.kind == tp::ConvertedBasket::kOriginal || cf.keepOriginals) {
    // Cannot be converted: the one kind of basket the byte cache keeps --
    // unless keeping every original was asked for, to compare the tiers.
    req->entry->writePages(rStart, rounded->size(), rounded->data());
    req->entry->flushMeta(false);
  }
  cf.stage(i, static_cast<uint8_t>(c.kind), std::move(c.record));
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
            [req, idx, buf, rs, recOff, recLen] { convertOne(req, idx, buf, rs, recOff, recLen); });
      } else {
        const auto& p = req_->origPieces[it.index];
        std::memcpy(p.dest, buf->data() + (p.off - it.rs), p.len);
        // Original bytes the reader asked for that are not a converted basket:
        // the byte cache keeps them.
        st->beginPersist();
        auto entry = req_->entry;
        const uint64_t rs = it.rs;
        Executor::instance().post([st, entry, buf, rs] {
          entry->writePages(rs, buf->size(), buf->data());
          entry->flushMeta(false);
          st->endPersist();
        });
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
        if (cf.slots[i].state.load(std::memory_order_acquire) != ColdFill::kReady)
          need.push_back(i);
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

#ifdef UCACHE_HAVE_PREFETCH
  const Config& cfg = globalConfig();
  if (cfg.prefetch && req->mayPark) {
    // Read-ahead learns from this fill in the ORIGINAL file's coordinates --
    // a slot is its basket -- and predicts and fetches the next baskets into
    // the byte cache's speculative stage, from where the conversion below
    // takes them. Slots are identified once per fill, not once per piece.
    std::vector<uint32_t> ids;
    for (const auto& p : req->slotPieces)
      ids.push_back(p.slot);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    XrdCl::ChunkList orig;
    for (uint32_t i : ids)
      orig.emplace_back(cf.L.slots[i].origSeek, cf.L.slots[i].origLen, nullptr);
    if (!orig.empty())
      Prefetcher::instance().onFill(req->st, req->entry, orig, !need.empty());
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
    auto cb = [req](bool ok) {
      if (!ok)
        req->retry.store(true);
      req->done();
    };
    if (cf.claimOrWait(i, cb, ready)) {
      req->claimed.push_back(i);
      req->outstanding.fetch_sub(1, std::memory_order_relaxed); // counted per conversion instead
    } else if (ready) {
      req->outstanding.fetch_sub(1, std::memory_order_relaxed);
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
      auto buf = std::make_shared<std::vector<char>>(s.origLen);
      if (req->entry->readCached(s.origSeek, s.origLen, buf->data(), /*account=*/false)) {
        // Read ahead into the stage: used now, so it leaves the stage as served
        // rather than being written or dropped as never used.
        req->entry->consumeSpeculative(s.origSeek, s.origLen);
        req->outstanding.fetch_add(1, std::memory_order_relaxed);
        convertPool().post([req, i, buf, s] { convertOne(req, i, buf, s.origSeek, 0, s.origLen); });
        continue;
      }
    }
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
    if (!part.empty() && partBytes >= kPartBytes && it.rs >= partEnd) {
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

// --------------------------------------------------------------- publishing

void ColdFill::publish() {
  std::vector<tp::RelocatedBasket> list;
  std::vector<uint32_t> idx;
  for (uint32_t i = 0; i < L.slots.size(); ++i)
    if (slots[i].state.load(std::memory_order_acquire) == kReady &&
        slots[i].kind != tp::ConvertedBasket::kOriginal) {
      list.push_back({L.slots[i].branch, L.slots[i].basket});
      idx.push_back(i);
    }
  const auto converted = nZstd.load() + nRaw.load();
  if (list.empty()) {
    UCACHE_DEBUG("cold run for %s converted nothing; no replica", key.key.c_str());
    return;
  }
  tp::Overlay ov;
  if (rnt) {
    // Pages are addressed by (range, page) here and by original offset in the
    // slots: a page several records share has one slot.
    std::unordered_map<uint64_t, uint32_t> byOffset;
    for (uint32_t i = 0; i < L.slots.size(); ++i)
      byOffset.emplace(L.slots[i].origSeek, i);
    auto slotOf = [&](size_t ri, size_t pi) -> int64_t {
      auto it = byOffset.find(rm.ranges[ri].pages[pi].offset);
      if (it == byOffset.end() || slots[it->second].state.load(std::memory_order_acquire) != kReady)
        return -1;
      return it->second;
    };
    auto rw = tp::buildRNTupleRewriteFromPages(
        rm, L.originSize, 1, [&](size_t ri, size_t pi) { return slotOf(ri, pi) >= 0; },
        [&](size_t ri, size_t pi, std::vector<uint8_t>& enc) {
          const int64_t i = slotOf(ri, pi);
          if (i < 0)
            return false;
          enc.resize(slots[i].len);
          return readRecord(static_cast<uint32_t>(i), enc.data(), 0, enc.size());
        });
    ov = tp::rnTupleOverlay(rm, rw);
  } else {
    ov = tp::buildOverlayFromRecords(fm, treeKeyHeader, keysList, list,
                                     [&](size_t j, std::vector<uint8_t>& out) {
                                       out.resize(slots[idx[j]].len);
                                       return readRecord(idx[j], out.data(), 0, out.size());
                                     });
  }
  if (!ov.error.empty()) {
    UCACHE_WARN("cold run for %s: replica not built (%s)", key.key.c_str(), ov.error.c_str());
    if (auto store = globalStore())
      store->stats().coldReplicaDeclined.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ReplicaMeta meta = ov.meta;
  meta.originMtime = originMtime;
  meta.cksumKind = cksumKind;
  meta.originCksum = originCksum;
  auto store = globalStore();
  if (!store)
    return;
  // Never evict for a replica: the reader's working set was cached for a
  // reason, and a replica written past the floor makes the next fill evict it.
  const uint64_t head = CacheStore::headroomToFloor(store->config(), RealIO::instance());
  if (ov.tdata.size() > head) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      UCACHE_WARN("cold run for %s: replica not published, %llu bytes do not fit the %llu "
                  "bytes above the free-space floor (further such files are not reported)",
                  key.key.c_str(), static_cast<unsigned long long>(ov.tdata.size()),
                  static_cast<unsigned long long>(head));
    store->stats().coldReplicaDeclined.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ReplicaStore rs(RealIO::instance(), store->config(), store->stats());
  // One publisher per file at a time, across processes: the entry's own data
  // file is the lock. The first complete publish wins; a later one would only
  // replace a valid replica with another.
  int lk = ::open(key.dataPath(cacheDir).c_str(), O_RDONLY | O_CLOEXEC);
  if (lk >= 0)
    ::flock(lk, LOCK_EX);
  struct ::stat sb;
  const bool exists = ::stat(ReplicaStore::tmetaPath(key, cacheDir).c_str(), &sb) == 0;
  int rc = exists ? 0 : rs.publish(key, meta, ov.tdata.data(), ov.tdata.size());
  if (lk >= 0) {
    ::flock(lk, LOCK_UN);
    ::close(lk);
  }
  if (exists)
    UCACHE_INFO("cold run for %s: a replica was published meanwhile; this one is dropped",
                key.key.c_str());
  else if (rc != 0) {
    UCACHE_WARN("cold run for %s: replica publish failed (%s)", key.key.c_str(),
                std::strerror(-rc));
    store->stats().coldReplicaDeclined.fetch_add(1, std::memory_order_relaxed);
  }
  else
    UCACHE_INFO("cold run for %s: replica published, %zu baskets (%llu converted, %llu kept as "
                "stored), %llu -> %llu bytes, %.1f s converting",
                key.key.c_str(), list.size(), static_cast<unsigned long long>(converted),
                static_cast<unsigned long long>(nOrig.load()),
                static_cast<unsigned long long>(inBytes.load()),
                static_cast<unsigned long long>(outBytes.load()), convertUs.load() / 1e6);
}

// -------------------------------------------------------------------- API

std::shared_ptr<ColdFill> coldAttach(const std::shared_ptr<HandleState>& st,
                                     const std::shared_ptr<FileEntry>& entry, const UrlKey& key,
                                     uint64_t originMtime, uint8_t cksumKind,
                                     uint32_t originCksum) {
  {
    std::lock_guard<std::mutex> g(g_regMu);
    auto it = registry().find(key.key);
    if (it != registry().end()) {
      std::lock_guard<std::mutex> g2(it->second->mu);
      ++it->second->handles;
      return it->second;
    }
  }
  auto cf = build(st, entry, key); // network reads: outside the registry lock
  if (!cf)
    return nullptr;
  // Registered after the store exists, so it runs before the store is torn down.
  static std::once_flag once;
  std::call_once(once, [] { std::atexit(waitForPublishes); });
  cf->originMtime = originMtime;
  cf->cksumKind = cksumKind;
  cf->originCksum = originCksum;
  std::lock_guard<std::mutex> g(g_regMu);
  auto [it, inserted] = registry().emplace(key.key, cf);
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
  if (last) {
    {
      std::lock_guard<std::mutex> g(g_pubMu);
      ++g_pubPending;
    }
    auto keep = cf;
    convertPool().post([keep] {
      keep->publish();
      std::lock_guard<std::mutex> g(g_pubMu);
      if (--g_pubPending == 0)
        g_pubCv.notify_all();
    });
  }
}

uint64_t coldVirtualSize(const ColdFill& cf) { return cf.L.virtualSize; }

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
