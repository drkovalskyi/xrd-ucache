// .meta sidecar (format_version 1 — full field table in
// docs/FORMAT.md; bump kFormatVersion on ANY layout change).
//
// Integrity: the header carries a crc32c over the entire serialized image
// (computed with the crc field zeroed), so torn or corrupt sidecars fail to
// load and are treated as absent — a crash can only lose cached pages, never
// corrupt served data. Rewrites go through <path>.tmp + rename (atomic on
// POSIX) under flock(LOCK_EX) on the entry's .data fd.
//
// Thread-safety: MetaData is a plain value; MetaFile functions are pure with
// respect to shared state (locking is the caller's job — see FileEntry).
#pragma once

#include "IOBackend.h"
#include "PageBitmap.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucache {

struct MetaData {
  static constexpr uint32_t kFormatVersion = 1;
  static constexpr uint32_t kFlagPinned = 1u << 0;
  static constexpr uint32_t kFlagComplete = 1u << 1;
  enum CksumKind : uint8_t { kCksumNone = 0, kCksumAdler32 = 1, kCksumCrc32c = 2 };

  uint32_t pageSize = 0; // fixed at entry creation
  uint32_t flags = 0;
  uint64_t fileSize = 0;
  uint64_t atime = 0;       // coarse (<= 1/min), eviction LRU key
  uint64_t originMtime = 0; // 0 when validation mode doesn't use it
  uint8_t cksumKind = kCksumNone;
  uint32_t originCksum = 0;
  std::string key; // full normalized key, for ls/debug
  PageBitmap bitmap;
  std::vector<uint32_t> pageCrcs; // crc32c per present page; 0 when absent

  uint64_t npages() const {
    return pageSize ? (fileSize + pageSize - 1) / pageSize : 0;
  }
  // Bytes the page at index i actually covers (tail page may be short).
  uint32_t pageBytes(uint64_t i) const {
    uint64_t start = i * pageSize;
    uint64_t end = std::min<uint64_t>(start + pageSize, fileSize);
    return static_cast<uint32_t>(end - start);
  }
  uint64_t cachedBytes() const;

  // Fresh meta for a new entry.
  static MetaData fresh(const std::string& key, uint64_t fileSize, uint32_t pageSize);
};

class MetaFile {
 public:
  // nullopt on missing/torn/corrupt/version-mismatched sidecars ("treat as
  // absent"); never throws.
  static std::optional<MetaData> load(IOBackend& io, const std::string& path);
  // Scan-path load: reads header + key + bitmap and SKIPS the
  // per-page crc table — ~97% of a real sidecar's bytes. Integrity is
  // magic/version/geometry plus an exact file-size match (store() rewrites
  // are atomic tmp+rename, so torn images present as size mismatches); the
  // whole-image crc32c is NOT verified. pageCrcs stays EMPTY: a summary is
  // for reporting/eviction accounting only — never serve or validate pages
  // from it, and never store() it back (crcs would serialize as absent).
  static std::optional<MetaData> loadSummary(IOBackend& io, const std::string& path);
  // Atomic rewrite via tmp+rename; 0 or -errno. fsyncMeta per UCACHE_FSYNC=all.
  static int store(IOBackend& io, const std::string& path, const MetaData& m, bool fsyncMeta);

  // Exposed for tests.
  static std::vector<uint8_t> serialize(const MetaData& m);
  static std::optional<MetaData> deserialize(const uint8_t* p, size_t n);
};

} // namespace ucache
