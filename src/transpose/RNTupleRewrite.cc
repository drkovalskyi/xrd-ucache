#include "RNTupleRewrite.h"

#include "vendor/xxh3.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <utility>
#include <unistd.h>

namespace ucache::transpose {
namespace {

void putLE(uint8_t* p, uint64_t v, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
}
void putBE(uint8_t* p, uint64_t v, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) p[i] = (uint8_t)((v >> (8 * (bytes - 1 - i))) & 0xFF);
}
void appendBE(std::vector<uint8_t>& out, uint64_t v, size_t bytes) {
  const size_t at = out.size();
  out.resize(at + bytes);
  putBE(out.data() + at, v, bytes);
}

// A minimal RBlob key: the wrapper every relocated payload lives in, and the
// same layout parseKey reads back. Version 1004 selects 64-bit seeks, so an
// appended key stays valid however large the file grows.
constexpr uint16_t kRBlobKeyVersion = 1004;
constexpr size_t kRBlobKeyLen = 4 + 2 + 4 + 4 + 2 + 2 + 8 + 8 + (1 + 5) + 1 + 1;

void appendRBlobKey(std::vector<uint8_t>& out, uint64_t seek, size_t payloadBytes,
                    uint64_t objlen) {
  appendBE(out, kRBlobKeyLen + payloadBytes, 4); // fNbytes
  appendBE(out, kRBlobKeyVersion, 2);
  appendBE(out, objlen, 4);
  appendBE(out, 0, 4); // fDatime
  appendBE(out, kRBlobKeyLen, 2);
  appendBE(out, 1, 2); // cycle
  appendBE(out, seek, 8);
  appendBE(out, 100, 8); // parent directory
  out.push_back(5);
  const char* cls = "RBlob";
  out.insert(out.end(), cls, cls + 5);
  out.push_back(0); // name
  out.push_back(0); // title
}

// Append `payload` inside a fresh RBlob key and return the absolute offset of
// the payload — which is what a locator points at, past the key header.
uint64_t appendBlob(std::vector<uint8_t>& ext, uint64_t extBase, const std::vector<uint8_t>& payload,
                    uint64_t objlen) {
  const uint64_t seek = extBase + ext.size();
  appendRBlobKey(ext, seek, payload.size(), objlen);
  const uint64_t payloadAt = extBase + ext.size();
  ext.insert(ext.end(), payload.begin(), payload.end());
  return payloadAt;
}

} // namespace

RNTupleRewrite buildRNTupleRewrite(const RNTupleMeta& m, Source& src, uint64_t fileSize,
                                   int level) {
  RNTupleRewrite rw;
  rw.extBase = fileSize;
  auto fail = [&](std::string why, bool transient = false) {
    rw.error = std::move(why);
    rw.transient = transient;
    rw.extension.clear();
    rw.patches.clear();
    return rw;
  };
  if (m.pageList.empty() || m.footer.empty()) return fail("no page list to rewrite");

  std::vector<uint8_t> pl = m.pageList; // patched copy
  std::vector<uint8_t> buf, raw;

  // RNTuple SHARES pages: distinct page records can point at the same bytes
  // (identical pages are written once). Re-encoding each record separately is
  // correct but writes the duplicate, so a source page already transcoded is
  // reused — same input, same output, and the locators simply both point at it.
  std::map<std::pair<uint64_t, uint32_t>, std::pair<uint64_t, uint32_t>> seen;

  for (const auto& range : m.ranges) {
    for (const auto& pg : range.pages) {
      {
        auto it = seen.find({pg.offset, pg.nbytes});
        if (it != seen.end()) {
          putLE(pl.data() + pg.recordOffset + 4, it->second.second, 4);
          putLE(pl.data() + pg.recordOffset + 8, it->second.first, 8);
          ++rw.pages;
          ++rw.sharedPages;
          continue;
        }
      }
      const uint64_t onDisk = (uint64_t)pg.nbytes + (pg.hasChecksum ? 8 : 0);
      if (!src.has(pg.offset, onDisk))
        return fail("page bytes not available", true);
      raw.resize(pg.nbytes);
      if (pg.nbytes && !src.read(raw.data(), pg.nbytes, pg.offset))
        return fail("page read failed", true);

      // The page's uncompressed size is authoritative from the schema, not
      // from the block header: a bit-packed column's byte count is a ceiling
      // over the element count and nothing on disk restates it.
      const uint64_t want = pg.uncompressedBytes;
      if (want == 0) return fail("page of unknown width (column not in schema)");

      const auto t0 = std::chrono::steady_clock::now();
      if ((uint64_t)pg.nbytes == want) {
        buf = raw; // stored uncompressed at source
      } else {
        buf = decompressFrames(raw.data(), raw.size(), want);
        if (buf.size() != want) return fail("page decompression failed");
      }
      rw.decodeNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
      rw.decodeBytes += want;

      auto enc = encodeZstdFrames(buf.data(), buf.size(), level);
      // An encoder failure is environmental (e.g. OOM), not a bad file; a
      // block that fails to shrink is stored raw, exactly as ROOT does.
      if (enc.empty() && !buf.empty()) return fail("zstd encode failed");
      const bool rawStore = enc.size() >= buf.size();
      if (rawStore) {
        enc = buf;
        ++rw.storedRaw;
      }

      std::vector<uint8_t> stored = enc;
      if (pg.hasChecksum) {
        // The checksum covers the BLOCK and sits past the locator's size, so
        // the page occupies size+8 bytes while the locator records size.
        const uint64_t h = xxh3_64(enc.data(), enc.size());
        stored.resize(enc.size() + 8);
        putLE(stored.data() + enc.size(), h, 8);
      }
      const uint64_t at = appendBlob(rw.extension, rw.extBase, stored, want);

      // Repoint the locator: int32 size then uint64 offset, 4 bytes into the
      // 16-byte page record.
      putLE(pl.data() + pg.recordOffset + 4, (uint64_t)enc.size(), 4);
      putLE(pl.data() + pg.recordOffset + 8, at, 8);
      seen[{pg.offset, pg.nbytes}] = {at, (uint32_t)enc.size()};

      ++rw.pages;
      rw.oldPageBytes += onDisk;
      rw.newPageBytes += stored.size();
    }
    // ROOT's usual algorithm*100 + level; 5 is ZSTD.
    putLE(pl.data() + range.compressionOffset, (uint64_t)(500 + level), 4);
  }

  // Re-seal the page list, then store it compressed like any other payload.
  sealEnvelope(pl.data(), pl.size());
  auto plStored = encodeZstdFrames(pl.data(), pl.size(), level);
  if (plStored.empty() || plStored.size() >= pl.size()) plStored = pl;
  const uint64_t plAt = appendBlob(rw.extension, rw.extBase, plStored, pl.size());

  // Point the footer's cluster-group link at the new page list: uncompressed
  // length, then the locator's size and offset. Three separate fields.
  std::vector<uint8_t> ftr = m.footer;
  if (m.pageListLocatorOffset < 8 || m.pageListLocatorOffset + 12 > ftr.size())
    return fail("footer page-list locator out of bounds");
  putLE(ftr.data() + m.pageListLocatorOffset - 8, pl.size(), 8);
  putLE(ftr.data() + m.pageListLocatorOffset, plStored.size(), 4);
  putLE(ftr.data() + m.pageListLocatorOffset + 4, plAt, 8);
  sealEnvelope(ftr.data(), ftr.size());
  auto ftrStored = encodeZstdFrames(ftr.data(), ftr.size(), level);
  if (ftrStored.empty() || ftrStored.size() >= ftr.size()) ftrStored = ftr;
  const uint64_t ftrAt = appendBlob(rw.extension, rw.extBase, ftrStored, ftr.size());

  // The anchor is patched where it lies: same size, so its enclosing key is
  // untouched. It is BIG-endian, and its own checksum must be recomputed or
  // ROOT refuses the file outright.
  if (m.anchor.payloadOffset == 0) return fail("compressed anchor cannot be patched in place");
  if (m.anchor.payloadLength < 78) return fail("anchor payload too short to patch");
  {
    RNTuplePatch p;
    p.offset = m.anchor.payloadOffset + 38; // seekFooter
    p.bytes.resize(24);
    putBE(p.bytes.data(), ftrAt, 8);
    putBE(p.bytes.data() + 8, ftrStored.size(), 8);
    putBE(p.bytes.data() + 16, ftr.size(), 8);

    // Rebuild the checksummed window (payload[6:70]) to hash it.
    std::vector<uint8_t> win(64);
    putBE(win.data(), m.anchor.versionEpoch, 2);
    putBE(win.data() + 2, m.anchor.versionMajor, 2);
    putBE(win.data() + 4, m.anchor.versionMinor, 2);
    putBE(win.data() + 6, m.anchor.versionPatch, 2);
    putBE(win.data() + 8, m.anchor.seekHeader, 8);
    putBE(win.data() + 16, m.anchor.nbytesHeader, 8);
    putBE(win.data() + 24, m.anchor.lenHeader, 8);
    putBE(win.data() + 32, ftrAt, 8);
    putBE(win.data() + 40, ftrStored.size(), 8);
    putBE(win.data() + 48, ftr.size(), 8);
    putBE(win.data() + 56, m.anchor.maxKeySize, 8);
    RNTuplePatch ck;
    ck.offset = m.anchor.payloadOffset + 70;
    ck.bytes.resize(8);
    putBE(ck.bytes.data(), xxh3_64(win.data(), win.size()), 8);
    rw.patches.push_back(std::move(p));
    rw.patches.push_back(std::move(ck));
  }

  // fEND must cover the appended data or ROOT truncates its view of the file.
  // It is patched at ITS OWN width; a narrow header that the rewrite would
  // push past 2 GB cannot be widened in place, so refuse rather than truncate.
  {
    const uint64_t newEnd = rw.extBase + rw.extension.size();
    if (m.headerSeekWidth == 4 && newEnd > 0x7FFFFFFFull)
      return fail("rewrite would push fEND past 2 GB under a 32-bit header");
    RNTuplePatch p;
    p.offset = 12;
    p.bytes.resize((size_t)m.headerSeekWidth);
    putBE(p.bytes.data(), newEnd, (size_t)m.headerSeekWidth);
    rw.patches.push_back(std::move(p));
  }

  return rw;
}

Overlay rnTupleOverlay(const RNTupleMeta& m, const RNTupleRewrite& rw) {
  Overlay ov;
  if (!rw.error.empty()) {
    ov.error = rw.error;
    ov.transient = rw.transient;
    return ov;
  }

  // Patches first, then the extension: extents must be sorted by virtual
  // offset and non-overlapping, and .tdata is concatenated in the same order.
  struct Piece {
    uint64_t virtOff;
    const std::vector<uint8_t>* bytes;
  };
  std::vector<Piece> pieces;
  for (const auto& p : rw.patches) pieces.push_back({p.offset, &p.bytes});
  std::sort(pieces.begin(), pieces.end(),
            [](const Piece& a, const Piece& b) { return a.virtOff < b.virtOff; });
  for (size_t i = 1; i < pieces.size(); ++i) {
    if (pieces[i].virtOff < pieces[i - 1].virtOff + pieces[i - 1].bytes->size()) {
      ov.error = "overlapping patch windows";
      return ov;
    }
  }

  uint64_t tdataAt = 0;
  for (const auto& pc : pieces) {
    ov.tdata.insert(ov.tdata.end(), pc.bytes->begin(), pc.bytes->end());
    ov.meta.extents.push_back({pc.virtOff, pc.bytes->size(), tdataAt});
    tdataAt += pc.bytes->size();
  }
  ov.meta.extents.push_back({rw.extBase, rw.extension.size(), tdataAt});
  ov.tdata.insert(ov.tdata.end(), rw.extension.begin(), rw.extension.end());

  // Superseded: the pages the rewrite replaced, plus the page list and footer
  // it repointed away from. The HEADER envelope is not superseded — the
  // rewrite still points at it.
  std::vector<ReplicaMeta::Range> sup;
  for (const auto& range : m.ranges)
    for (const auto& pg : range.pages)
      sup.push_back({pg.offset, (uint64_t)pg.nbytes + (pg.hasChecksum ? 8 : 0)});
  if (m.pageListNbytes) sup.push_back({m.pageListOffset, m.pageListNbytes});
  if (m.anchor.nbytesFooter) sup.push_back({m.anchor.seekFooter, m.anchor.nbytesFooter});

  // Merge, so reclaim is a few large punches rather than one per page.
  std::sort(sup.begin(), sup.end(),
            [](const auto& a, const auto& b) { return a.off < b.off; });
  std::vector<ReplicaMeta::Range> merged;
  for (const auto& r : sup) {
    if (!merged.empty() && r.off <= merged.back().off + merged.back().len) {
      const uint64_t end = std::max(merged.back().off + merged.back().len, r.off + r.len);
      merged.back().len = end - merged.back().off;
    } else {
      merged.push_back(r);
    }
  }
  ov.meta.superseded = std::move(merged);
  ov.meta.virtualSize = rw.extBase + rw.extension.size();
  ov.meta.originSize = m.fileSize;
  ov.meta.encoding = ReplicaMeta::kZstd1;
  ov.baskets = rw.pages; // pages here; the accounting field is shared
  ov.transcoded = rw.pages - rw.storedRaw;
  ov.fallbackRaw = rw.storedRaw;
  ov.oldBytes = rw.oldPageBytes;
  ov.newBytes = rw.newPageBytes;
  ov.decodeBytes = rw.decodeBytes;
  ov.decodeNs = rw.decodeNs;
  return ov;
}

bool writeRewrittenRNTuple(const std::string& srcPath, const std::string& dstPath,
                           const RNTupleRewrite& rw, std::string& error) {
  int in = ::open(srcPath.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) {
    error = "cannot open " + srcPath;
    return false;
  }
  int out = ::open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    ::close(in);
    error = "cannot create " + dstPath;
    return false;
  }
  std::vector<uint8_t> buf(1 << 20);
  uint64_t at = 0;
  bool ok = true;
  while (at < rw.extBase) {
    const size_t want = (size_t)std::min<uint64_t>(buf.size(), rw.extBase - at);
    if (::pread(in, buf.data(), want, (off_t)at) != (ssize_t)want ||
        ::pwrite(out, buf.data(), want, (off_t)at) != (ssize_t)want) {
      ok = false;
      error = "copy failed";
      break;
    }
    at += want;
  }
  if (ok && !rw.extension.empty() &&
      ::pwrite(out, rw.extension.data(), rw.extension.size(), (off_t)rw.extBase) !=
          (ssize_t)rw.extension.size()) {
    ok = false;
    error = "extension write failed";
  }
  for (const auto& p : rw.patches) {
    if (!ok) break;
    if (::pwrite(out, p.bytes.data(), p.bytes.size(), (off_t)p.offset) != (ssize_t)p.bytes.size()) {
      ok = false;
      error = "patch write failed";
    }
  }
  ::close(in);
  ::close(out);
  return ok;
}

} // namespace ucache::transpose
