#include "ReplicaFile.h"

#include "vendor/crc32c.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>

namespace ucache {
namespace {

constexpr char kMagic[4] = {'U', 'C', 'T', 'R'};
constexpr size_t kHeaderSize = 72;
constexpr size_t kCrcFieldOff = 68;
// Sanity bounds: reject absurd sidecars instead of allocating for them.
constexpr uint32_t kMaxKeyLen = 64 * 1024;
constexpr uint32_t kMaxExtents = 1u << 20;
constexpr uint32_t kMaxSuperseded = 1u << 24;

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

std::vector<uint8_t> ReplicaFile::serialize(const ReplicaMeta& m) {
  const uint64_t npages = m.npages();
  const size_t keyOff = kHeaderSize;
  const size_t extOff = align8(keyOff + m.key.size());
  const size_t supOff = extOff + m.extents.size() * 24;
  const size_t crcsOff = supOff + m.superseded.size() * 16;
  const size_t total = crcsOff + npages * 4;

  std::vector<uint8_t> buf(total, 0);
  std::memcpy(buf.data(), kMagic, 4);
  put<uint32_t>(buf, 4, ReplicaMeta::kFormatVersion);
  put<uint32_t>(buf, 8, m.flags);
  buf[12] = m.encoding;
  buf[13] = m.cksumKind; // 14..15 reserved
  put<uint64_t>(buf, 16, m.originSize);
  put<uint64_t>(buf, 24, m.originMtime);
  put<uint32_t>(buf, 32, m.originCksum);
  put<uint32_t>(buf, 36, m.encoderVersion);
  put<uint64_t>(buf, 40, m.virtualSize);
  put<uint64_t>(buf, 48, m.tdataBytes);
  put<uint32_t>(buf, 56, static_cast<uint32_t>(m.key.size()));
  put<uint32_t>(buf, 60, static_cast<uint32_t>(m.extents.size()));
  put<uint32_t>(buf, 64, static_cast<uint32_t>(m.superseded.size()));
  // image crc at 68 stays 0 for the digest pass
  std::memcpy(buf.data() + keyOff, m.key.data(), m.key.size());
  size_t p = extOff;
  for (const auto& e : m.extents) {
    put<uint64_t>(buf, p, e.virtOff);
    put<uint64_t>(buf, p + 8, e.len);
    put<uint64_t>(buf, p + 16, e.tdataOff);
    p += 24;
  }
  for (const auto& r : m.superseded) {
    put<uint64_t>(buf, p, r.off);
    put<uint64_t>(buf, p + 8, r.len);
    p += 16;
  }
  // Defensive: copy at most what the caller supplied (missing tail stays 0
  // and will fail the overlay CRC verify => treated as absent, fail-open).
  const size_t ncrcs = std::min<size_t>(npages, m.pageCrcs.size());
  if (ncrcs)
    std::memcpy(buf.data() + crcsOff, m.pageCrcs.data(), ncrcs * 4);
  put<uint32_t>(buf, kCrcFieldOff, crc32c(buf.data(), buf.size()));
  return buf;
}

std::optional<ReplicaMeta> ReplicaFile::deserialize(const uint8_t* p, size_t n) {
  if (n < kHeaderSize || std::memcmp(p, kMagic, 4) != 0)
    return std::nullopt;
  if (get<uint32_t>(p + 4) != ReplicaMeta::kFormatVersion)
    return std::nullopt; // reject mismatched versions (treat as absent)

  ReplicaMeta m;
  m.flags = get<uint32_t>(p + 8);
  m.encoding = p[12];
  m.cksumKind = p[13];
  m.originSize = get<uint64_t>(p + 16);
  m.originMtime = get<uint64_t>(p + 24);
  m.originCksum = get<uint32_t>(p + 32);
  m.encoderVersion = get<uint32_t>(p + 36);
  m.virtualSize = get<uint64_t>(p + 40);
  m.tdataBytes = get<uint64_t>(p + 48);
  uint32_t keyLen = get<uint32_t>(p + 56);
  uint32_t nExt = get<uint32_t>(p + 60);
  uint32_t nSup = get<uint32_t>(p + 64);
  uint32_t storedCrc = get<uint32_t>(p + kCrcFieldOff);

  if (keyLen > kMaxKeyLen || nExt > kMaxExtents || nSup > kMaxSuperseded)
    return std::nullopt;
  const uint64_t npages = m.npages();
  const size_t keyOff = kHeaderSize;
  const size_t extOff = align8(keyOff + keyLen);
  const size_t supOff = extOff + size_t(nExt) * 24;
  const size_t crcsOff = supOff + size_t(nSup) * 16;
  const size_t total = crcsOff + npages * 4;
  if (n != total)
    return std::nullopt;

  // Whole-image CRC with the crc field zeroed (torn-write detection).
  std::vector<uint8_t> copy(p, p + n);
  std::memset(copy.data() + kCrcFieldOff, 0, 4);
  if (crc32c(copy.data(), copy.size()) != storedCrc)
    return std::nullopt;

  m.key.assign(reinterpret_cast<const char*>(p + keyOff), keyLen);
  m.extents.resize(nExt);
  size_t q = extOff;
  for (auto& e : m.extents) {
    e.virtOff = get<uint64_t>(p + q);
    e.len = get<uint64_t>(p + q + 8);
    e.tdataOff = get<uint64_t>(p + q + 16);
    q += 24;
  }
  m.superseded.resize(nSup);
  for (auto& r : m.superseded) {
    r.off = get<uint64_t>(p + q);
    r.len = get<uint64_t>(p + q + 8);
    q += 16;
  }
  m.pageCrcs.resize(npages);
  if (npages)
    std::memcpy(m.pageCrcs.data(), p + crcsOff, npages * 4);

  // Structural consistency: a sidecar that lies about its geometry is treated
  // as absent — the serving layer must be able to trust extents blindly.
  if (m.virtualSize < m.originSize)
    return std::nullopt;
  uint64_t prevEnd = 0;
  for (const auto& e : m.extents) {
    if (e.len == 0 || e.virtOff < prevEnd)
      return std::nullopt; // unsorted or overlapping
    if (e.virtOff + e.len < e.virtOff || e.virtOff + e.len > m.virtualSize)
      return std::nullopt;
    if (e.tdataOff + e.len < e.tdataOff || e.tdataOff + e.len > m.tdataBytes)
      return std::nullopt;
    prevEnd = e.virtOff + e.len;
  }
  for (const auto& r : m.superseded)
    if (r.len == 0 || r.off + r.len < r.off || r.off + r.len > m.originSize)
      return std::nullopt;
  return m;
}

std::optional<ReplicaMeta> ReplicaFile::load(IOBackend& io, const std::string& path) {
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

int ReplicaFile::store(IOBackend& io, const std::string& path, const ReplicaMeta& m,
                       bool fsyncMeta) {
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
