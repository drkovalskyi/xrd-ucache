#include "FileEntry.h"

#include "Log.h"
#include "Trace.h"
#include "vendor/crc32c.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sstream>
#include <sys/file.h>

namespace ucache {
namespace {
uint64_t nowS() { return static_cast<uint64_t>(::time(nullptr)); }

uint64_t nowUsSteady() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

// Minimal JSON escape for URL keys (quote/backslash; control bytes dropped).
std::string jsonEscapeMin(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    if (static_cast<unsigned char>(c) >= 0x20)
      out += c;
  }
  return out;
}
} // namespace

std::atomic<uint64_t> FileEntry::g_bufTotal_{0};

std::shared_ptr<FileEntry> FileEntry::open(IOBackend& io, const Config& cfg, Stats& stats,
                                           const UrlKey& key, uint64_t originSize,
                                           uint64_t originMtime, uint8_t cksumKind,
                                           uint32_t originCksum,
                                           std::function<void(uint64_t, bool)> onPersist) {
  auto e = std::shared_ptr<FileEntry>(new FileEntry(io, cfg, stats, key));
  e->dataPath_ = key.dataPath(cfg.cacheDir);
  e->metaPath_ = key.metaPath(cfg.cacheDir);
  e->onPersist_ = std::move(onPersist);
  if (io.mkdirs(key.objectDir(cfg.cacheDir), 0700) < 0)
    return nullptr;
  e->dataFd_ = io.open(e->dataPath_, O_RDWR | O_CREAT, 0600);
  if (e->dataFd_ < 0)
    return nullptr;

  // Full-meta load under shared lock.
  io.flock(e->dataFd_, LOCK_SH);
  auto loaded = MetaFile::load(io, e->metaPath_);
  io.flock(e->dataFd_, LOCK_UN);

  bool metaFileExists = false;
  {
    struct ::stat st;
    metaFileExists = io.stat(e->metaPath_, &st) == 0;
  }

  bool adopt = false;
  if (loaded) {
    const MetaData& m = *loaded;
    adopt = m.key == key.key && m.fileSize == originSize &&
            Config::validPageSize(m.pageSize) && m.npages() == m.pageCrcs.size();
    if (adopt && cfg.validate == ValidateMode::kSizeMtime)
      adopt = m.originMtime == originMtime;
    if (adopt && cfg.validate == ValidateMode::kCksum && cksumKind != MetaData::kCksumNone)
      adopt = m.cksumKind == cksumKind && m.originCksum == originCksum;
    if (!adopt) {
      stats.validationsFailed.fetch_add(1, std::memory_order_relaxed);
      UCACHE_INFO("validation failed for %s (stale sidecar); starting fresh", key.key.c_str());
    }
  } else if (metaFileExists) {
    stats.metaCorrupt.fetch_add(1, std::memory_order_relaxed);
    UCACHE_WARN("corrupt/torn sidecar for %s; starting fresh", key.key.c_str());
  }

  if (adopt) {
    e->meta_ = std::move(*loaded);
  } else {
    // Fresh start: logical size = origin size, sparse.
    if (io.ftruncate(e->dataFd_, 0) < 0 || io.ftruncate(e->dataFd_, originSize) < 0) {
      io.close(e->dataFd_);
      return nullptr;
    }
    e->meta_ = MetaData::fresh(key.key, originSize, cfg.pageSize);
    e->meta_.originMtime = originMtime;
    e->meta_.cksumKind = cksumKind;
    e->meta_.originCksum = originCksum;
    e->dirty_ = true;
  }
  e->lastFlushS_ = nowS();
  e->lastBufFlushS_ = nowS();
  stats.opens.fetch_add(1, std::memory_order_relaxed);
  return e;
}

FileEntry::~FileEntry() {
  closing_ = true;
  flushAll();
  if (dataFd_ >= 0)
    io_.close(dataFd_);
  // Per-file record: one lifetime record per entry, at last release (or at
  // the final stats dump for entries teardown never releases — emit-once).
  emitObsRecord();
  // Drop any staged bytes that a failed flush left behind from the global
  // accounting (the pages themselves die with the maps).
  std::lock_guard<std::mutex> g(mu_);
  if (bufBytes_)
    g_bufTotal_.fetch_sub(bufBytes_, std::memory_order_relaxed);
}

void FileEntry::emitObsRecord() {
  if (!obsSink_ || obs_.opens.load(std::memory_order_relaxed) == 0)
    return;
  if (obsEmitted_.exchange(true))
    return;
  auto v = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
  std::ostringstream os;
  os << "{\"ts\":" << nowS() << ",\"key\":\"" << jsonEscapeMin(key_.key) << '"'
     << ",\"opens\":" << v(obs_.opens) << ",\"served_bytes\":" << v(obs_.servedBytes)
     << ",\"ram_bytes\":" << v(obs_.ramBytes)
     << ",\"replica_bytes\":" << v(obs_.replicaBytes)
     << ",\"disk_reads\":" << v(obs_.diskReads) << ",\"disk_seq\":" << v(obs_.diskSeq)
     << ",\"disk_bytes\":" << v(obs_.diskBytes)
     << ",\"first_touch_bytes\":" << v(obs_.firstTouchBytes)
     << ",\"wire_bytes\":" << v(obs_.wireBytes) << "}\n";
  obsSink_->append(os.str());
}

void FileEntry::ObsSink::append(const std::string& line) {
  std::lock_guard<std::mutex> g(mu_);
  if (fd_ < 0) {
    fd_ = io_.open(path_, O_WRONLY | O_CREAT, 0600);
    if (fd_ < 0)
      return; // observability loss is acceptable; job health is not
  }
  if (io_.pwriteFull(fd_, line.data(), line.size(), off_) == static_cast<int64_t>(line.size()))
    off_ += line.size();
}

bool FileEntry::hasRange(uint64_t off, uint64_t len) {
  if (len == 0 || off + len > meta_.fileSize)
    return false;
  uint64_t first = off / meta_.pageSize;
  uint64_t last = (off + len - 1) / meta_.pageSize;
  std::lock_guard<std::mutex> g(mu_);
  if (buf_.empty() && flushing_.empty())
    return meta_.bitmap.rangeSet(first, last);
  for (uint64_t i = first; i <= last; ++i)
    if (!meta_.bitmap.get(i) && !buf_.count(i) && !flushing_.count(i))
      return false;
  return true;
}

bool FileEntry::readVerifyPage(uint64_t i, uint32_t expectCrc, uint8_t* scratch) {
  const uint32_t nbytes = meta_.pageBytes(i);
  int64_t r = io_.preadFull(dataFd_, scratch, nbytes, i * meta_.pageSize);
  bool ok = r == static_cast<int64_t>(nbytes) && crc32c(scratch, nbytes) == expectCrc;
  if (!ok) {
    std::lock_guard<std::mutex> g(mu_);
    meta_.bitmap.clear(i);
    meta_.pageCrcs[i] = 0;
    meta_.flags &= ~MetaData::kFlagComplete;
    dirty_ = true;
    stats_.crcFailures.fetch_add(1, std::memory_order_relaxed);
    UCACHE_WARN("CRC mismatch on page %llu of %s; marked absent",
                static_cast<unsigned long long>(i), key_.key.c_str());
  }
  return ok;
}

bool FileEntry::readCached(uint64_t off, uint64_t len, void* buf) {
  if (len == 0)
    return true;
  if (off + len > meta_.fileSize)
    return false;
  const uint32_t P = meta_.pageSize;
  uint64_t first = off / P, last = (off + len - 1) / P;
  std::vector<uint8_t> scratch(P);
  auto* out = static_cast<uint8_t*>(buf);

  // Accounted locally, published at every exit — partial work on a
  // miss/CRC-demotion still describes real disk activity.
  uint64_t ramB = 0, ftB = 0, dReads = 0, dBytes = 0, dSeq = 0;
  auto publish = [&] {
    if (ramB) {
      stats_.ramHitBytes.fetch_add(ramB, std::memory_order_relaxed);
      obs_.ramBytes.fetch_add(ramB, std::memory_order_relaxed);
    }
    if (dReads) {
      stats_.hitDiskReads.fetch_add(dReads, std::memory_order_relaxed);
      stats_.hitDiskBytes.fetch_add(dBytes, std::memory_order_relaxed);
      stats_.hitDiskSeq.fetch_add(dSeq, std::memory_order_relaxed);
      obs_.diskReads.fetch_add(dReads, std::memory_order_relaxed);
      obs_.diskBytes.fetch_add(dBytes, std::memory_order_relaxed);
      obs_.diskSeq.fetch_add(dSeq, std::memory_order_relaxed);
    }
    if (ftB) {
      stats_.firstTouchBytes.fetch_add(ftB, std::memory_order_relaxed);
      obs_.firstTouchBytes.fetch_add(ftB, std::memory_order_relaxed);
    }
  };

  for (uint64_t i = first; i <= last; ++i) {
    uint64_t pStart = i * P;
    uint64_t cStart = std::max(off, pStart);
    uint64_t cEnd = std::min(off + len, pStart + meta_.pageBytes(i));
    uint32_t expect;
    {
      // Staged pages serve straight from RAM — during a fill the
      // cache disk sees no random reads. Copy under the lock: a concurrent
      // flush completion frees flushing_ payloads under this same mutex.
      std::lock_guard<std::mutex> g(mu_);
      // first_touch: page-granular, counted at the serving read's width. The
      // disk branch marks it here too (before the pread) — a CRC-demoted
      // page inflates it by one page width, a negligible skew (crc_failures
      // is ~always 0) that keeps the update under the same lock take.
      if (servedOnce_.npages() == 0)
        servedOnce_.reset(meta_.npages());
      if (!servedOnce_.get(i)) {
        servedOnce_.set(i);
        ftB += cEnd - cStart;
      }
      const BufPage* staged = nullptr;
      if (auto it = buf_.find(i); it != buf_.end())
        staged = &it->second;
      else if (auto it2 = flushing_.find(i); it2 != flushing_.end())
        staged = &it2->second;
      if (staged) {
        std::memcpy(out + (cStart - off), staged->data.get() + (cStart - pStart),
                    cEnd - cStart);
        ramB += cEnd - cStart;
        continue;
      }
      if (!meta_.bitmap.get(i)) {
        publish();
        return false;
      }
      expect = meta_.pageCrcs[i];
    }
    if (!readVerifyPage(i, expect, scratch.data())) {
      publish();
      return false;
    }
    const uint32_t rLen = meta_.pageBytes(i);
    const uint64_t prevEnd = lastDiskEnd_.exchange(pStart + rLen, std::memory_order_relaxed);
    ++dReads;
    dBytes += rLen;
    if (pStart == prevEnd)
      ++dSeq;
    std::memcpy(out + (cStart - off), scratch.data() + (cStart - pStart), cEnd - cStart);
  }
  publish();
  touchAtime();
  stats_.hitBytes.fetch_add(len, std::memory_order_relaxed);
  obs_.servedBytes.fetch_add(len, std::memory_order_relaxed);
  return true;
}

void FileEntry::writePages(uint64_t off, uint64_t len, const void* buf) {
  if (cfg_.fillBufferMb <= 0) {
    writePagesDirect(off, len, buf);
    return;
  }
  if (len == 0)
    return;
  const uint32_t P = meta_.pageSize;
  const auto* in = static_cast<const uint8_t*>(buf);
  uint64_t end = std::min(off + len, meta_.fileSize);
  if (off >= end)
    return;

  // Stage full pages in RAM: no disk IO on this path at all.
  uint64_t staged = 0;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (uint64_t i = (off + P - 1) / P; i * P < end; ++i) {
      uint64_t pStart = i * P;
      uint32_t nbytes = meta_.pageBytes(i);
      if (pStart + nbytes > end)
        break; // partial edge page: leave unstaged
      if (meta_.bitmap.get(i) || buf_.count(i) || flushing_.count(i))
        continue; // already present or staged; writes are idempotent
      BufPage p;
      p.data = std::make_unique<uint8_t[]>(nbytes);
      std::memcpy(p.data.get(), in + (pStart - off), nbytes);
      p.crc = crc32c(p.data.get(), nbytes);
      buf_.emplace(i, std::move(p));
      bufBytes_ += nbytes;
      staged += nbytes;
    }
  }
  if (staged == 0)
    return;
  g_bufTotal_.fetch_add(staged, std::memory_order_relaxed);
  obs_.wireBytes.fetch_add(staged, std::memory_order_relaxed);
  touchAtime(); // an entry being written is in use — keep it LRU-fresh
  flushBuffer(false); // cap/interval policy decides; usually a no-op
}

void FileEntry::writePagesDirect(uint64_t off, uint64_t len, const void* buf) {
  if (len == 0)
    return;
  const uint32_t P = meta_.pageSize;
  const auto* in = static_cast<const uint8_t*>(buf);
  uint64_t end = std::min(off + len, meta_.fileSize);
  if (off >= end)
    return;

  // First page fully contained in [off, end) — tail page needs only its
  // real bytes (persist full pages).
  uint64_t i = (off + P - 1) / P;
  uint64_t written = 0;
  std::vector<std::pair<uint64_t, uint32_t>> done; // (page, crc)
  for (; i * P < end; ++i) {
    uint64_t pStart = i * P;
    uint32_t nbytes = meta_.pageBytes(i);
    if (pStart + nbytes > end)
      break; // partial edge page: leave unmarked
    {
      std::lock_guard<std::mutex> g(mu_);
      if (meta_.bitmap.get(i))
        continue; // already present; writes are idempotent, skip the IO
    }
    int64_t w = io_.pwriteFull(dataFd_, in + (pStart - off), nbytes, pStart);
    if (w != static_cast<int64_t>(nbytes)) {
      stats_.failopenEvents.fetch_add(1, std::memory_order_relaxed);
      UCACHE_WARN("page write failed (%lld) for %s; degrading to pass-through",
                  static_cast<long long>(w), key_.key.c_str());
      break; // ENOSPC etc. likely persists — stop persisting this span
    }
    done.emplace_back(i, crc32c(in + (pStart - off), nbytes));
    written += nbytes;
  }
  if (done.empty())
    return;
  // Data before bit: sync payload per policy, then publish bits.
  if (cfg_.fsync != FsyncMode::kOff) {
    if (io_.fdatasync(dataFd_) < 0) {
      stats_.failopenEvents.fetch_add(1, std::memory_order_relaxed);
      return; // bits never published; pages will be refetched
    }
  }
  bool complete = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto [pg, crc] : done) {
      meta_.bitmap.set(pg);
      meta_.pageCrcs[pg] = crc;
    }
    dirty_ = true;
    complete = meta_.bitmap.count() == meta_.npages();
    if (complete)
      meta_.flags |= MetaData::kFlagComplete;
  }
  stats_.pageWrites.fetch_add(done.size(), std::memory_order_relaxed);
  obs_.wireBytes.fetch_add(written, std::memory_order_relaxed);
  touchAtime(); // an entry being written is in use — keep it LRU-fresh so a
                // long cold fill isn't evicted from under itself.
  flushMeta(false);
  if (onPersist_)
    onPersist_(written, true); // usage accounting + rate-limited eviction
}

void FileEntry::flushBuffer(bool force) {
  uint64_t snapBytes = 0;
  bool capStall = false; // a fill thread draining synchronously at cap
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (force)
      flushCv_.wait(lk, [&] { return !flushInProgress_; });
    else if (flushInProgress_)
      return; // staged pages ride the next trigger
    if (buf_.empty())
      return;
    if (!force) {
      const uint64_t capB = uint64_t(cfg_.fillBufferMb) << 20;
      const uint64_t totalCapB = uint64_t(cfg_.fillBufferTotalMb) << 20;
      capStall = bufBytes_ >= capB ||
                 g_bufTotal_.load(std::memory_order_relaxed) >= totalCapB;
      if (!capStall && nowS() < lastBufFlushS_ + static_cast<uint64_t>(cfg_.metaFlushSeconds))
        return;
    }
    flushing_ = std::move(buf_);
    buf_.clear();
    snapBytes = bufBytes_;
    bufBytes_ = 0;
    flushInProgress_ = true;
  }
  const uint64_t stallT0 = capStall ? nowUsSteady() : 0;

  // Offset-sorted coalesced writes: walk the ordered snapshot, merge
  // consecutive pages into runs, ONE pwrite per run (≤ 8 MiB each).
  const uint32_t P = meta_.pageSize;
  constexpr uint64_t kMaxRun = 8ull << 20;
  std::vector<uint8_t> run;
  std::vector<std::pair<uint64_t, uint32_t>> published; // (page, crc)
  uint64_t written = 0;
  bool ioFailed = false;

  for (auto it = flushing_.begin(); it != flushing_.end() && !ioFailed;) {
    const uint64_t firstPage = it->first;
    run.clear();
    std::vector<std::pair<uint64_t, uint32_t>> runPages;
    uint64_t next = firstPage;
    while (it != flushing_.end() && it->first == next && run.size() < kMaxRun) {
      const uint32_t nbytes = meta_.pageBytes(it->first);
      run.insert(run.end(), it->second.data.get(), it->second.data.get() + nbytes);
      runPages.emplace_back(it->first, it->second.crc);
      ++next;
      ++it;
    }
    const uint64_t wT0 = nowUsSteady();
    int64_t w = io_.pwriteFull(dataFd_, run.data(), run.size(),
                               firstPage * static_cast<uint64_t>(P));
    if (w != static_cast<int64_t>(run.size())) {
      stats_.failopenEvents.fetch_add(1, std::memory_order_relaxed);
      UCACHE_WARN("buffered flush write failed (%lld) for %s; dropping staged pages",
                  static_cast<long long>(w), key_.key.c_str());
      ioFailed = true; // ENOSPC etc. likely persists — drop the rest, refetchable
      break;
    }
    const uint64_t wUs = nowUsSteady() - wT0;
    stats_.flushRuns.fetch_add(1, std::memory_order_relaxed);
    stats_.flushRunBytes.fetch_add(run.size(), std::memory_order_relaxed);
    stats_.flushWriteUs.add(wUs);
    if (stats_.tracer)
      stats_.tracer->rec("flush", key_.key, firstPage * static_cast<uint64_t>(P), run.size(),
                         wUs, /*sampled=*/false);
    for (auto& pc : runPages)
      published.push_back(pc);
    written += run.size();
  }

  // Data before bit: sync payload per policy, then publish bits.
  if (!published.empty() && cfg_.fsync != FsyncMode::kOff) {
    if (io_.fdatasync(dataFd_) < 0) {
      stats_.failopenEvents.fetch_add(1, std::memory_order_relaxed);
      published.clear(); // bits never published; pages will be refetched
      written = 0;
    }
  }

  bool anyPublished = !published.empty();
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto [pg, crc] : published) {
      meta_.bitmap.set(pg);
      meta_.pageCrcs[pg] = crc;
    }
    if (anyPublished) {
      dirty_ = true;
      if (meta_.bitmap.count() == meta_.npages())
        meta_.flags |= MetaData::kFlagComplete;
    }
    flushing_.clear(); // staged payloads freed under the same lock readers use
    flushInProgress_ = false;
    lastBufFlushS_ = nowS();
  }
  flushCv_.notify_all();
  g_bufTotal_.fetch_sub(snapBytes, std::memory_order_relaxed);
  if (anyPublished)
    stats_.pageWrites.fetch_add(published.size(), std::memory_order_relaxed);
  if (capStall) {
    // The whole cap-triggered drain ran in a fill thread's context — this is
    // the wall time the analysis actually lost to the cache disk.
    stats_.bufferStalls.fetch_add(1, std::memory_order_relaxed);
    stats_.bufferStallUs.fetch_add(nowUsSteady() - stallT0, std::memory_order_relaxed);
  }
  flushMeta(force);
  if (written && onPersist_)
    onPersist_(written, !closing_); // count always; never evict from the dtor
}

bool FileEntry::beginFetch(uint64_t off, uint64_t len, std::function<void()> onOwnerDone) {
  std::lock_guard<std::mutex> g(mu_);
  auto k = std::make_pair(off, len);
  auto it = inflight_.find(k);
  if (it == inflight_.end()) {
    inflight_.emplace(k, std::vector<std::function<void()>>{});
    return true; // caller owns the wire fetch
  }
  it->second.push_back(std::move(onOwnerDone));
  stats_.fetchesJoined.fetch_add(1, std::memory_order_relaxed);
  return false; // parked behind the owner
}

void FileEntry::endFetch(uint64_t off, uint64_t len) {
  std::vector<std::function<void()>> cbs;
  {
    std::lock_guard<std::mutex> g(mu_);
    auto it = inflight_.find({off, len});
    if (it == inflight_.end())
      return;
    cbs = std::move(it->second);
    inflight_.erase(it);
  }
  for (auto& cb : cbs)
    cb(); // callbacks re-dispatch on their own executor — cheap here
}

void FileEntry::flushAll() {
  flushBuffer(true);
  flushMeta(true);
}

void FileEntry::touchAtime() {
  uint64_t now = nowS();
  std::lock_guard<std::mutex> g(mu_);
  if (now >= meta_.atime + 60) { // coarse: at most one update per minute
    meta_.atime = now;
    dirty_ = true;
  }
}

void FileEntry::flushMeta(bool force) {
  MetaData snapshot;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (!dirty_)
      return;
    uint64_t now = nowS();
    if (!force && now < lastFlushS_ + static_cast<uint64_t>(cfg_.metaFlushSeconds))
      return;
    snapshot = meta_; // value copy under lock; store happens outside
    dirty_ = false;
    lastFlushS_ = now;
  }
  // Evicted-while-open: never resurrect the sidecar (see FORMAT.md).
  struct ::stat st;
  if (io_.fstat(dataFd_, &st) == 0 && st.st_nlink == 0)
    return;
  const uint64_t mT0 = nowUsSteady();
  io_.flock(dataFd_, LOCK_EX);
  int rc = MetaFile::store(io_, metaPath_, snapshot, cfg_.fsync == FsyncMode::kAll);
  io_.flock(dataFd_, LOCK_UN);
  stats_.metaFlushUs.add(nowUsSteady() - mT0);
  if (stats_.tracer)
    stats_.tracer->rec("meta", key_.key, 0, 0, nowUsSteady() - mT0, /*sampled=*/false);
  if (rc < 0) {
    stats_.failopenEvents.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> g(mu_);
    dirty_ = true; // retry on a later flush
  }
}

bool FileEntry::pinned() {
  std::lock_guard<std::mutex> g(mu_);
  return meta_.flags & MetaData::kFlagPinned;
}

void FileEntry::setPinned(bool p) {
  {
    std::lock_guard<std::mutex> g(mu_);
    if (p)
      meta_.flags |= MetaData::kFlagPinned;
    else
      meta_.flags &= ~MetaData::kFlagPinned;
    dirty_ = true;
  }
  flushMeta(true);
}

uint64_t FileEntry::cachedBytes() {
  std::lock_guard<std::mutex> g(mu_);
  return meta_.cachedBytes();
}

uint64_t FileEntry::releaseRanges(const std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
  const uint32_t P = meta_.pageSize;
  // Aligned inner span of each range: only pages FULLY inside are released —
  // an edge page may still serve bytes outside the superseded range.
  std::vector<std::pair<uint64_t, uint64_t>> spans; // (byteOff, byteLen)
  std::vector<uint64_t> resident; // formerly-cached ON-DISK bytes per span
  bool any = false;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (const auto& [off, len] : ranges) {
      if (len == 0 || off + len < off || off + len > meta_.fileSize)
        continue;
      uint64_t first = (off + P - 1) / P;          // first fully-covered page
      uint64_t lastEnd = (off + len) / P;          // one past the last one
      if (first >= lastEnd)
        continue;
      uint64_t res = 0;
      for (uint64_t i = first; i < lastEnd; ++i) {
        // Staged-but-unflushed pages in the released range are dropped too
        // (a page mid-flush in flushing_ republishes and is then simply a
        // cached page again — punch is reclaim, not correctness).
        if (auto bit = buf_.find(i); bit != buf_.end()) {
          uint64_t nb = meta_.pageBytes(i);
          bufBytes_ -= nb;
          g_bufTotal_.fetch_sub(nb, std::memory_order_relaxed);
          buf_.erase(bit);
        }
        if (!meta_.bitmap.get(i))
          continue;
        meta_.bitmap.clear(i);
        meta_.pageCrcs[i] = 0;
        res += meta_.pageBytes(i);
        any = true;
      }
      meta_.flags &= ~MetaData::kFlagComplete;
      spans.emplace_back(first * uint64_t(P), (lastEnd - first) * uint64_t(P));
      resident.push_back(res);
    }
    if (any)
      dirty_ = true;
  }
  if (spans.empty())
    return 0;
  // Bits durable BEFORE the bytes vanish (D1 ordering): a reader that loads
  // the flushed sidecar refetches from origin; one already holding the old
  // in-memory bitmap is protected by per-page CRC (punched zeros fail it).
  flushMeta(true);
  uint64_t punched = 0;
  for (size_t s = 0; s < spans.size(); ++s) {
    const auto [off, len] = spans[s];
    int rc = io_.punchHole(dataFd_, off, len);
    if (rc == 0)
      punched += resident[s]; // report bytes that were actually on disk, not span length
    else if (rc != -EOPNOTSUPP && rc != -ENOSYS)
      UCACHE_WARN("punchHole(%llu,%llu) failed (%d) for %s",
                  static_cast<unsigned long long>(off), static_cast<unsigned long long>(len),
                  rc, key_.key.c_str());
  }
  return punched;
}

FileEntry::ScrubResult FileEntry::verifyAll() {
  ScrubResult res;
  std::vector<uint8_t> scratch(meta_.pageSize);
  for (uint64_t i = 0; i < meta_.npages(); ++i) {
    uint32_t expect;
    {
      std::lock_guard<std::mutex> g(mu_);
      if (!meta_.bitmap.get(i))
        continue;
      expect = meta_.pageCrcs[i];
    }
    ++res.checked;
    if (!readVerifyPage(i, expect, scratch.data()))
      ++res.bad;
  }
  if (res.bad)
    flushMeta(true);
  return res;
}

} // namespace ucache
