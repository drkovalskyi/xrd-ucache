// Native overlay builder: re-encodes a hot branch
// set's baskets to ZSTD-1 in an extension region, relocates the (patched)
// tree metadata and keys list, and emits the replica overlay — byte-for-byte
// the artifact the uproot-based reference builder produces (the differential
// gate), ready for ReplicaStore::publish.
//
// Sources: a plain file, or a v1 cache .data image where every range the
// build touches must be PRESENT in the entry's bitmap (deriveHotBranches
// only ever selects fully-cached branches, so `ucache materialize` works
// from cached bytes alone — no network in the CLI).
//
// Every failure returns Overlay{error}; the caller fails open (no replica).
//
// Thread-safety: pure functions over caller-owned data; no shared state.
#pragma once

#include "ReplicaFile.h"
#include "TreeMeta.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ucache::transpose {

// Byte source with presence semantics (bitmap-gated for cache images).
struct Source {
  virtual ~Source() = default;
  virtual bool read(void* dst, uint64_t n, uint64_t off) = 0;
  virtual bool has(uint64_t off, uint64_t n) = 0; // false => range not usable
};

// Σ uncompressed bytes (fObjlen from each basket's key header — NO decode)
// of the `hot` branches: the estimator's numerator.
uint64_t hotUncompressedBytes(const FileMeta& fm, const std::vector<std::string>& hot,
                              Source& src);

struct Overlay {
  uint64_t decodeBytes = 0; // uncompressed bytes produced by the transcode's
  uint64_t decodeNs = 0;    // decodes + the time they took: calibration data
  std::vector<uint8_t> tdata;
  ReplicaMeta meta; // extents/superseded/virtualSize/encoding filled; origin
                    // validators + publish integrity are the caller's job
  // build accounting (mirrors the Python builder's report)
  uint64_t baskets = 0, transcoded = 0, verbatim = 0, fallbackRaw = 0;
  uint64_t oldBytes = 0, newBytes = 0;
  // relocated tree-metadata key: raw (decompressed) size vs
  // the size actually stored at rest (ROOT-compressed, or raw on fallback).
  uint64_t metaRawBytes = 0, metaStoredBytes = 0;
  std::string error; // non-empty => build failed (fail open)
};

// Append `name`'s counter leaves so the hot set is loop-complete — the same
// rule the Python builder applies (with_counters).
// Counters ride along ONLY when buildable (all their baskets present in
// `src`): a partially-read counter must not fail the file's build.
std::vector<std::string> withCounters(const FileMeta& fm,
                                      const std::vector<std::string>& names, Source& src);

// Source codec of a branch's baskets, from basket 0's compression frame:
// "lzma" | "zlib" | "zstd" | "lz4" | "none" (uncompressed) | "" (unreadable).
std::string branchCodec(const FileMeta& fm, const BranchInfo& b, Source& src);

// Every basket of `b` fully present in `src`?
bool fullyCached(const BranchInfo& b, Source& src);

// Learner: branches whose EVERY basket range is fully present in the
// cache — exactly what warm runs touched. Counters ride along when buildable.
// The `codecs` form keeps only branches whose source codec is listed
// (recompress_codecs policy); empty list = no filter.
std::vector<std::string> deriveHotBranches(const FileMeta& fm, Source& src);
std::vector<std::string> deriveHotBranches(const FileMeta& fm, Source& src,
                                           const std::vector<std::string>& codecs);

// Build the overlay for `hot` (already counter-completed) over `src`.
Overlay buildOverlay(const FileMeta& fm, Source& src, const std::vector<std::string>& hot);

} // namespace ucache::transpose
