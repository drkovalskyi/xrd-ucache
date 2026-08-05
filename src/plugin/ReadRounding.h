#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace ucache {

// kXR_readv element ceiling: each element of a vector read must fit inside a
// 2 MiB frame, 16 bytes of which are its header.
constexpr uint64_t kMaxReadvElem = 2 * 1024 * 1024 - 16;

// Which wire request the rounded span is destined for. This is not a tuning
// choice: it is a protocol fact about the request, and picking the wrong one
// silently costs cacheability (see below).
enum class RoundCap {
  kWholeRead,  // one kXR_read: no element ceiling exists, so always round
  kReadvElem,  // one kXR_readv element: bounded by kMaxReadvElem
};

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
// Only kXR_readv has a reason not to round: its elements are capped, so a
// rounded element that would cross kMaxReadvElem is issued unrounded and
// knowingly gives up its edge pages. A single kXR_read has no element limit, so
// it always rounds. Applying the readv cap to single reads made every large
// unaligned read permanently uncacheable -- reads above the cap were served
// correctly and then dropped, forever, on every pass.
inline std::pair<uint64_t, uint64_t> roundSpan(uint64_t pageSize, uint64_t fileSize,
                                              uint64_t off, uint64_t len, RoundCap cap) {
  const uint64_t start = off / pageSize * pageSize;
  const uint64_t end =
      std::min<uint64_t>((off + len + pageSize - 1) / pageSize * pageSize, fileSize);

  // At or past EOF the clamp can put end below start; rounding has nothing to
  // widen into, and the subtraction below would wrap.
  if (end <= start)
    return {off, off + len};

  if (cap == RoundCap::kReadvElem && end - start > kMaxReadvElem)
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

} // namespace ucache
