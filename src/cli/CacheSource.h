// Byte-cache view for the overlay builder: reads an entry's sparse .data
// image, gated by its sidecar bitmap and VERIFIED against its per-page crc32c.
//
// Why the verification matters. A cache entry's bitmap is a snapshot: the
// builder loads the sidecar once and then reads baskets for as long as the
// build takes. Reclaim (`punchSuperseded`) clears the bits, flushes the
// sidecar and only then punches the bytes, precisely so that a reader still
// holding the older bitmap is caught by the per-page checksum — punched pages
// read back as zeros, which no longer match the crc recorded when the page was
// filled. The serving path relies on that (it verifies every page it reads);
// a builder that pread's the image raw does not, and so consumes the zeros
// instead: the hole surfaces much later as "unexpected basket key" or "basket
// decompression failed", which reads like a corrupt file rather than the
// transient, self-healing condition it actually is.
//
// Verifying here also covers ordinary rot in the byte cache during a build,
// and costs nothing worth measuring: checksumming is hardware-accelerated and
// runs against bytes the builder is about to decompress and re-encode anyway.
//
// Read granularity is deliberately NOT checksum granularity: a basket is
// fetched with one pread over its whole page span, then each covered page is
// checked from that buffer. The op count is what it was before verification.
#ifndef UCACHE_CLI_CACHESOURCE_H
#define UCACHE_CLI_CACHESOURCE_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <vector>

#include "MetaFile.h"
#include "Transposer.h"
#include "vendor/crc32c.h"

namespace ucache {

// Not thread-safe: one instance per builder thread (each holds its own scratch
// buffer and its own file descriptor).
struct CacheSource : transpose::Source {
  int fd = -1;
  const MetaData* meta = nullptr;

  // A range is usable only if every page it touches is marked present. The
  // caller's own coverage check runs earlier and over the same bitmap; this is
  // the per-read backstop.
  bool has(uint64_t off, uint64_t n) override {
    if (!meta || n == 0 || off + n < off || off + n > meta->fileSize)
      return false;
    const uint64_t first = off / meta->pageSize, last = (off + n - 1) / meta->pageSize;
    for (uint64_t i = first; i <= last; ++i)
      if (!meta->bitmap.get(i))
        return false;
    return true;
  }

  // One pread over the page span, then crc32c per covered page. False on a
  // short read, a checksum mismatch (a hole punched under us, or rot), or a
  // sidecar carrying no checksum table — never a partially-filled `dst`.
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (!meta || n == 0 || off + n < off || off + n > meta->fileSize)
      return false;
    const uint32_t P = meta->pageSize;
    if (!P || meta->pageCrcs.size() != meta->npages())
      return false; // summary-loaded sidecar: nothing to verify against
    const uint64_t first = off / P, last = (off + n - 1) / P;
    const uint64_t base = first * static_cast<uint64_t>(P);
    const uint64_t end = std::min<uint64_t>((last + 1) * static_cast<uint64_t>(P), meta->fileSize);
    buf_.resize(static_cast<size_t>(end - base));
    if (::pread(fd, buf_.data(), buf_.size(), static_cast<off_t>(base)) !=
        static_cast<ssize_t>(buf_.size()))
      return false;
    for (uint64_t i = first; i <= last; ++i) {
      const uint32_t nb = meta->pageBytes(i);
      const uint8_t* p = buf_.data() + (i * static_cast<uint64_t>(P) - base);
      if (crc32c(p, nb) != meta->pageCrcs[i])
        return false;
    }
    std::memcpy(dst, buf_.data() + (off - base), static_cast<size_t>(n));
    return true;
  }

 private:
  std::vector<uint8_t> buf_;
};

} // namespace ucache

#endif // UCACHE_CLI_CACHESOURCE_H
