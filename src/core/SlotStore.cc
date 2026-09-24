#include "SlotStore.h"

#include "vendor/crc32c.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <sys/file.h>
#include <unistd.h>

namespace ucache {

namespace {

constexpr char kMagic[8] = {'U', 'C', 'S', 'L', 'O', 'T', 'S', '1'};
constexpr char kBlockMagic[8] = {'U', 'S', 'B', 'L', 'O', 'C', 'K', '1'};
constexpr uint64_t kBlockHeader = 64;
constexpr uint64_t kEntry = 24;
constexpr size_t kCodecsMax = 256;
// A header that cannot be read and has not changed for this long is debris of
// a crash during creation, not a creation in progress.
constexpr int64_t kDebrisAgeS = 60;

template <typename T> void put(uint8_t* p, size_t off, T v) { std::memcpy(p + off, &v, sizeof v); }
template <typename T> T get(const uint8_t* p, size_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

uint64_t alignUp(uint64_t v) { return (v + SlotStore::kAlign - 1) / SlotStore::kAlign * SlotStore::kAlign; }

std::atomic<uint64_t> g_tmpSeq{0};

uint64_t newStoreId() {
  std::random_device rd;
  uint64_t id = (static_cast<uint64_t>(rd()) << 32) ^ rd();
  id ^= static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  id ^= static_cast<uint64_t>(::getpid()) << 17;
  return id ? id : 1;
}

void encodeEntry(const SlotEntry& e, uint8_t* p) {
  std::memset(p, 0, kEntry);
  put<uint32_t>(p, 0, e.slot);
  p[4] = e.kind;
  put<uint64_t>(p, 8, e.off);
  put<uint32_t>(p, 16, e.len);
  put<uint32_t>(p, 20, e.crc);
}

bool decodeEntry(const uint8_t* p, SlotEntry& e) {
  e.slot = get<uint32_t>(p, 0);
  e.kind = p[4];
  e.off = get<uint64_t>(p, 8);
  e.len = get<uint32_t>(p, 16);
  e.crc = get<uint32_t>(p, 20);
  return e.kind == SlotEntry::kZstd || e.kind == SlotEntry::kRaw || e.kind == SlotEntry::kKept;
}

} // namespace

std::vector<uint8_t> encodeSlotHeader(const SlotStoreHeader& h) {
  std::vector<uint8_t> b(SlotStore::kHeaderBytes, 0);
  std::memcpy(b.data(), kMagic, 8);
  put<uint32_t>(b.data(), 8, SlotStoreHeader::kFormatVersion);
  put<uint32_t>(b.data(), 12, h.layoutVersion);
  b[16] = h.container;
  b[17] = h.slotFactor;
  b[18] = h.declined ? 1 : 0;
  put<uint32_t>(b.data(), 20, h.nSlots);
  put<uint64_t>(b.data(), 24, h.originSize);
  put<uint64_t>(b.data(), 32, h.originMtime);
  b[40] = h.cksumKind;
  put<uint32_t>(b.data(), 44, h.originCksum);
  put<uint64_t>(b.data(), 48, h.virtualSize);
  put<uint64_t>(b.data(), 56, h.layoutHash);
  put<uint64_t>(b.data(), 64, h.storeId);
  put<uint64_t>(b.data(), 72, h.blobLen);
  put<uint32_t>(b.data(), 80, h.blobCrc);
  const size_t n = std::min(h.codecs.size(), kCodecsMax);
  put<uint16_t>(b.data(), 84, static_cast<uint16_t>(n));
  std::memcpy(b.data() + 86, h.codecs.data(), n);
  put<uint32_t>(b.data(), SlotStore::kHeaderBytes - 4, crc32c(b.data(), SlotStore::kHeaderBytes - 4));
  return b;
}

bool decodeSlotHeader(const uint8_t* p, size_t n, SlotStoreHeader& h) {
  if (n < SlotStore::kHeaderBytes || std::memcmp(p, kMagic, 8) != 0)
    return false;
  if (get<uint32_t>(p, SlotStore::kHeaderBytes - 4) != crc32c(p, SlotStore::kHeaderBytes - 4))
    return false;
  if (get<uint32_t>(p, 8) != SlotStoreHeader::kFormatVersion)
    return false;
  h.layoutVersion = get<uint32_t>(p, 12);
  h.container = p[16];
  h.slotFactor = p[17];
  h.declined = p[18] != 0;
  h.nSlots = get<uint32_t>(p, 20);
  h.originSize = get<uint64_t>(p, 24);
  h.originMtime = get<uint64_t>(p, 32);
  h.cksumKind = p[40];
  h.originCksum = get<uint32_t>(p, 44);
  h.virtualSize = get<uint64_t>(p, 48);
  h.layoutHash = get<uint64_t>(p, 56);
  h.storeId = get<uint64_t>(p, 64);
  h.blobLen = get<uint64_t>(p, 72);
  h.blobCrc = get<uint32_t>(p, 80);
  const uint16_t cn = get<uint16_t>(p, 84);
  if (cn > kCodecsMax)
    return false;
  h.codecs.assign(reinterpret_cast<const char*>(p + 86), cn);
  return true;
}

uint32_t SlotStore::recordCrc(uint64_t storeId, uint32_t slot, const uint8_t* p, size_t n) {
  uint8_t seed[16];
  put<uint64_t>(seed, 0, storeId);
  put<uint32_t>(seed, 8, slot);
  put<uint32_t>(seed, 12, static_cast<uint32_t>(n));
  return crc32c(crc32c(0, seed, sizeof seed), p, n);
}

std::string SlotStore::path(const std::string& objectDir, const std::string& hashHex) {
  return objectDir + "/" + hashHex + ".slots";
}

SlotStore::SlotStore(IOBackend& io, int fd, std::string path, SlotStoreHeader hdr,
                     std::vector<uint8_t> blob)
    : io_(io), fd_(fd), path_(std::move(path)), hdr_(std::move(hdr)), blob_(std::move(blob)) {
  readOff_ = alignUp(kHeaderBytes + hdr_.blobLen);
}

SlotStore::~SlotStore() {
  if (fd_ >= 0)
    io_.close(fd_);
}

namespace {

enum class Read { kOk, kAbsent, kUnusable };

// Header and layout blob of an open store.
Read readHeadAndBlob(IOBackend& io, int fd, SlotStoreHeader& h, std::vector<uint8_t>& blob) {
  std::vector<uint8_t> b(SlotStore::kHeaderBytes);
  if (io.preadFull(fd, b.data(), b.size(), 0) != static_cast<int64_t>(b.size()) ||
      !decodeSlotHeader(b.data(), b.size(), h))
    return Read::kUnusable;
  blob.resize(h.blobLen);
  if (h.blobLen &&
      (io.preadFull(fd, blob.data(), blob.size(), SlotStore::kHeaderBytes) !=
           static_cast<int64_t>(blob.size()) ||
       crc32c(blob.data(), blob.size()) != h.blobCrc))
    return Read::kUnusable;
  return Read::kOk;
}

bool isDebris(IOBackend& io, int fd) {
  struct ::stat st;
  if (io.fstat(fd, &st) != 0)
    return false;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const int64_t nowS = std::chrono::duration_cast<std::chrono::seconds>(now).count();
  return nowS - static_cast<int64_t>(statMtime(st).tv_sec) > kDebrisAgeS;
}

} // namespace

std::shared_ptr<SlotStore> SlotStore::open(IOBackend& io, const std::string& objectDir,
                                           const std::string& hashHex) {
  const std::string p = path(objectDir, hashHex);
  int fd = io.open(p, O_RDWR | O_CLOEXEC, 0);
  if (fd < 0)
    return nullptr;
  SlotStoreHeader h;
  std::vector<uint8_t> blob;
  if (readHeadAndBlob(io, fd, h, blob) != Read::kOk) {
    io.close(fd);
    return nullptr;
  }
  auto s = std::shared_ptr<SlotStore>(new SlotStore(io, fd, p, std::move(h), std::move(blob)));
  return s;
}

std::shared_ptr<SlotStore> SlotStore::openOrCreate(IOBackend& io, const std::string& objectDir,
                                                   const std::string& hashHex, SlotStoreHeader want,
                                                   const std::vector<uint8_t>& blob, bool& created,
                                                   std::string& err) {
  created = false;
  if (int rc = io.mkdirs(objectDir, 0700); rc < 0) {
    err = std::string("cannot create ") + objectDir + ": " + std::strerror(-rc);
    return nullptr;
  }
  const std::string p = path(objectDir, hashHex);
  for (int attempt = 0; attempt < 3; ++attempt) {
    int fd = io.open(p, O_RDWR | O_CLOEXEC, 0);
    if (fd >= 0) {
      SlotStoreHeader h;
      std::vector<uint8_t> b;
      if (readHeadAndBlob(io, fd, h, b) == Read::kOk)
        return std::shared_ptr<SlotStore>(new SlotStore(io, fd, p, std::move(h), std::move(b)));
      const bool debris = isDebris(io, fd);
      io.close(fd);
      if (!debris) {
        err = "store being written by another process";
        return nullptr;
      }
      io.unlink(p); // a creation that died, or a format this build cannot read
      continue;
    }
    if (fd != -ENOENT) {
      err = std::string("cannot open ") + p + ": " + std::strerror(-fd);
      return nullptr;
    }
    // Create: header and layout in a private file, then link() it into place
    // -- which fails if someone else got there first, so the store never
    // exists without its header and layout.
    want.storeId = newStoreId();
    want.blobLen = blob.size();
    want.blobCrc = crc32c(blob.data(), blob.size());
    const std::string tmp = p + ".tmp." + std::to_string(::getpid()) + "." +
                            std::to_string(g_tmpSeq.fetch_add(1));
    int tfd = io.open(tmp, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (tfd < 0) {
      err = std::string("cannot create ") + tmp + ": " + std::strerror(-tfd);
      return nullptr;
    }
    const auto hb = encodeSlotHeader(want);
    bool ok = io.pwriteFull(tfd, hb.data(), hb.size(), 0) == static_cast<int64_t>(hb.size());
    if (ok && !blob.empty())
      ok = io.pwriteFull(tfd, blob.data(), blob.size(), kHeaderBytes) ==
           static_cast<int64_t>(blob.size());
    if (!ok || io.fdatasync(tfd) != 0) {
      io.close(tfd);
      io.unlink(tmp);
      err = "cannot write the store's header and layout";
      return nullptr;
    }
    const int lrc = io.link(tmp, p);
    io.unlink(tmp);
    if (lrc == -EEXIST) {
      io.close(tfd);
      continue; // someone else created it: use theirs
    }
    if (lrc < 0) {
      io.close(tfd);
      err = std::string("cannot link ") + p + ": " + std::strerror(-lrc);
      return nullptr;
    }
    created = true;
    return std::shared_ptr<SlotStore>(new SlotStore(io, tfd, p, want, blob));
  }
  err = "store creation raced repeatedly";
  return nullptr;
}

bool SlotStore::serving(IOBackend& io, const std::string& objectDir, const std::string& hashHex) {
  int fd = io.open(path(objectDir, hashHex), O_RDONLY | O_CLOEXEC, 0);
  if (fd < 0)
    return false;
  std::vector<uint8_t> b(kHeaderBytes);
  SlotStoreHeader h;
  const bool ok = io.preadFull(fd, b.data(), b.size(), 0) == static_cast<int64_t>(b.size()) &&
                  decodeSlotHeader(b.data(), b.size(), h) && !h.declined;
  io.close(fd);
  return ok;
}

void SlotStore::drop(IOBackend& io, const std::string& objectDir, const std::string& hashHex) {
  io.unlink(path(objectDir, hashHex));
}

bool SlotStore::dropIfCurrent() {
  std::unique_lock<std::mutex> g(mu_, std::try_to_lock);
  if (!g.owns_lock() || io_.flock(fd_, LOCK_EX | LOCK_NB) != 0)
    return false;
  // A commit waiting for the lock sees the file unlinked once it has it.
  struct ::stat st, sp;
  const bool same = io_.fstat(fd_, &st) == 0 && st.st_nlink > 0 && io_.stat(path_, &sp) == 0 &&
                    st.st_ino == sp.st_ino && st.st_dev == sp.st_dev;
  if (same)
    io_.unlink(path_);
  io_.flock(fd_, LOCK_UN);
  return same;
}

std::vector<SlotEntry> SlotStore::readBlocks(uint64_t end) {
  std::vector<SlotEntry> out;
  uint64_t pos = readOff_;
  uint8_t h[kBlockHeader];
  while (pos + kBlockHeader <= end) {
    const bool okHdr = io_.preadFull(fd_, h, kBlockHeader, pos) == static_cast<int64_t>(kBlockHeader) &&
                       std::memcmp(h, kBlockMagic, 8) == 0 &&
                       get<uint64_t>(h, 8) == hdr_.storeId &&
                       get<uint32_t>(h, 60) == crc32c(h, 60);
    const uint64_t blockLen = okHdr ? get<uint64_t>(h, 16) : 0;
    const uint32_t n = okHdr ? get<uint32_t>(h, 24) : 0;
    if (okHdr && blockLen >= kBlockHeader && blockLen < (1ull << 40))
      claimedEnd_ = std::max(claimedEnd_, pos + blockLen);
    if (!okHdr || blockLen < kBlockHeader + static_cast<uint64_t>(n) * kEntry ||
        pos + blockLen > end) {
      // Not a block: debris of a commit that died (we hold a lock no writer
      // holds, so nothing is being written). The next block starts on a later
      // boundary.
      pos += kAlign;
      continue;
    }
    std::vector<uint8_t> ents(static_cast<size_t>(n) * kEntry);
    if (n && (io_.preadFull(fd_, ents.data(), ents.size(), pos + kBlockHeader) !=
                  static_cast<int64_t>(ents.size()) ||
              crc32c(ents.data(), ents.size()) != get<uint32_t>(h, 28))) {
      pos += kAlign;
      continue;
    }
    for (uint32_t i = 0; i < n; ++i) {
      SlotEntry e;
      if (decodeEntry(ents.data() + i * kEntry, e) &&
          (e.kind == SlotEntry::kKept || (e.off >= pos && e.off + e.len <= pos + blockLen)))
        out.push_back(e);
    }
    pos = alignUp(pos + blockLen);
  }
  readOff_ = std::max(readOff_, pos);
  return out;
}

std::vector<SlotEntry> SlotStore::refresh(bool wait) {
  // Without `wait`, never queue behind this process's own commit either: it
  // holds mu_ while it waits for the file lock another process may hold.
  std::unique_lock<std::mutex> g(mu_, std::defer_lock);
  if (wait)
    g.lock();
  else if (!g.try_lock())
    return {};
  struct ::stat st;
  if (io_.fstat(fd_, &st) != 0 || static_cast<uint64_t>(st.st_size) < readOff_ + kBlockHeader)
    return {};
  if (io_.flock(fd_, wait ? LOCK_SH : (LOCK_SH | LOCK_NB)) != 0)
    return {}; // a commit is running: its entries come next time
  std::vector<SlotEntry> out;
  if (io_.fstat(fd_, &st) == 0)
    out = readBlocks(static_cast<uint64_t>(st.st_size));
  io_.flock(fd_, LOCK_UN);
  return out;
}

int64_t SlotStore::commit(std::vector<SlotRecord>& recs,
                          const std::function<void(const std::vector<SlotEntry>&)>& onOthers,
                          const std::function<bool(uint32_t)>& taken, bool fsync,
                          std::vector<SlotEntry>& committed) {
  committed.clear();
  if (hdr_.declined)
    return -EINVAL;
  std::lock_guard<std::mutex> g(mu_);
  if (int rc = io_.flock(fd_, LOCK_EX); rc != 0)
    return rc;
  struct Unlock {
    IOBackend& io;
    int fd;
    ~Unlock() { io.flock(fd, LOCK_UN); }
  } unlock{io_, fd_};

  // The path must still name this file: a dropped or replaced store takes no
  // more records.
  struct ::stat st, sp;
  if (io_.fstat(fd_, &st) != 0 || st.st_nlink == 0 || io_.stat(path_, &sp) != 0 ||
      st.st_ino != sp.st_ino || st.st_dev != sp.st_dev)
    return -ESTALE;
  const auto others = readBlocks(static_cast<uint64_t>(st.st_size));
  if (onOthers && !others.empty())
    onOthers(others);

  std::vector<const SlotRecord*> write;
  uint64_t dataLen = 0;
  for (const auto& r : recs)
    if (!taken || !taken(r.slot)) {
      write.push_back(&r);
      dataLen += r.bytes.size();
    }
  if (write.empty())
    return 0;

  // Past everything anyone has written or claimed: the file's end, the blocks
  // read, and the extent of any block cut short (a crash without fsync can
  // leave a header whose records never landed).
  const uint64_t base = alignUp(
      std::max({static_cast<uint64_t>(st.st_size), readOff_, claimedEnd_}));
  const uint32_t n = static_cast<uint32_t>(write.size());
  const uint64_t blockLen = kBlockHeader + n * kEntry + dataLen;
  std::vector<uint8_t> blk(blockLen, 0);
  uint64_t at = kBlockHeader + n * kEntry;
  for (uint32_t i = 0; i < n; ++i) {
    const SlotRecord& r = *write[i];
    SlotEntry e;
    e.slot = r.slot;
    e.kind = r.kind;
    if (r.kind != SlotEntry::kKept) {
      e.off = base + at;
      e.len = static_cast<uint32_t>(r.bytes.size());
      e.crc = recordCrc(hdr_.storeId, r.slot, r.bytes.data(), r.bytes.size());
      std::memcpy(blk.data() + at, r.bytes.data(), r.bytes.size());
      at += r.bytes.size();
    }
    encodeEntry(e, blk.data() + kBlockHeader + i * kEntry);
    committed.push_back(e);
  }
  std::memcpy(blk.data(), kBlockMagic, 8);
  put<uint64_t>(blk.data(), 8, hdr_.storeId);
  put<uint64_t>(blk.data(), 16, blockLen);
  put<uint32_t>(blk.data(), 24, n);
  put<uint32_t>(blk.data(), 28, crc32c(blk.data() + kBlockHeader, n * kEntry));
  put<uint32_t>(blk.data(), 60, crc32c(blk.data(), 60));
  const int64_t w = io_.pwriteFull(fd_, blk.data(), blk.size(), base);
  if (w != static_cast<int64_t>(blk.size())) {
    committed.clear();
    return w < 0 ? w : -EIO; // a torn block is stepped over by every reader
  }
  if (fsync)
    io_.fdatasync(fd_);
  readOff_ = alignUp(base + blockLen);
  claimedEnd_ = std::max(claimedEnd_, base + blockLen);
  return static_cast<int64_t>(dataLen);
}

bool SlotStore::readRecord(const SlotEntry& e, std::vector<uint8_t>& out) {
  if (e.kind == SlotEntry::kKept)
    return false;
  out.resize(e.len);
  if (io_.preadFull(fd_, out.data(), e.len, e.off) != static_cast<int64_t>(e.len))
    return false;
  return recordCrc(hdr_.storeId, e.slot, out.data(), out.size()) == e.crc;
}

uint64_t SlotStore::fileBytes() {
  struct ::stat st;
  return io_.fstat(fd_, &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

} // namespace ucache
