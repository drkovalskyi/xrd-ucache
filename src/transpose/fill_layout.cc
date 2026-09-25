// Write a TTree file out in the layout its cold replica run presents to the
// reader, so the layout can be handed to ROOT and checked. Every slot is filled
// the way a reader's request would fill it; the extension's unused bytes are
// the zeros a reader would be served.
#include "FillLayout.h"
#include "RNTupleRewrite.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

bool preadAll(int fd, void* dst, size_t n, uint64_t off) {
  return ::pread(fd, dst, n, (off_t)off) == (ssize_t)n;
}
bool writeAll(int fd, const void* src, size_t n) {
  const char* p = static_cast<const char*>(src);
  while (n) {
    ssize_t w = ::write(fd, p, n);
    if (w <= 0) return false;
    p += w;
    n -= (size_t)w;
  }
  return true;
}
bool writeZeros(int fd, uint64_t n) {
  static const std::vector<uint8_t> z(1 << 20, 0);
  while (n) {
    const size_t c = n < z.size() ? (size_t)n : z.size();
    if (!writeAll(fd, z.data(), c)) return false;
    n -= c;
  }
  return true;
}

// An RNTuple file in its cold-run layout: pages decoded into their slots.
int rntupleMain(const char* inPath, const char* outPath, const std::vector<std::string>& codecs) {
  using namespace ucache::transpose;
  RNTupleMeta m = parseRNTuple(inPath, "");
  if (!m.error.empty()) {
    std::fprintf(stderr, "parse: %s\n", m.error.c_str());
    return 1;
  }
  int in = ::open(inPath, O_RDONLY | O_CLOEXEC);
  std::vector<uint8_t> header(100);
  if (in < 0 || !preadAll(in, header.data(), header.size(), 0)) {
    std::fprintf(stderr, "cannot read %s\n", inPath);
    return 1;
  }
  FillLayout L = layoutForRNTupleFill(m, m.fileSize, header, codecs);
  if (!L.error.empty()) {
    std::fprintf(stderr, "declined: %s\n", L.error.c_str());
    return 3;
  }
  int out = ::open(outPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0)
    return 1;
  std::vector<uint8_t> buf(8 << 20);
  for (uint64_t at = 0; at < L.originSize;) {
    size_t n = (size_t)std::min<uint64_t>(buf.size(), L.originSize - at);
    if (!preadAll(in, buf.data(), n, at))
      return 1;
    for (const auto& w : L.windows) {
      const uint64_t a = std::max(w.off, at), b = std::min(w.off + w.bytes.size(), at + n);
      if (a < b) std::memcpy(buf.data() + (a - at), w.bytes.data() + (a - w.off), b - a);
    }
    if (!writeAll(out, buf.data(), n))
      return 1;
    at += n;
  }
  if (!writeZeros(out, L.metaSeek - L.originSize) || !writeAll(out, L.metaRecord.data(), L.metaRecord.size()) ||
      !writeZeros(out, L.slotsBegin - L.metaSeek - L.metaRecord.size()))
    return 1;
  unsigned long long rawBytes = 0, encBytes = 0, inBytes = 0;
  std::vector<uint8_t> rec;
  for (const auto& s : L.slots) {
    rec.resize(s.origLen);
    if (!preadAll(in, rec.data(), rec.size(), s.origSeek))
      return 1;
    const auto& pg = m.ranges[s.branch].pages[s.basket];
    ConvertedPage c = convertPage(rec.data(), rec.size(), pg.nbytes, pg.hasChecksum, pg.uncompressedBytes);
    if (!c.error.empty() || c.raw.size() != s.vLen) {
      std::fprintf(stderr, "page at %llu: %s\n", (unsigned long long)s.origSeek, c.error.c_str());
      return 1;
    }
    if (!writeAll(out, c.raw.data(), c.raw.size()))
      return 1;
    rawBytes += c.raw.size();
    encBytes += c.enc.size();
    inBytes += s.origLen;
  }
  ::close(in);
  if (::close(out) != 0)
    return 1;
  std::printf("{\"rntuple\":true,\"slots\":%zu,\"ranges_relocated\":%zu,\"ranges\":%zu,"
              "\"orig_page_bytes\":%llu,\"raw_bytes\":%llu,\"zstd1_bytes\":%llu,"
              "\"origin_size\":%llu,\"virtual_size\":%llu,\"header_promoted\":%s}\n",
              L.slots.size(), L.relocated.size(), m.ranges.size(), inBytes, rawBytes, encBytes,
              (unsigned long long)L.originSize, (unsigned long long)L.virtualSize,
              L.windows.front().off == 0 ? "true" : "false");
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  using namespace ucache::transpose;
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s IN.root OUT.root [slot-factor] [codecs] [tree]\n", argv[0]);
    return 2;
  }
  // The slot factor as a number (4, 2.5), handed to the layout in hundredths.
  const uint32_t k =
      argc > 3 ? static_cast<uint32_t>(std::llround(std::strtod(argv[3], nullptr) * 100.0)) : 400;
  std::vector<std::string> codecs;
  {
    std::string s = argc > 4 ? argv[4] : "lzma,zlib", cur;
    for (char c : s + ",")
      if (c == ',') {
        if (!cur.empty()) codecs.push_back(cur);
        cur.clear();
      } else
        cur += c;
  }
  const std::string tree = argc > 5 ? argv[5] : "Events";

  const auto t0 = std::chrono::steady_clock::now();
  FileMeta fm = parseFile(argv[1], tree);
  if (!fm.error.empty() && fm.error.find("not found") != std::string::npos)
    return rntupleMain(argv[1], argv[2], codecs); // no such TTree: perhaps an RNTuple
  if (!fm.error.empty()) {
    std::fprintf(stderr, "parse: %s\n", fm.error.c_str());
    return 1;
  }
  int in = ::open(argv[1], O_RDONLY | O_CLOEXEC);
  struct stat st {};
  if (in < 0 || ::fstat(in, &st) != 0) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  std::vector<uint8_t> header(100);
  std::vector<uint8_t> treeKeyHeader(fm.treeKey.keylen);
  if (!preadAll(in, header.data(), header.size(), 0) ||
      !preadAll(in, treeKeyHeader.data(), treeKeyHeader.size(), (uint64_t)fm.treeKey.seekkey)) {
    std::fprintf(stderr, "short read on header\n");
    return 1;
  }
  std::vector<uint8_t> kl(4);
  if (!preadAll(in, kl.data(), 4, (uint64_t)fm.keyslistSeek)) {
    std::fprintf(stderr, "short read on keys list\n");
    return 1;
  }
  const int32_t klLen = (int32_t)((uint32_t)kl[0] << 24 | (uint32_t)kl[1] << 16 | (uint32_t)kl[2] << 8 | kl[3]);
  if (klLen <= 0) {
    std::fprintf(stderr, "keys list length %d\n", klLen);
    return 1;
  }
  kl.resize((size_t)klLen);
  if (!preadAll(in, kl.data(), kl.size(), (uint64_t)fm.keyslistSeek)) {
    std::fprintf(stderr, "short read on keys list\n");
    return 1;
  }
  FillLayout L = layoutForFill(fm, (uint64_t)st.st_size, header, treeKeyHeader, kl, codecs, k);
  const double setupMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!L.error.empty()) {
    std::fprintf(stderr, "declined: %s\n", L.error.c_str());
    return 3;
  }

  int out = ::open(argv[2], O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    std::fprintf(stderr, "cannot create %s\n", argv[2]);
    return 1;
  }
  // The original bytes with the windows applied.
  {
    std::vector<uint8_t> buf(8 << 20);
    uint64_t at = 0;
    while (at < L.originSize) {
      size_t n = (size_t)std::min<uint64_t>(buf.size(), L.originSize - at);
      if (!preadAll(in, buf.data(), n, at)) {
        std::fprintf(stderr, "short read at %llu\n", (unsigned long long)at);
        return 1;
      }
      for (const auto& win : L.windows) {
        const uint64_t a = std::max(win.off, at), b = std::min(win.off + win.bytes.size(), at + n);
        if (a < b) std::memcpy(buf.data() + (a - at), win.bytes.data() + (a - win.off), b - a);
      }
      if (!writeAll(out, buf.data(), n)) return 1;
      at += n;
    }
  }
  if (!writeZeros(out, L.metaSeek - L.originSize) || !writeAll(out, L.metaRecord.data(), L.metaRecord.size()) ||
      !writeZeros(out, L.slotsBegin - L.metaSeek - L.metaRecord.size())) {
    std::fprintf(stderr, "write failed\n");
    return 1;
  }

  unsigned long long nZstd = 0, nRaw = 0, nOrig = 0, origBytes = 0, storedBytes = 0, origKeptBytes = 0,
                     compressedOrigKept = 0;
  double convertMs = 0;
  std::vector<uint8_t> rec, slot;
  for (const FillSlot& s : L.slots) {
    rec.resize(s.origLen);
    if (!preadAll(in, rec.data(), rec.size(), s.origSeek)) {
      std::fprintf(stderr, "short read on basket at %llu\n", (unsigned long long)s.origSeek);
      return 1;
    }
    const auto c0 = std::chrono::steady_clock::now();
    ConvertedBasket c = convertBasket(rec.data(), rec.size(), s.vLen, codecs);
    convertMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    if (!c.error.empty()) {
      std::fprintf(stderr, "basket at %llu: %s\n", (unsigned long long)s.origSeek, c.error.c_str());
      return 1;
    }
    slot.assign(s.vLen, 0xAA);
    std::string err;
    if (!placeInSlot(c, s, slot.data(), err)) {
      std::fprintf(stderr, "slot at %llu: %s\n", (unsigned long long)s.vSeek, err.c_str());
      return 1;
    }
    if (!writeAll(out, slot.data(), slot.size())) return 1;
    origBytes += s.origLen;
    storedBytes += c.record.size();
    switch (c.kind) {
    case ConvertedBasket::kZstd: ++nZstd; break;
    case ConvertedBasket::kRaw: ++nRaw; break;
    case ConvertedBasket::kOriginal:
      ++nOrig;
      origKeptBytes += s.origLen;
      if (!c.codec.empty()) compressedOrigKept += s.origLen; // listed codec that did not fit
      break;
    }
  }
  ::close(in);
  if (::close(out) != 0) return 1;
  std::printf("{\"slots\":%zu,\"branches_relocated\":%zu,\"branches\":%zu,\"zstd\":%llu,\"raw\":%llu,"
              "\"original\":%llu,\"orig_basket_bytes\":%llu,\"stored_bytes\":%llu,"
              "\"original_kept_bytes\":%llu,\"listed_codec_overflow_bytes\":%llu,"
              "\"origin_size\":%llu,\"virtual_size\":%llu,\"meta_seek\":%llu,\"meta_record\":%zu,"
              "\"tree_blob\":%zu,\"header_promoted\":%s,\"setup_ms\":%.1f,\"convert_ms\":%.1f}\n",
              L.slots.size(), L.relocated.size(), fm.branches.size(), nZstd, nRaw, nOrig, origBytes,
              storedBytes, origKeptBytes, compressedOrigKept, (unsigned long long)L.originSize,
              (unsigned long long)L.virtualSize, (unsigned long long)L.metaSeek, L.metaRecord.size(),
              fm.treeBlob.size(), L.windows.front().off == 0 ? "true" : "false", setupMs, convertMs);
  return 0;
}
