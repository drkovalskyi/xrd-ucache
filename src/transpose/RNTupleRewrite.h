// Recompress an RNTuple's pages by patch-and-append.
//
// Every page is decoded and re-encoded into a new codec, appended as a fresh
// RBlob key, and its locator repointed. The rebuilt page list and footer are
// appended too, and the anchor is patched where it lies to point at them. The
// original pages become dead space; reclaiming it is a separate concern, and a
// from-scratch container writer would be far more code for no extra confidence.
//
// The result is deliberately expressed as "an extension plus a few in-place
// patch windows" rather than as a finished file, because that is exactly the
// shape the replica tier stores: the extension becomes the replica's data and
// the patches are applied over the original when serving. A standalone
// rewritten file is then just one way of consuming the same result, which is
// what makes it possible to hand the output to ROOT and ask whether it reads.
//
// Thread-safety: pure functions over caller-owned data; no shared state.
#pragma once

#include "RNTupleMeta.h"
#include "Transposer.h" // Source

#include <cstdint>
#include <string>
#include <vector>

namespace ucache::transpose {

// A window of bytes to overwrite in place. In-place means the record is never
// relocated and its size never changes, so no enclosing key needs adjusting.
struct RNTuplePatch {
  uint64_t offset = 0;
  std::vector<uint8_t> bytes;
};

struct RNTupleRewrite {
  uint64_t extBase = 0;             // where `extension` is appended (old file size)
  std::vector<uint8_t> extension;   // relocated pages + page list + footer
  std::vector<RNTuplePatch> patches; // anchor payload, and fEND

  uint64_t pages = 0, storedRaw = 0;
  uint64_t oldPageBytes = 0, newPageBytes = 0;
  uint64_t decodeBytes = 0, decodeNs = 0; // calibration data, as for baskets

  std::string error;
  // As for a basket build: set when the SOURCE could not vouch for bytes it
  // was asked for. Retryable, and says nothing about the file being malformed.
  bool transient = false;
};

// Recompress every page of `m` to ZSTD at `level`, reading original page bytes
// through `src`. `fileSize` is the source file's size, which is where the
// extension begins.
RNTupleRewrite buildRNTupleRewrite(const RNTupleMeta& m, Source& src, uint64_t fileSize,
                                   int level);

// Apply a rewrite to a copy of `srcPath`, producing a standalone file. This is
// the verification path: the arbiter for any change here is whether ROOT reads
// the result and reports the same physics.
bool writeRewrittenRNTuple(const std::string& srcPath, const std::string& dstPath,
                           const RNTupleRewrite& rw, std::string& error);

} // namespace ucache::transpose
