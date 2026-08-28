#include "FileEntry.h"

#include "CpuCounters.h"


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
  // The span starts HERE, not at first read: opening is part of what a file
  // costs, and a file read exactly once would otherwise have no span at all.
  // Sample the process width while the run is active: at exit the pool has
  // wound down, and the peak is what the wall must be divided by.
  {
    const uint64_t n = CpuCounters::liveThreads();
    uint64_t prev = stats.threadsHighWater.load(std::memory_order_relaxed);
    while (n > prev &&
           !stats.threadsHighWater.compare_exchange_weak(prev, n, std::memory_order_relaxed)) {
    }
  }
  e->noteActivity();
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

void FileEntry::noteActivity() {
  const uint64_t t = nowUsSteady();
  uint64_t expected = 0;
  // Only the first caller sets the start; the rest just push the end forward.
  // Monotone, so a stale racing store cannot shorten the span below truth.
  obs_.firstUs.compare_exchange_strong(expected, t, std::memory_order_relaxed);
  obs_.lastUs.store(t, std::memory_order_relaxed);
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
     << ",\"wire_bytes\":" << v(obs_.wireBytes)
     << ",\"span_us\":" << spanUs()
     // The one size that means the same thing on both routes. Bytes SERVED
     // differ by route -- the replica tier hands over decompressed data, the
     // origin compressed -- so they cannot normalise a comparison between
     // routes; the file's size at the origin can.
     << ",\"origin_size\":" << meta_.fileSize
     // A file this process mostly FETCHED is a fill, whatever else it also
     // served; the distinction decides which population a measurement joins.
     << ",\"mode\":\"" << (v(obs_.wireBytes) > v(obs_.servedBytes) ? "fill" : "cached")
     << "\"}\n";
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
  // off + len can WRAP on a hostile or buggy caller; an unchecked wrap used to
  // pass the range test and then index the bitmap out of bounds.
  if (len == 0 || off + len < off || off + len > meta_.fileSize)
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

void FileEntry::demoteRun(uint64_t firstPage, uint64_t lastPage, const char* why) {
  std::lock_guard<std::mutex> g(mu_);
  for (uint64_t i = firstPage; i <= lastPage; ++i) {
    meta_.bitmap.clear(i);
    meta_.pageCrcs[i] = 0;
  }
  meta_.flags &= ~MetaData::kFlagComplete;
  dirty_ = true;
  stats_.crcFailures.fetch_add(1, std::memory_order_relaxed);
  if (firstPage == lastPage)
    UCACHE_WARN("%s on page %llu of %s; marked absent", why,
                static_cast<unsigned long long>(firstPage), key_.key.c_str());
  else
    UCACHE_WARN("%s on pages %llu-%llu of %s; marked absent", why,
                static_cast<unsigned long long>(firstPage),
                static_cast<unsigned long long>(lastPage), key_.key.c_str());
}

// A coalesced read that comes up short cannot say WHICH page is bad, and
// discarding the whole run would throw away up to a megabyte of good cache
// because of one transient error. Re-read the remainder one page at a time to
// demote exactly the pages that really are bad — the same blast radius, and the
// same per-page crc_failures accounting, as before reads were coalesced. A
// transient error that has already cleared costs nothing but the re-reads.
bool FileEntry::demoteBadPages(uint64_t firstPage, uint64_t lastPage, const uint32_t* expect) {
  std::vector<uint8_t> page(meta_.pageSize);
  for (uint64_t i = firstPage; i <= lastPage; ++i) {
    const uint32_t nbytes = meta_.pageBytes(i);
    const int64_t r =
        io_.preadFull(dataFd_, page.data(), nbytes, i * uint64_t(meta_.pageSize));
    if (r != static_cast<int64_t>(nbytes))
      demoteRun(i, i, "short read");
    else if (crc32c(page.data(), nbytes) != expect[i - firstPage])
      demoteRun(i, i, "CRC mismatch");
  }
  return false;
}

bool FileEntry::readVerifyRun(uint64_t firstPage, uint64_t lastPage, const uint32_t* expect,
                              uint8_t* dst) {
  const uint64_t start = firstPage * uint64_t(meta_.pageSize);
  const uint64_t bytes = runBytes(firstPage, lastPage);
  const int64_t r = io_.preadFull(dataFd_, dst, bytes, start);
  const uint64_t got = r > 0 ? static_cast<uint64_t>(r) : 0;
  uint64_t o = 0;
  for (uint64_t i = firstPage; i <= lastPage; ++i) {
    const uint32_t nbytes = meta_.pageBytes(i);
    if (o + nbytes > got) // short read or IO error: localize it page by page
      return demoteBadPages(i, lastPage, expect + (i - firstPage));
    if (crc32c(dst + o, nbytes) != expect[i - firstPage]) {
      demoteRun(i, i, "CRC mismatch"); // this one page is provably bad
      return false;
    }
    o += nbytes;
  }
  return true;
}

bool FileEntry::readCached(uint64_t off, uint64_t len, void* buf) {
  noteActivity();
  if (len == 0)
    return true;
  if (off + len < off || off + len > meta_.fileSize) // wrap, then range
    return false;
  const uint32_t P = meta_.pageSize;
  uint64_t first = off / P, last = (off + len - 1) / P;
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

  // Plan under ONE lock take (it used to be one per page): staged pages are
  // copied out here — a concurrent flush completion frees flushing_ payloads
  // under this same mutex — while resident disk pages accumulate into
  // maximal contiguous runs. Each run costs ONE pread below, whatever its
  // page count: a 38 KiB request used to cost ten 4 KiB preads.
  struct Run {
    uint64_t firstPage, lastPage, crcBase;
  };
  std::vector<Run> runs;
  std::vector<uint32_t> crcs;
  {
    std::lock_guard<std::mutex> g(mu_);
    if (servedOnce_.npages() == 0)
      servedOnce_.reset(meta_.npages());
    for (uint64_t i = first; i <= last; ++i) {
      const uint64_t pStart = i * P;
      const uint64_t cStart = std::max(off, pStart);
      const uint64_t cEnd = std::min(off + len, pStart + meta_.pageBytes(i));
      const BufPage* staged = nullptr;
      if (auto it = buf_.find(i); it != buf_.end())
        staged = &it->second;
      else if (auto it2 = flushing_.find(i); it2 != flushing_.end())
        staged = &it2->second;
      if (staged) {
        // Staged pages serve straight from RAM — during a fill the cache
        // disk sees no random reads. A RAM page also ends the current run.
        std::memcpy(out + (cStart - off), staged->data.get() + (cStart - pStart),
                    cEnd - cStart);
        ramB += cEnd - cStart;
        // first_touch: page-granular, at the serving read's width. Marked here
        // because these bytes ARE served, now, under this lock.
        if (!servedOnce_.get(i)) {
          servedOnce_.set(i);
          ftB += cEnd - cStart;
        }
        continue;
      }
      if (!meta_.bitmap.get(i)) {
        publish(); // absent page: no disk read was issued for this request
        return false;
      }
      if (!runs.empty() && runs.back().lastPage + 1 == i &&
          runBytes(runs.back().firstPage, i) <= kMaxCoalescedRead)
        runs.back().lastPage = i;
      else
        runs.push_back({i, i, crcs.size()});
      crcs.push_back(meta_.pageCrcs[i]);
    }
  }

  std::vector<uint8_t> scratch;
  for (const Run& r : runs) {
    const uint64_t rStart = r.firstPage * uint64_t(P);
    const uint64_t rBytes = runBytes(r.firstPage, r.lastPage);
    // A run that lies entirely inside the request is read STRAIGHT into the
    // caller's buffer — no scratch, no copy. Otherwise it goes through
    // scratch, because a partially-requested edge page must still be
    // checksummed whole. Either way the caller must treat the buffer as
    // undefined when this returns false; every caller refetches the span.
    const bool direct = rStart >= off && rStart + rBytes <= off + len;
    uint8_t* dst;
    if (direct) {
      dst = out + (rStart - off);
    } else {
      if (scratch.size() < rBytes)
        scratch.resize(rBytes);
      dst = scratch.data();
    }
    if (!readVerifyRun(r.firstPage, r.lastPage, &crcs[r.crcBase], dst)) {
      publish();
      return false;
    }
    if (!direct) {
      const uint64_t cStart = std::max(off, rStart);
      const uint64_t cEnd = std::min(off + len, rStart + rBytes);
      std::memcpy(out + (cStart - off), dst + (cStart - rStart), cEnd - cStart);
    }
    const uint64_t prevEnd = lastDiskEnd_.exchange(rStart + rBytes, std::memory_order_relaxed);
    ++dReads;
    dBytes += rBytes;
    if (rStart == prevEnd)
      ++dSeq;
    stats_.hitReadSize.add(rBytes);
  }

  // first_touch for the disk pages, once every run has verified: a request
  // that failed served nothing, so it must not consume the attribution (the
  // refetch that follows will). One lock take for the whole request.
  if (!runs.empty()) {
    std::lock_guard<std::mutex> g(mu_);
    for (const Run& r : runs)
      for (uint64_t i = r.firstPage; i <= r.lastPage; ++i)
        if (!servedOnce_.get(i)) {
          servedOnce_.set(i);
          const uint64_t pStart = i * P;
          ftB += std::min(off + len, pStart + meta_.pageBytes(i)) - std::max(off, pStart);
        }
  }
  publish();
  touchAtime();
  stats_.hitBytes.fetch_add(len, std::memory_order_relaxed);
  obs_.servedBytes.fetch_add(len, std::memory_order_relaxed);
  return true;
}

void FileEntry::writePages(uint64_t off, uint64_t len, const void* buf) {
  noteActivity();
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
  noteActivity();
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
    // One page at a time here on purpose: the scrub reports how many pages
    // are bad, and a coalesced run would demote all of them together.
    if (!readVerifyRun(i, i, &expect, scratch.data()))
      ++res.bad;
  }
  if (res.bad)
    flushMeta(true);
  return res;
}

} // namespace ucache
