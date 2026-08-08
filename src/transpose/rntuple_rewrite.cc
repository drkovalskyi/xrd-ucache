// Recompress an RNTuple file's pages and write a standalone result, so the
// transcode can be handed to ROOT and checked. ROOT is the arbiter here: a
// reader that is wrong but self-consistent round-trips through our own code
// perfectly and is rejected only when ROOT opens the file.
#include "RNTupleRewrite.h"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace {

// Plain-file byte source: every byte is present, which is the difference
// between rewriting a file and rewriting a partially cached entry.
class FileSource : public ucache::transpose::Source {
public:
  explicit FileSource(int fd, uint64_t size) : fd_(fd), size_(size) {}
  bool read(void* dst, uint64_t n, uint64_t off) override {
    return ::pread(fd_, dst, n, (off_t)off) == (ssize_t)n;
  }
  bool has(uint64_t off, uint64_t n) override { return off + n <= size_ && off + n >= off; }

private:
  int fd_;
  uint64_t size_;
};

} // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s IN.root OUT.root [zstd-level]\n", argv[0]);
    return 2;
  }
  const int level = argc > 3 ? std::atoi(argv[3]) : 1;

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
  auto rw = ucache::transpose::buildRNTupleRewrite(m, src, m.fileSize, level);
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
  std::printf("{\"pages\":%llu,\"stored_raw\":%llu,\"page_bytes\":[%llu,%llu],"
              "\"ratio\":%.4f,\"extension_bytes\":%llu,\"decoded_bytes\":%llu,"
              "\"decode_ms\":%.1f}\n",
              (unsigned long long)rw.pages, (unsigned long long)rw.storedRaw,
              (unsigned long long)rw.oldPageBytes, (unsigned long long)rw.newPageBytes,
              rw.oldPageBytes ? (double)rw.newPageBytes / (double)rw.oldPageBytes : 0.0,
              (unsigned long long)rw.extension.size(), (unsigned long long)rw.decodeBytes,
              rw.decodeNs / 1e6);
  return 0;
}
