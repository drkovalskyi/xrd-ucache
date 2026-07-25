// Transposed-replica sidecars: a replica
// lives NEXT TO the v1 entry in two extra files —
//   objects/<aa>/<hash>.tdata   overlay bytes: the small patched header/
//                               directory windows plus the extension region
//                               (re-encoded hot baskets, relocated metadata)
//   objects/<aa>/<hash>.tmeta   this sidecar (format below)
// while the v1 .meta stays at ITS format v1, so mixed-version processes
// sharing one cache dir coexist: a v1.0.0 reader never looks at replica
// files (deliberate: no format-version bump, for coexistence).
//
// The stitched virtual view = origin bytes, except where an extent says the
// range comes from .tdata. Extents are sorted, non-overlapping, and may lie
// past the origin EOF (the extension); virtualSize is the stitched fEND′
// that Stat must report so ROOT's fBEGIN <= fEND <= size check passes.
//
// Integrity mirrors MetaFile: whole-image crc32c on the sidecar (torn =>
// treat as absent), per-page crc32c over .tdata (kOverlayPageSize, an
// economy choice — serving is page-size-insensitive), origin
// validators so an origin change invalidates the replica,
// and an encoder pin (encoding + encoderVersion) so repair/rebuild never
// mixes bytes from different encoder builds.
//
// Thread-safety: ReplicaMeta is a plain value; ReplicaFile functions are
// pure with respect to shared state (locking is the caller's job).
#pragma once

#include "IOBackend.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucache {

struct ReplicaMeta {
  static constexpr uint32_t kFormatVersion = 1;
  static constexpr uint32_t kOverlayPageSize = 64 * 1024; // economy, not perf
  enum Encoding : uint8_t { kZstd1 = 1, kLz4 = 2, kRaw = 3 };

  // One stitched range: virtual [virtOff, virtOff+len) is served from
  // .tdata [tdataOff, tdataOff+len).
  struct Extent {
    uint64_t virtOff = 0, len = 0, tdataOff = 0;
  };
  // Original-file byte range superseded by the overlay (input to
  // punch-and-clear, and the repair recipe: refetchable at origin offsets).
  struct Range {
    uint64_t off = 0, len = 0;
  };

  uint32_t flags = 0;   // reserved
  uint8_t encoding = kZstd1;
  uint32_t encoderVersion = 0; // builder's codec library version (pinned)
  // Origin validators — must match the v1 entry's view of the origin (§7).
  uint64_t originSize = 0;
  uint64_t originMtime = 0;
  uint8_t cksumKind = 0;
  uint32_t originCksum = 0;
  uint64_t virtualSize = 0; // stitched fEND′ (>= originSize)
  uint64_t tdataBytes = 0;  // exact .tdata size
  std::string key;          // full normalized key, for ls/debug
  std::vector<Extent> extents;      // sorted by virtOff, non-overlapping
  std::vector<Range> superseded;    // original ranges replaced by the overlay
  std::vector<uint32_t> pageCrcs;   // crc32c per overlay page of .tdata

  uint64_t npages() const {
    return (tdataBytes + kOverlayPageSize - 1) / kOverlayPageSize;
  }
  // Bytes overlay page i actually covers (tail page may be short).
  uint32_t pageBytes(uint64_t i) const {
    uint64_t start = i * kOverlayPageSize;
    uint64_t end = std::min<uint64_t>(start + kOverlayPageSize, tdataBytes);
    return static_cast<uint32_t>(end - start);
  }
  // Byte span of pages [first, last] — exact, since only the tail page of
  // .tdata is ever short. Used to size one coalesced pread over a run.
  uint64_t runBytes(uint64_t first, uint64_t last) const {
    return last * uint64_t(kOverlayPageSize) + pageBytes(last) -
           first * uint64_t(kOverlayPageSize);
  }
};

class ReplicaFile {
 public:
  // nullopt on missing/torn/corrupt/version-mismatched/inconsistent sidecars
  // ("treat as absent"); never throws. Consistency checked: extents sorted,
  // non-overlapping, inside [0, virtualSize) and [0, tdataBytes) on the
  // .tdata side; virtualSize >= originSize.
  static std::optional<ReplicaMeta> load(IOBackend& io, const std::string& path);
  // Atomic rewrite via tmp+rename; 0 or -errno.
  static int store(IOBackend& io, const std::string& path, const ReplicaMeta& m,
                   bool fsyncMeta);

  // Exposed for tests.
  static std::vector<uint8_t> serialize(const ReplicaMeta& m);
  static std::optional<ReplicaMeta> deserialize(const uint8_t* p, size_t n);
};

} // namespace ucache
