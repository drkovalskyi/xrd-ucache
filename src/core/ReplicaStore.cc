#include "ReplicaStore.h"

#include "IOBackend.h"
#include "Log.h"
#include "vendor/crc32c.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace ucache {

// ------------------------------------------------------------- ReplicaView --

ReplicaView::~ReplicaView() {
  if (fd_ >= 0)
    io_.close(fd_);
}

std::vector<ReplicaView::Seg> ReplicaView::map(uint64_t off, uint64_t len) const {
  std::vector<Seg> out;
  if (off >= meta_.virtualSize)
    return out;
  uint64_t end = std::min(off + len, meta_.virtualSize);
  if (end <= off)
    return out;

  // First extent whose end lies past `off` (extents sorted, non-overlapping).
  auto it = std::lower_bound(meta_.extents.begin(), meta_.extents.end(), off,
                             [](const ReplicaMeta::Extent& e, uint64_t o) {
                               return e.virtOff + e.len <= o;
                             });
  uint64_t pos = off;
  while (pos < end) {
    if (it == meta_.extents.end() || it->virtOff >= end) {
      out.push_back({false, pos, end - pos, 0});
      break;
    }
    if (pos < it->virtOff) { // gap before the next overlay extent
      out.push_back({false, pos, it->virtOff - pos, 0});
      pos = it->virtOff;
    }
    uint64_t segEnd = std::min(end, it->virtOff + it->len);
    if (pos < segEnd) {
      out.push_back({true, pos, segEnd - pos, it->tdataOff + (pos - it->virtOff)});
      pos = segEnd;
    }
    ++it;
  }
  return out;
}

bool ReplicaView::read(uint64_t tdataOff, uint64_t len, void* buf) {
  if (len == 0)
    return true;
  if (tdataOff + len < tdataOff || tdataOff + len > meta_.tdataBytes) {
    invalid_.store(true, std::memory_order_relaxed);
    return false; // caller bug or corrupt map — never guess
  }
  const uint32_t P = ReplicaMeta::kOverlayPageSize;
  const uint64_t first = tdataOff / P, last = (tdataOff + len - 1) / P;
  auto* out = static_cast<uint8_t*>(buf);
  // Every overlay page is present by construction, so the run is the whole
  // requested page span, split only by the coalescing cap: ONE pread per run,
  // each page's CRC verified out of that buffer.
  std::vector<uint8_t> scratch;
  for (uint64_t rf = first; rf <= last;) {
    uint64_t rl = rf;
    while (rl < last && meta_.runBytes(rf, rl + 1) <= kMaxCoalescedRead)
      ++rl;
    const uint64_t rStart = rf * uint64_t(P);
    const uint64_t rBytes = meta_.runBytes(rf, rl);
    // Read straight into the caller's buffer when the run lies entirely
    // inside the request; otherwise via scratch, since a partially-requested
    // edge page must still be checksummed whole.
    const bool direct = rStart >= tdataOff && rStart + rBytes <= tdataOff + len;
    uint8_t* dst;
    if (direct) {
      dst = out + (rStart - tdataOff);
    } else {
      if (scratch.size() < rBytes)
        scratch.resize(rBytes);
      dst = scratch.data();
    }
    const int64_t r = io_.preadFull(fd_, dst, rBytes, rStart);
    const uint64_t got = r > 0 ? static_cast<uint64_t>(r) : 0;
    uint64_t o = 0;
    for (uint64_t i = rf; i <= rl; ++i) {
      const uint32_t nbytes = meta_.pageBytes(i);
      const bool short_ = o + nbytes > got;
      if (short_ || crc32c(dst + o, nbytes) != meta_.pageCrcs[i]) {
        stats_.replicaCrcFailures.fetch_add(1, std::memory_order_relaxed);
        invalid_.store(true, std::memory_order_relaxed);
        // Distinguish a truncated/unreadable .tdata from corrupt content —
        // they point at different causes.
        UCACHE_WARN("replica overlay page %llu %s for %s; view invalidated",
                    static_cast<unsigned long long>(i),
                    short_ ? "unreadable (short read)" : "failed its checksum",
                    meta_.key.c_str());
        return false;
      }
      o += nbytes;
    }
    // Counted after the run verifies, so a failed pread is not reported as
    // work delivered — the same convention as the byte tier's hit_disk_reads.
    stats_.replicaReads.fetch_add(1, std::memory_order_relaxed);
    stats_.replicaReadBytes.fetch_add(rBytes, std::memory_order_relaxed);
    stats_.replicaReadSize.add(rBytes);
    if (!direct) {
      const uint64_t cStart = std::max(tdataOff, rStart);
      const uint64_t cEnd = std::min(tdataOff + len, rStart + rBytes);
      std::memcpy(out + (cStart - tdataOff), dst + (cStart - rStart), cEnd - cStart);
    }
    rf = rl + 1;
  }
  return true;
}

// ------------------------------------------------------------ ReplicaStore --

std::string ReplicaStore::tdataPath(const UrlKey& key, const std::string& cacheDir) {
  return key.objectDir(cacheDir) + "/" + key.hashHex + ".tdata";
}
std::string ReplicaStore::tmetaPath(const UrlKey& key, const std::string& cacheDir) {
  return key.objectDir(cacheDir) + "/" + key.hashHex + ".tmeta";
}
std::string ReplicaStore::tokPath(const UrlKey& key, const std::string& cacheDir) {
  return key.objectDir(cacheDir) + "/" + key.hashHex + ".tok";
}

namespace {
// Advisory verify-once marker: 28 bytes, self-checksummed. Torn/stale/absent
// => just re-run the full open-time verify.
constexpr char kTokMagic[4] = {'U', 'C', 'T', 'V'};

uint64_t mtimeNs(const struct ::stat& st) {
  return static_cast<uint64_t>(statMtime(st).tv_sec) * 1000000000ull +
         static_cast<uint64_t>(statMtime(st).tv_nsec);
}

std::vector<uint8_t> tokImage(uint64_t tdataBytes, uint64_t mtNs, const ReplicaMeta& m) {
  std::vector<uint8_t> buf(28, 0);
  std::memcpy(buf.data(), kTokMagic, 4);
  std::memcpy(buf.data() + 4, &tdataBytes, 8);
  std::memcpy(buf.data() + 12, &mtNs, 8);
  uint32_t crcs = crc32c(reinterpret_cast<const uint8_t*>(m.pageCrcs.data()),
                         m.pageCrcs.size() * 4);
  std::memcpy(buf.data() + 20, &crcs, 4);
  uint32_t self = crc32c(buf.data(), 24);
  std::memcpy(buf.data() + 24, &self, 4);
  return buf;
}

void tokWrite(IOBackend& io, const std::string& path, uint64_t tdataBytes, uint64_t mtNs,
              const ReplicaMeta& m) {
  auto buf = tokImage(tdataBytes, mtNs, m);
  int fd = io.open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return; // advisory: losing it only costs a re-verify
  io.pwriteFull(fd, buf.data(), buf.size(), 0);
  io.close(fd);
}

bool tokMatches(IOBackend& io, const std::string& path, uint64_t tdataBytes, uint64_t mtNs,
                const ReplicaMeta& m) {
  int fd = io.open(path, O_RDONLY, 0);
  if (fd < 0)
    return false;
  std::vector<uint8_t> got(28);
  int64_t r = io.preadFull(fd, got.data(), got.size(), 0);
  io.close(fd);
  return r == 28 && got == tokImage(tdataBytes, mtNs, m);
}
} // namespace

int ReplicaStore::publish(const UrlKey& key, ReplicaMeta meta, const void* tdata, uint64_t n) {
  // Integrity data is computed HERE from the payload — callers cannot publish
  // a sidecar that disagrees with its overlay bytes.
  meta.key = key.key;
  meta.tdataBytes = n;
  meta.pageCrcs.assign(meta.npages(), 0);
  const auto* p = static_cast<const uint8_t*>(tdata);
  for (uint64_t i = 0; i < meta.npages(); ++i)
    meta.pageCrcs[i] = crc32c(p + i * uint64_t(ReplicaMeta::kOverlayPageSize), meta.pageBytes(i));
  // Round-trip through the deserializer's structural validation so an
  // inconsistent extent map is rejected at publish time, not at first open.
  auto img = ReplicaFile::serialize(meta);
  if (!ReplicaFile::deserialize(img.data(), img.size())) {
    UCACHE_WARN("replica publish rejected for %s: inconsistent metadata", key.key.c_str());
    return -EINVAL;
  }

  if (int rc = io_.mkdirs(key.objectDir(cfg_.cacheDir), 0700); rc < 0)
    return rc;
  const std::string dPath = tdataPath(key, cfg_.cacheDir);
  const std::string dTmp = dPath + ".tmp";
  int fd = io_.open(dTmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return fd;
  int rc = 0;
  int64_t w = io_.pwriteFull(fd, tdata, n, 0);
  if (w != static_cast<int64_t>(n))
    rc = w < 0 ? static_cast<int>(w) : -EIO;
  // The publish protocol REQUIRES the overlay durable before the sidecar
  // becomes visible (visible .tmeta => complete .tdata), independent of the
  // UCACHE_FSYNC policy for ordinary pages.
  if (rc == 0)
    rc = io_.fdatasync(fd);
  int crc_ = io_.close(fd);
  if (rc == 0 && crc_ < 0)
    rc = crc_;
  if (rc == 0)
    rc = io_.rename(dTmp, dPath);
  if (rc != 0) {
    io_.unlink(dTmp);
    return rc;
  }
  rc = ReplicaFile::store(io_, tmetaPath(key, cfg_.cacheDir), meta, /*fsyncMeta=*/true);
  if (rc != 0) {
    io_.unlink(dPath); // no orphan when we know the sidecar never landed
    return rc;
  }
  // Verify-once marker: the publisher computed the page CRCs from the bytes
  // it just wrote+synced, so the overlay is verified by construction.
  struct ::stat dst;
  if (io_.stat(dPath, &dst) == 0)
    tokWrite(io_, tokPath(key, cfg_.cacheDir), static_cast<uint64_t>(dst.st_size),
             mtimeNs(dst), meta);
  stats_.replicaPublished.fetch_add(1, std::memory_order_relaxed);
  UCACHE_INFO("replica published for %s (%llu overlay bytes, %zu extents)", key.key.c_str(),
              static_cast<unsigned long long>(n), meta.extents.size());
  return 0;
}

std::shared_ptr<ReplicaView> ReplicaStore::openView(const UrlKey& key, uint64_t originSize,
                                                    uint64_t originMtime, uint8_t cksumKind,
                                                    uint32_t originCksum) {
  const std::string mPath = tmetaPath(key, cfg_.cacheDir);
  struct ::stat st;
  if (io_.stat(mPath, &st) < 0)
    return nullptr; // no replica — the common case, silent and cheap

  auto quarantine = [&](const char* why) {
    stats_.replicaInvalid.fetch_add(1, std::memory_order_relaxed);
    UCACHE_WARN("replica for %s dropped: %s", key.key.c_str(), why);
    drop(key);
    return nullptr;
  };

  auto m = ReplicaFile::load(io_, mPath);
  if (!m)
    return quarantine("torn/corrupt sidecar");
  bool ok = m->key == key.key && m->originSize == originSize &&
            m->encoding >= ReplicaMeta::kZstd1 && m->encoding <= ReplicaMeta::kRaw;
  if (ok && cfg_.validate == ValidateMode::kSizeMtime)
    ok = m->originMtime == originMtime;
  if (ok && cfg_.validate == ValidateMode::kCksum && cksumKind != 0)
    ok = m->cksumKind == cksumKind && m->originCksum == originCksum;
  if (!ok)
    return quarantine("origin validation failed (stale replica)");

  const std::string dPath = tdataPath(key, cfg_.cacheDir);
  int fd = io_.open(dPath, O_RDONLY, 0);
  if (fd < 0)
    return quarantine(".tdata missing");
  struct ::stat dst;
  if (io_.fstat(fd, &dst) < 0 || static_cast<uint64_t>(dst.st_size) != m->tdataBytes) {
    io_.close(fd);
    return quarantine(".tdata size mismatch");
  }

  // Open-time FULL overlay verify: after this, the view
  // behaves like a pre-copied local file for the handle's lifetime. The scan
  // is skipped when the advisory verify-once marker matches (header comment);
  // per-read page CRCs stay on either way.
  if (!tokMatches(io_, tokPath(key, cfg_.cacheDir), m->tdataBytes, mtimeNs(dst), *m)) {
    // A sequential scan of the whole overlay: read it in coalesced spans and
    // verify each page's CRC out of the buffer, rather than one pread per page.
    const uint64_t npages = m->npages();
    // Sized to the overlay, not to the cap: this runs on the application's
    // thread at open, and most overlays are far smaller than 1 MiB.
    std::vector<uint8_t> scratch(std::min<uint64_t>(kMaxCoalescedRead, m->tdataBytes));
    for (uint64_t rf = 0; rf < npages;) {
      uint64_t rl = rf;
      while (rl + 1 < npages && m->runBytes(rf, rl + 1) <= kMaxCoalescedRead)
        ++rl;
      const uint64_t rBytes = m->runBytes(rf, rl);
      const int64_t r =
          io_.preadFull(fd, scratch.data(), rBytes, rf * uint64_t(ReplicaMeta::kOverlayPageSize));
      const uint64_t got = r > 0 ? static_cast<uint64_t>(r) : 0;
      uint64_t o = 0;
      for (uint64_t i = rf; i <= rl; ++i) {
        const uint32_t nbytes = m->pageBytes(i);
        if (o + nbytes > got || crc32c(scratch.data() + o, nbytes) != m->pageCrcs[i]) {
          stats_.replicaCrcFailures.fetch_add(1, std::memory_order_relaxed);
          io_.close(fd);
          return quarantine("overlay CRC failure at open");
        }
        o += nbytes;
      }
      rf = rl + 1;
    }
    tokWrite(io_, tokPath(key, cfg_.cacheDir), m->tdataBytes, mtimeNs(dst), *m);
  }

  stats_.replicaOpens.fetch_add(1, std::memory_order_relaxed);
  return std::shared_ptr<ReplicaView>(new ReplicaView(io_, stats_, std::move(*m), fd));
}

void ReplicaStore::drop(const UrlKey& key) {
  io_.unlink(tmetaPath(key, cfg_.cacheDir));
  io_.unlink(tdataPath(key, cfg_.cacheDir));
  io_.unlink(tokPath(key, cfg_.cacheDir));
  io_.unlink(tmetaPath(key, cfg_.cacheDir) + ".tmp");
  io_.unlink(tdataPath(key, cfg_.cacheDir) + ".tmp");
}

uint64_t ReplicaStore::punchSuperseded(FileEntry& entry,
                                       const std::vector<ReplicaMeta::Range>& ranges) {
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  spans.reserve(ranges.size());
  for (const auto& r : ranges)
    spans.emplace_back(r.off, r.len);
  uint64_t punched = entry.releaseRanges(spans);
  stats_.replicaPunchedBytes.fetch_add(punched, std::memory_order_relaxed);
  return punched;
}

} // namespace ucache
