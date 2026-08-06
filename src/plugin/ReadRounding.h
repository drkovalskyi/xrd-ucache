#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace ucache {

// kXR_readv element ceiling: each element of a vector read must fit inside a
// 2 MiB frame, 16 bytes of which are its header.
constexpr uint64_t kMaxReadvElem = 2 * 1024 * 1024 - 16;

// Widen [off, off+len) out to whole pages, so that what is fetched is what can
// be stored.
//
// The cache stores whole pages and nothing else: a page is the unit of both
// residency (one bit) and integrity (one checksum), so a span that starts or
// ends mid-page leaves those edge pages unstored. A later request asks for
// every page it overlaps, edge pages included, and therefore misses -- refetches
// the same span, stores nothing new, and misses again on every pass after that.
// Rounding the fetched span out to page boundaries is what makes a request
// cacheable at all, and it costs at most two extra pages on the wire.
//
// This holds for every wire request, with no exception for vector reads. Their
// elements are size-capped, but that is a reason to CUT a rounded span into
// legal pieces (readvElemEnd below), never a reason to leave it unrounded:
// declining to round never made an oversized element legal — the request still
// failed — it only made the bytes uncacheable on top.
inline std::pair<uint64_t, uint64_t> roundSpan(uint64_t pageSize, uint64_t fileSize,
                                              uint64_t off, uint64_t len) {
  const uint64_t start = off / pageSize * pageSize;
  const uint64_t end =
      std::min<uint64_t>((off + len + pageSize - 1) / pageSize * pageSize, fileSize);

  // At or past EOF the clamp can put end below start; rounding has nothing to
  // widen into, and the subtraction below would wrap.
  if (end <= start)
    return {off, off + len};

  // The wire read length crosses a uint32_t API boundary. A rounded span that
  // would not fit is issued unrounded rather than silently truncated -- losing
  // edge pages is a missed optimization, serving truncated bytes is a wrong
  // answer. Unreachable for real readers: it takes a request within two pages
  // of 4 GiB.
  if (end - start > std::numeric_limits<uint32_t>::max())
    return {off, off + len};

  return {start, end};
}

// Where to cut [start, end) so the piece is a legal vector-read element and
// still ends on a page boundary, keeping it storable whole. Callers loop until
// the returned cut reaches `end`.
//
// Cutting is mandatory, not an optimization: an element above the ceiling is
// refused by the protocol and the whole request fails. Page-aligned cuts keep
// every piece cacheable, so a large read costs no residency either.
inline uint64_t readvElemEnd(uint64_t start, uint64_t end, uint64_t pageSize) {
  if (end - start <= kMaxReadvElem)
    return end;
  uint64_t cut = start + kMaxReadvElem;
  cut -= cut % pageSize; // land on an absolute page boundary
  if (cut <= start)      // page bigger than the ceiling: one page per element
    cut = start + pageSize;
  return std::min(cut, end);
}

} // namespace ucache
