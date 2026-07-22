#include "MetaFile.h"

#include "Log.h"
#include "vendor/crc32c.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>

namespace ucache {
namespace {

constexpr char kMagic[4] = {'U', 'C', 'A', 'C'};
constexpr size_t kHeaderSize = 56;
constexpr size_t kCrcFieldOff = 52;

template <typename T> void put(std::vector<uint8_t>& b, size_t off, T v) {
  // little-endian store (x86_64/aarch64 hosts)
  std::memcpy(b.data() + off, &v, sizeof v);
}
template <typename T> T get(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof v);
  return v;
}

size_t align8(size_t n) { return (n + 7) & ~size_t(7); }

} // namespace

uint64_t MetaData::cachedBytes() const {
  uint64_t n = bitmap.count();
  if (n == 0)
    return 0;
  uint64_t bytes = n * pageSize;
  // Tail page counts only its real bytes.
  uint64_t last = npages() - 1;
  if (bitmap.get(last))
    bytes -= pageSize - pageBytes(last);
  return bytes;
}

MetaData MetaData::fresh(const std::string& key, uint64_t fileSize, uint32_t pageSize) {
  MetaData m;
  m.pageSize = pageSize;
  m.fileSize = fileSize;
  m.atime = static_cast<uint64_t>(::time(nullptr));
  m.key = key;
  m.bitmap.reset(m.npages());
  m.pageCrcs.assign(m.npages(), 0);
  return m;
}

std::vector<uint8_t> MetaFile::serialize(const MetaData& m) {
  const uint64_t npages = m.npages();
  const size_t keyOff = kHeaderSize;
  const size_t bitmapOff = align8(keyOff + m.key.size());
  const size_t crcsOff = bitmapOff + m.bitmap.rawSize();
  const size_t total = crcsOff + npages * 4;

  std::vector<uint8_t> buf(total, 0);
  std::memcpy(buf.data(), kMagic, 4);
  put<uint32_t>(buf, 4, MetaData::kFormatVersion);
  put<uint32_t>(buf, 8, m.pageSize);
  put<uint32_t>(buf, 12, m.flags);
  put<uint64_t>(buf, 16, m.fileSize);
  put<uint64_t>(buf, 24, m.atime);
  put<uint64_t>(buf, 32, m.originMtime);
  buf[40] = m.cksumKind; // 41..43 reserved
  put<uint32_t>(buf, 44, m.originCksum);
  put<uint32_t>(buf, 48, static_cast<uint32_t>(m.key.size()));
  // meta_crc at 52 stays 0 for the digest pass
  std::memcpy(buf.data() + keyOff, m.key.data(), m.key.size());
  std::memcpy(buf.data() + bitmapOff, m.bitmap.raw(), m.bitmap.rawSize());
  // A summary-loaded MetaData carries no pageCrcs; the table then serializes
  // as all-zero — the documented "crc absent" state (and store()ing a summary
  // is forbidden by contract anyway).
  if (npages && m.pageCrcs.size() == npages)
    std::memcpy(buf.data() + crcsOff, m.pageCrcs.data(), npages * 4);
  put<uint32_t>(buf, kCrcFieldOff, crc32c(buf.data(), buf.size()));
  return buf;
}

std::optional<MetaData> MetaFile::deserialize(const uint8_t* p, size_t n) {
  if (n < kHeaderSize || std::memcmp(p, kMagic, 4) != 0)
    return std::nullopt;
  if (get<uint32_t>(p + 4) != MetaData::kFormatVersion)
    return std::nullopt; // reject mismatched versions (treat as absent)

  MetaData m;
  m.pageSize = get<uint32_t>(p + 8);
  m.flags = get<uint32_t>(p + 12);
  m.fileSize = get<uint64_t>(p + 16);
  m.atime = get<uint64_t>(p + 24);
  m.originMtime = get<uint64_t>(p + 32);
  m.cksumKind = p[40];
  m.originCksum = get<uint32_t>(p + 44);
  uint32_t keyLen = get<uint32_t>(p + 48);
  uint32_t storedCrc = get<uint32_t>(p + kCrcFieldOff);

  if (m.pageSize == 0 || (m.pageSize & (m.pageSize - 1)) != 0)
    return std::nullopt;
  const uint64_t npages = m.npages();
  const size_t keyOff = kHeaderSize;
  const size_t bitmapOff = align8(keyOff + keyLen);
  const size_t bitmapSize = (npages + 7) / 8;
  const size_t crcsOff = bitmapOff + bitmapSize;
  const size_t total = crcsOff + npages * 4;
  if (n != total || keyLen > 64 * 1024)
    return std::nullopt;

  // Whole-image CRC with the crc field zeroed (torn-write detection).
  std::vector<uint8_t> copy(p, p + n);
  std::memset(copy.data() + kCrcFieldOff, 0, 4);
  if (crc32c(copy.data(), copy.size()) != storedCrc)
    return std::nullopt;

  m.key.assign(reinterpret_cast<const char*>(p + keyOff), keyLen);
  m.bitmap.reset(npages);
  std::memcpy(m.bitmap.raw(), p + bitmapOff, bitmapSize);
  m.bitmap.recount();
  m.pageCrcs.resize(npages);
  if (npages)
    std::memcpy(m.pageCrcs.data(), p + crcsOff, npages * 4);
  return m;
}

std::optional<MetaData> MetaFile::loadSummary(IOBackend& io, const std::string& path) {
  struct ::stat st;
  if (io.stat(path, &st) < 0)
    return std::nullopt;
  if (st.st_size < static_cast<int64_t>(kHeaderSize) || st.st_size > (int64_t(1) << 31))
    return std::nullopt;
  int fd = io.open(path, O_RDONLY, 0);
  if (fd < 0)
    return std::nullopt;
  uint8_t hdr[kHeaderSize];
  if (io.preadFull(fd, hdr, kHeaderSize, 0) != static_cast<int64_t>(kHeaderSize) ||
      std::memcmp(hdr, kMagic, 4) != 0 ||
      get<uint32_t>(hdr + 4) != MetaData::kFormatVersion) {
    io.close(fd);
    return std::nullopt;
  }
  MetaData m;
  m.pageSize = get<uint32_t>(hdr + 8);
  m.flags = get<uint32_t>(hdr + 12);
  m.fileSize = get<uint64_t>(hdr + 16);
  m.atime = get<uint64_t>(hdr + 24);
  m.originMtime = get<uint64_t>(hdr + 32);
  m.cksumKind = hdr[40];
  m.originCksum = get<uint32_t>(hdr + 44);
  const uint32_t keyLen = get<uint32_t>(hdr + 48);
  if (m.pageSize == 0 || (m.pageSize & (m.pageSize - 1)) != 0 || keyLen > 64 * 1024) {
    io.close(fd);
    return std::nullopt;
  }
  const uint64_t npages = m.npages();
  const size_t keyOff = kHeaderSize;
  const size_t bitmapOff = align8(keyOff + keyLen);
  const size_t bitmapSize = (npages + 7) / 8;
  const size_t crcsOff = bitmapOff + bitmapSize;
  const size_t total = crcsOff + npages * 4;
  // Geometry gate in place of the whole-image crc: a truncated/grown image
  // fails here (rewrites are atomic tmp+rename, so partial images are the
  // only torn shape and they change the size).
  if (static_cast<uint64_t>(st.st_size) != total) {
    io.close(fd);
    return std::nullopt;
  }
  std::vector<uint8_t> buf(crcsOff - keyOff);
  int64_t r = io.preadFull(fd, buf.data(), buf.size(), keyOff);
  io.close(fd);
  if (r != static_cast<int64_t>(buf.size()))
    return std::nullopt;
  m.key.assign(reinterpret_cast<const char*>(buf.data()), keyLen);
  m.bitmap.reset(npages);
  std::memcpy(m.bitmap.raw(), buf.data() + (bitmapOff - keyOff), bitmapSize);
  m.bitmap.recount();
  // pageCrcs intentionally left empty (see header contract).
  return m;
}

std::optional<MetaData> MetaFile::load(IOBackend& io, const std::string& path) {
  struct ::stat st;
  if (io.stat(path, &st) < 0)
    return std::nullopt;
  if (st.st_size <= 0 || st.st_size > (int64_t(1) << 31))
    return std::nullopt;
  int fd = io.open(path, O_RDONLY, 0);
  if (fd < 0)
    return std::nullopt;
  std::vector<uint8_t> buf(static_cast<size_t>(st.st_size));
  int64_t r = io.preadFull(fd, buf.data(), buf.size(), 0);
  io.close(fd);
  if (r != static_cast<int64_t>(buf.size()))
    return std::nullopt;
  return deserialize(buf.data(), buf.size());
}

int MetaFile::store(IOBackend& io, const std::string& path, const MetaData& m, bool fsyncMeta) {
  auto buf = serialize(m);
  const std::string tmp = path + ".tmp";
  int fd = io.open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return fd;
  int64_t w = io.pwriteFull(fd, buf.data(), buf.size(), 0);
  int rc = 0;
  if (w != static_cast<int64_t>(buf.size()))
    rc = w < 0 ? static_cast<int>(w) : -EIO;
  if (rc == 0 && fsyncMeta)
    rc = io.fdatasync(fd);
  int crc_ = io.close(fd);
  if (rc == 0 && crc_ < 0)
    rc = crc_;
  if (rc != 0) {
    io.unlink(tmp);
    return rc;
  }
  rc = io.rename(tmp, path);
  if (rc != 0)
    io.unlink(tmp);
  return rc;
}

} // namespace ucache
