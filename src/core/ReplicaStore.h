// Replica lifecycle: crash-safe publish, validated adoption, drop, and
// punch-and-clear of superseded v1 pages.
//
// Publish protocol (a torn publish must NEVER yield a servable stitched
// view): write .tdata.tmp -> fdatasync -> rename to .tdata, THEN store
// .tmeta (itself tmp+fsync+rename). Readers adopt only through a valid
// .tmeta, so a visible .tmeta implies a complete, synced .tdata. A crash
// between the renames leaves an orphaned .tdata that eviction sweeps.
//
// Adoption (openView) fail-opens on everything: missing/torn/mismatched
// sidecar, validator mismatch vs the origin, .tdata size
// mismatch, or any overlay-page CRC failure during the OPEN-TIME FULL
// VERIFY -> the replica is quarantined (files dropped)
// and nullptr returned; the caller serves the plain v1 view. After a
// successful open the view is handle-stable: reads re-verify the touched
// overlay pages' CRCs and a failure marks the view invalid for the caller's
// repair ladder, but never silently returns bad bytes.
//
// Verify-once (.tok marker): the full open-time scan
// is skipped when an advisory marker matches the overlay's (size, mtime,
// crc-of-page-crcs) — written by publish and by the first successful full
// verify, so a 32-process batch scans once, not 32 times. The marker is
// ADVISORY only: a stale/torn/missing marker just re-runs the full scan, and
// the per-read page CRC stays on regardless, so a corruption that somehow
// preserved size+mtime is still caught before any byte is served.
//
// Thread-safety: ReplicaView is immutable after construction plus one
// atomic invalid flag; concurrent read() is safe (stateless pread).
// ReplicaStore methods are stateless orchestration and thread-safe.
#pragma once

#include "Config.h"
#include "FileEntry.h"
#include "ReplicaFile.h"
#include "Stats.h"
#include "UrlKey.h"

#include <atomic>
#include <memory>

namespace ucache {

class ReplicaView {
 public:
  ~ReplicaView();
  ReplicaView(const ReplicaView&) = delete;
  ReplicaView& operator=(const ReplicaView&) = delete;

  const ReplicaMeta& meta() const { return meta_; }
  uint64_t virtualSize() const { return meta_.virtualSize; }

  // One piece of a stitched range: overlay pieces come from .tdata at
  // tdataOff; non-overlay pieces are plain origin bytes (v1 extent path).
  struct Seg {
    bool overlay = false;
    uint64_t off = 0; // virtual offset
    uint64_t len = 0;
    uint64_t tdataOff = 0; // valid when overlay
  };
  // Split [off, off+len) (clamped to virtualSize) into consecutive segments.
  std::vector<Seg> map(uint64_t off, uint64_t len) const;

  // The ORIGINAL-file ranges whose content a virtual read of [off, off+len)
  // carried. Overlay pieces are looked up in origMap and yield the WHOLE
  // original range they came from -- a recompressed basket has no meaningful
  // sub-offset, so touching part of it means that basket was read. Pieces the
  // overlay does not cover are origin bytes already and pass through. Empty
  // when the sidecar predates origMap (v1), which the caller must treat as
  // "unknown", never as "nothing".
  void originRanges(uint64_t off, uint64_t len,
                    std::vector<ReplicaMeta::Range>& out) const;
  bool hasOriginMap() const { return !meta_.origMap.empty(); }

  // Read overlay bytes [tdataOff, +len), verifying every touched overlay
  // page's CRC. false => bad page or IO error: replica_crc_failures counted,
  // invalid() latched.
  //
  // CALLER CONTRACT: on false, `buf` holds UNDEFINED bytes — a contiguous run
  // of pages is read in one pread and verified afterwards, so bytes that
  // failed verification may already have landed there. Treat the whole
  // request as unserved and overwrite the entire span (every caller in this
  // tree either errors the request or refetches it in full).
  bool read(uint64_t tdataOff, uint64_t len, void* buf);

  bool invalid() const { return invalid_.load(std::memory_order_relaxed); }

 private:
  friend class ReplicaStore;
  ReplicaView(IOBackend& io, Stats& stats, ReplicaMeta meta, int fd)
      : io_(io), stats_(stats), meta_(std::move(meta)), fd_(fd) {}

  IOBackend& io_;
  Stats& stats_;
  const ReplicaMeta meta_;
  int fd_ = -1;
  std::atomic<bool> invalid_{false};
};

class ReplicaStore {
 public:
  ReplicaStore(IOBackend& io, const Config& cfg, Stats& stats)
      : io_(io), cfg_(cfg), stats_(stats) {}

  // objects/<aa>/<hash>.tdata / .tmeta / .tok under cacheDir.
  static std::string tdataPath(const UrlKey& key, const std::string& cacheDir);
  static std::string tmetaPath(const UrlKey& key, const std::string& cacheDir);
  static std::string tokPath(const UrlKey& key, const std::string& cacheDir);

  // Crash-safe publish (header protocol). `meta.pageCrcs`/`tdataBytes` are
  // (re)computed here from the payload — callers cannot publish inconsistent
  // integrity data. 0 or -errno; on failure nothing is adopted and any tmp
  // is removed (fail-open: entry stays v1-only).
  int publish(const UrlKey& key, ReplicaMeta meta, const void* tdata, uint64_t n);

  // Validated adoption + open-time full overlay verify (header contract).
  // Origin metadata must be the same values the v1 entry validated against.
  std::shared_ptr<ReplicaView> openView(const UrlKey& key, uint64_t originSize,
                                        uint64_t originMtime = 0, uint8_t cksumKind = 0,
                                        uint32_t originCksum = 0);

  // Remove replica files (untranspose/invalidate). Best-effort, idempotent.
  void drop(const UrlKey& key);

  // Punch-and-clear (D1): for each superseded original range, clear the v1
  // bitmap bits of every FULLY-covered page (partial edge pages stay), flush
  // the v1 sidecar, and only then hole-punch the cleared spans in .data — a
  // crash in between costs cached pages, never serves zeros as data. The
  // entry MUST belong to the same key/cache dir. Returns bytes punched
  // (0 when the filesystem lacks punch support; bits are cleared regardless).
  uint64_t punchSuperseded(FileEntry& entry, const std::vector<ReplicaMeta::Range>& ranges);

 private:
  IOBackend& io_;
  const Config& cfg_;
  Stats& stats_;
};

} // namespace ucache
