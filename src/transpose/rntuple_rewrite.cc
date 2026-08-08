// Recompress an RNTuple file's pages and write a standalone result, so the
// transcode can be handed to ROOT and checked. ROOT is the arbiter here: a
// reader that is wrong but self-consistent round-trips through our own code
// perfectly and is rejected only when ROOT opens the file.
#include "RNTupleRewrite.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

// Plain-file byte source: every byte is present, which is the difference
// between rewriting a file and rewriting a partially cached entry.
//
// UCACHE_TEST_HIDE_ABOVE makes it declare everything past an offset absent, so
// the partial-cache path — the one that produces a partly relocated file — can
// be exercised without building a real half-filled cache entry.
class FileSource : public ucache::transpose::Source {
public:
  explicit FileSource(int fd, uint64_t size) : fd_(fd), size_(size) {
    if (const char* h = ::getenv("UCACHE_TEST_HIDE_ABOVE")) hide_ = std::strtoull(h, nullptr, 10);
  }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    return ::pread(fd_, dst, n, (off_t)off) == (ssize_t)n;
  }
  bool has(uint64_t off, uint64_t n) override {
    if (off + n < off || off + n > size_) return false;
    return hide_ == 0 || off + n <= hide_;
  }

private:
  int fd_;
  uint64_t size_;
  uint64_t hide_ = 0;
};

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s IN.root OUT.root [zstd-level]\n", argv[0]);
    return 2;
  }
  const int level = argc > 3 ? std::atoi(argv[3]) : 1;
  std::vector<std::string> codecs; // comma-separated source-codec policy
  if (argc > 4) {
    std::string s = argv[4], cur;
    for (char c : s + ",")
      if (c == ',') {
        if (!cur.empty()) codecs.push_back(cur);
        cur.clear();
      } else
        cur += c;
  }

  auto m = ucache::transpose::parseRNTuple(argv[1], "");
  if (!m.error.empty()) {
    std::fprintf(stderr, "parse: %s\n", m.error.c_str());
    return 1;
  }
  int fd = ::open(argv[1], O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  FileSource src(fd, m.fileSize);
  auto rw = ucache::transpose::buildRNTupleRewrite(m, src, m.fileSize, level, codecs);
  ::close(fd);
  if (!rw.error.empty()) {
    std::fprintf(stderr, "rewrite: %s%s\n", rw.error.c_str(), rw.transient ? " (retryable)" : "");
    return 1;
  }
  std::string err;
  if (!ucache::transpose::writeRewrittenRNTuple(argv[1], argv[2], rw, err)) {
    std::fprintf(stderr, "write: %s\n", err.c_str());
    return 1;
  }

  // The overlay must stitch to exactly the file ROOT was given. This is what
  // makes the standalone verification transfer to the served form: if the two
  // agree byte for byte, ROOT's verdict on one is a verdict on both.
  auto ov = ucache::transpose::rnTupleOverlay(m, rw);
  if (!ov.error.empty()) {
    std::fprintf(stderr, "overlay: %s\n", ov.error.c_str());
    return 1;
  }
  {
    std::vector<uint8_t> stitched(ov.meta.virtualSize, 0);
    int f = ::open(argv[1], O_RDONLY | O_CLOEXEC);
    if (f < 0 || ::pread(f, stitched.data(), m.fileSize, 0) != (ssize_t)m.fileSize) {
      std::fprintf(stderr, "stitch: cannot re-read source\n");
      return 1;
    }
    ::close(f);
    for (const auto& e : ov.meta.extents)
      std::memcpy(stitched.data() + e.virtOff, ov.tdata.data() + e.tdataOff, e.len);

    std::vector<uint8_t> written(ov.meta.virtualSize, 0);
    f = ::open(argv[2], O_RDONLY | O_CLOEXEC);
    const ssize_t got = f < 0 ? -1 : ::pread(f, written.data(), written.size(), 0);
    if (f >= 0) ::close(f);
    if (got != (ssize_t)written.size() || stitched != written) {
      std::fprintf(stderr, "stitch: overlay does not reproduce the rewritten file\n");
      return 1;
    }
  }
  std::printf("{\"pages\":%llu,\"stored_raw\":%llu,\"page_bytes\":[%llu,%llu],"
              "\"ratio\":%.4f,\"extension_bytes\":%llu,\"decoded_bytes\":%llu,"
              "\"decode_ms\":%.1f}\n",
              (unsigned long long)rw.pages, (unsigned long long)rw.storedRaw,
              (unsigned long long)rw.oldPageBytes, (unsigned long long)rw.newPageBytes,
              rw.oldPageBytes ? (double)rw.newPageBytes / (double)rw.oldPageBytes : 0.0,
              (unsigned long long)rw.extension.size(), (unsigned long long)rw.decodeBytes,
              rw.decodeNs / 1e6);
  std::printf("{\"ranges_relocated\":%llu,\"ranges_uncached\":%llu,\"ranges_declined\":%llu,"
              "\"shared_pages\":%llu}\n",
              (unsigned long long)rw.rangesRelocated, (unsigned long long)rw.rangesUncached,
              (unsigned long long)rw.rangesDeclined, (unsigned long long)rw.sharedPages);
  std::printf("{\"overlay_extents\":%zu,\"tdata_bytes\":%zu,\"superseded_ranges\":%zu,"
              "\"superseded_bytes\":%llu,\"virtual_size\":%llu,\"stitch\":\"matches\"}\n",
              ov.meta.extents.size(), ov.tdata.size(), ov.meta.superseded.size(),
              [&] {
                unsigned long long t = 0;
                for (const auto& r : ov.meta.superseded) t += r.len;
                return t;
              }(),
              (unsigned long long)ov.meta.virtualSize);
  return 0;
}
