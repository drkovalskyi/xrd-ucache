#include "RNTupleRewrite.h"

#include "vendor/xxh3.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <map>
#include <set>
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

std::string rnTupleCodecName(int32_t compressionSettings) {
  switch (compressionSettings / 100) {
    case 0: return "none";
    case 1: return "zlib";
    case 2: return "lzma";
    case 4: return "lz4";
    case 5: return "zstd";
    default: return "";
  }
}

namespace {

// A page-list or footer envelope as stored: ZSTD at `level`, or raw when that
// does not shrink it.
std::vector<uint8_t> storeEnvelope(const std::vector<uint8_t>& env, int level) {
  auto out = encodeZstdFrames(env.data(), env.size(), level);
  if (out.empty() || out.size() >= env.size()) out = env;
  return out;
}

// Point the footer's cluster-group link at a page list: uncompressed length,
// then the locator's size and offset. Three separate fields.
bool patchFooterLink(const RNTupleMeta& m, std::vector<uint8_t>& ftr, uint64_t plLen,
                     uint64_t plStored, uint64_t plAt) {
  if (m.pageListLocatorOffset < 8 || m.pageListLocatorOffset + 12 > ftr.size()) return false;
  putLE(ftr.data() + m.pageListLocatorOffset - 8, plLen, 8);
  putLE(ftr.data() + m.pageListLocatorOffset, plStored, 4);
  putLE(ftr.data() + m.pageListLocatorOffset + 4, plAt, 8);
  return true;
}

// The anchor, patched where it lies: same size, so its enclosing key is
// untouched. It is BIG-endian, and its own checksum must be recomputed or ROOT
// refuses the file outright.
//
// ONE contiguous window from seekFooter through the checksum, rather than two
// with maxKeySize untouched between them. The bytes in the gap are rewritten to
// their existing values, which costs 8 bytes and means no read of the anchor can
// straddle an extent boundary -- a stitched reader then never has to splice this
// record out of two sources.
RNTuplePatch anchorPatch(const RNTupleMeta& m, uint64_t ftrAt, uint64_t ftrStored, uint64_t ftrLen) {
  RNTuplePatch p;
  p.offset = m.anchor.payloadOffset + 38; // seekFooter .. checksum
  p.bytes.resize(40);
  putBE(p.bytes.data(), ftrAt, 8);
  putBE(p.bytes.data() + 8, ftrStored, 8);
  putBE(p.bytes.data() + 16, ftrLen, 8);
  putBE(p.bytes.data() + 24, m.anchor.maxKeySize, 8); // unchanged, rewritten
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
  putBE(win.data() + 40, ftrStored, 8);
  putBE(win.data() + 48, ftrLen, 8);
  putBE(win.data() + 56, m.anchor.maxKeySize, 8);
  putBE(p.bytes.data() + 32, xxh3_64(win.data(), win.size()), 8);
  return p;
}

// How a build decides a range, and obtains a relocated page's new block.
//   eligible: 0 = relocate, 1 = declined by codec policy (`codec` set when
//             known), 2 = not available.
//   block:    fill `enc` with the page's new block WITHOUT its checksum; false
//             with `err` set (and `transient` when the source could not vouch).
using EligibleFn = std::function<int(size_t ri, const ColumnRange& r, std::string& codec)>;
using BlockFn = std::function<bool(size_t ri, size_t pi, const PageInfo& pg, std::vector<uint8_t>& enc,
                                   RNTupleRewrite& rw, std::string& err, bool& transient)>;

RNTupleRewrite buildImpl(const RNTupleMeta& m, uint64_t fileSize, int level,
                         const EligibleFn& eligible, const BlockFn& block) {
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
  std::vector<uint8_t> enc;

  // RNTuple SHARES pages: distinct page records can point at the same bytes
  // (identical pages are written once). Re-encoding each record separately is
  // correct but writes the duplicate, so a source page already transcoded is
  // reused — same input, same output, and the locators simply both point at it.
  std::map<std::pair<uint64_t, uint32_t>, std::pair<uint64_t, uint32_t>> seen;
  // Page offsets still referenced by a range this build did NOT relocate.
  // Sharing makes this necessary: punching a page because one range moved off
  // it would silently destroy a page another range is still serving from.
  std::set<uint64_t> live;
  std::vector<ReplicaMeta::Range> supersedeCandidates;

  for (size_t ri = 0; ri < m.ranges.size(); ++ri) {
    const auto& range = m.ranges[ri];
    // Eligibility is decided for the whole range BEFORE any transcode work,
    // because a range that turns out to be unusable half way through would
    // leave its locators half patched.
    std::string codec;
    const int e = eligible(ri, range, codec);
    if (e == 1) {
      ++rw.rangesDeclined;
      if (rw.declinedCodec.empty() && !codec.empty()) rw.declinedCodec = codec;
      for (const auto& pg : range.pages) live.insert(pg.offset);
      continue;
    }
    if (e == 2) {
      ++rw.rangesUncached;
      for (const auto& pg : range.pages) live.insert(pg.offset);
      continue; // left pointing at the original bytes
    }
    ++rw.rangesRelocated;

    for (size_t pi = 0; pi < range.pages.size(); ++pi) {
      const auto& pg = range.pages[pi];
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
      std::string err;
      bool transient = false;
      enc.clear();
      if (!block(ri, pi, pg, enc, rw, err, transient)) return fail(err, transient);
      const uint64_t want = pg.uncompressedBytes;

      std::vector<uint8_t> stored = enc;
      if (pg.hasChecksum) {
        // The checksum covers the BLOCK and sits past the locator's size, so
        // the page occupies size+8 bytes while the locator records size.
        const uint64_t h = xxh3_64(enc.data(), enc.size());
        stored.resize(enc.size() + 8);
        putLE(stored.data() + enc.size(), h, 8);
      }
      const uint64_t at = appendBlob(rw.extension, rw.extBase, stored, want);
      // The ORIGINAL extent is the block PLUS its checksum, exactly what the
      // superseded list records below. The locator's size covers the block
      // only, so mapping `nbytes` here would leave the checksum tail with no
      // original address: a read served from the replica would then report
      // fewer bytes touched than the same read served from the byte cache,
      // and only when that tail crosses a bucket boundary — which is what
      // makes it a rare, silent disagreement rather than an obvious one.
      rw.origMap.push_back({at, stored.size(), pg.offset, onDisk});

      // Repoint the locator: int32 size then uint64 offset, 4 bytes into the
      // 16-byte page record.
      putLE(pl.data() + pg.recordOffset + 4, (uint64_t)enc.size(), 4);
      putLE(pl.data() + pg.recordOffset + 8, at, 8);
      seen[{pg.offset, pg.nbytes}] = {at, (uint32_t)enc.size()};

      ++rw.pages;
      rw.oldPageBytes += onDisk;
      rw.newPageBytes += stored.size();
      supersedeCandidates.push_back({pg.offset, onDisk});
    }
    // ROOT's usual algorithm*100 + level; 5 is ZSTD.
    putLE(pl.data() + range.compressionOffset, (uint64_t)(500 + level), 4);
  }

  if (rw.rangesRelocated == 0) {
    // Nothing moved, so an overlay would be pure overhead. The counters say
    // WHY — codec policy, or nothing cached — and the caller reports that
    // rather than a bare failure.
    return fail("no eligible column ranges", rw.rangesUncached > 0 && rw.rangesDeclined == 0);
  }

  // Only pages this build actually moved off may be punched, and only when no
  // range left in place still points at them.
  for (const auto& r : supersedeCandidates)
    if (!live.count(r.off)) rw.superseded.push_back(r);

  // Re-seal the page list, then store it compressed like any other payload.
  sealEnvelope(pl.data(), pl.size());
  auto plStored = storeEnvelope(pl, level);
  const uint64_t plAt = appendBlob(rw.extension, rw.extBase, plStored, pl.size());
  if (m.pageListNbytes)
    rw.origMap.push_back({plAt, plStored.size(), m.pageListOffset, m.pageListNbytes});

  std::vector<uint8_t> ftr = m.footer;
  if (!patchFooterLink(m, ftr, pl.size(), plStored.size(), plAt))
    return fail("footer page-list locator out of bounds");
  sealEnvelope(ftr.data(), ftr.size());
  auto ftrStored = storeEnvelope(ftr, level);
  const uint64_t ftrAt = appendBlob(rw.extension, rw.extBase, ftrStored, ftr.size());
  if (m.anchor.nbytesFooter)
    rw.origMap.push_back({ftrAt, ftrStored.size(), m.anchor.seekFooter, m.anchor.nbytesFooter});

  if (m.anchor.payloadOffset == 0) return fail("compressed anchor cannot be patched in place");
  if (m.anchor.payloadLength < 78) return fail("anchor payload too short to patch");
  rw.patches.push_back(anchorPatch(m, ftrAt, ftrStored.size(), ftr.size()));

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

} // namespace

RNTupleRewrite buildRNTupleRewrite(const RNTupleMeta& m, Source& src, uint64_t fileSize, int level,
                                   const std::vector<std::string>& codecs) {
  std::vector<uint8_t> raw, buf;
  EligibleFn eligible = [&](size_t, const ColumnRange& range, std::string& codecOut) {
    if (!codecs.empty()) {
      const std::string codec = rnTupleCodecName(range.compressionSettings);
      if (std::find(codecs.begin(), codecs.end(), codec) == codecs.end()) {
        codecOut = codec;
        return 1;
      }
    }
    for (const auto& pg : range.pages)
      if (!src.has(pg.offset, (uint64_t)pg.nbytes + (pg.hasChecksum ? 8 : 0)))
        return 2;
    return 0;
  };
  BlockFn block = [&](size_t, size_t, const PageInfo& pg, std::vector<uint8_t>& enc,
                      RNTupleRewrite& rw, std::string& err, bool& transient) {
    const uint64_t onDisk = (uint64_t)pg.nbytes + (pg.hasChecksum ? 8 : 0);
    if (!src.has(pg.offset, onDisk)) {
      err = "page bytes not available";
      transient = true;
      return false;
    }
    raw.resize(pg.nbytes);
    if (pg.nbytes && !src.read(raw.data(), pg.nbytes, pg.offset)) {
      err = "page read failed";
      transient = true;
      return false;
    }
    // Verify the page against the checksum the FORMAT already carries,
    // before anything is re-encoded. Skipping this is not a theoretical
    // risk: a page stored uncompressed decodes to itself, so damaged bytes
    // pass straight through the transcoder and into a replica that then
    // serves them for ever. The checksum is right there — use it.
    if (pg.hasChecksum) {
      uint8_t ck[8];
      if (!src.read(ck, 8, pg.offset + pg.nbytes)) {
        err = "page checksum read failed";
        transient = true;
        return false;
      }
      uint64_t stored = 0;
      for (int i = 7; i >= 0; --i) stored = (stored << 8) | ck[(size_t)i];
      if (xxh3_64(raw.data(), raw.size()) != stored) {
        err = "page checksum mismatch — source bytes are damaged (run `ucache verify`)";
        return false;
      }
    }
    // The page's uncompressed size is authoritative from the schema, not
    // from the block header: a bit-packed column's byte count is a ceiling
    // over the element count and nothing on disk restates it.
    const uint64_t want = pg.uncompressedBytes;
    if (want == 0) {
      err = "page of unknown width (column not in schema)";
      return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if ((uint64_t)pg.nbytes == want) {
      buf = raw; // stored uncompressed at source
    } else {
      buf = decompressFrames(raw.data(), raw.size(), want);
      if (buf.size() != want) {
        err = "page decompression failed";
        return false;
      }
    }
    rw.decodeNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    rw.decodeBytes += want;
    enc = encodeZstdFrames(buf.data(), buf.size(), level);
    // An encoder failure is environmental (e.g. OOM), not a bad file; a
    // block that fails to shrink is stored raw, exactly as ROOT does.
    if (enc.empty() && !buf.empty()) {
      err = "zstd encode failed";
      return false;
    }
    if (enc.size() >= buf.size()) {
      enc = buf;
      ++rw.storedRaw;
    }
    return true;
  };
  return buildImpl(m, fileSize, level, eligible, block);
}

RNTupleRewrite buildRNTupleRewriteFromPages(
    const RNTupleMeta& m, uint64_t fileSize, int level,
    const std::function<bool(size_t ri, size_t pi)>& has,
    const std::function<bool(size_t ri, size_t pi, std::vector<uint8_t>& enc)>& page) {
  EligibleFn eligible = [&](size_t ri, const ColumnRange& range, std::string&) {
    for (size_t pi = 0; pi < range.pages.size(); ++pi)
      if (!has(ri, pi))
        return 2;
    return 0;
  };
  BlockFn block = [&](size_t ri, size_t pi, const PageInfo& pg, std::vector<uint8_t>& enc,
                      RNTupleRewrite& rw, std::string& err, bool& transient) {
    if (!page(ri, pi, enc)) {
      err = "converted page unavailable";
      transient = true;
      return false;
    }
    if (enc.size() > pg.uncompressedBytes) {
      err = "converted page larger than the page";
      return false;
    }
    if (enc.size() == pg.uncompressedBytes)
      ++rw.storedRaw;
    rw.decodeBytes += pg.uncompressedBytes;
    return true;
  };
  return buildImpl(m, fileSize, level, eligible, block);
}

FillLayout layoutForRNTupleFill(const RNTupleMeta& m, uint64_t fileSize,
                                const std::vector<uint8_t>& header,
                                const std::vector<std::string>& codecs) {
  FillLayout L;
  auto decline = [&](std::string why) {
    L = FillLayout();
    L.error = std::move(why);
    return L;
  };
  if (!m.error.empty()) return decline("parse: " + m.error);
  if (m.pageList.empty() || m.footer.empty()) return decline("no page list");
  if (fileSize != static_cast<uint64_t>(m.fend) || fileSize != m.fileSize)
    return decline("file size differs from fEND");
  if (m.anchor.payloadOffset == 0) return decline("compressed anchor cannot be patched in place");
  if (m.anchor.payloadLength < 78) return decline("anchor payload too short to patch");
  if (m.anchor.payloadOffset + 78 > fileSize) return decline("anchor outside the file");

  for (uint32_t ri = 0; ri < m.ranges.size(); ++ri) {
    const auto& r = m.ranges[ri];
    const std::string codec = rnTupleCodecName(r.compressionSettings);
    if (r.pages.empty() || std::find(codecs.begin(), codecs.end(), codec) == codecs.end())
      continue;
    bool ok = true;
    for (const auto& pg : r.pages)
      ok = ok && pg.uncompressedBytes > 0 && pg.uncompressedBytes <= 0x7FFFFFFFull &&
           pg.offset + pg.nbytes + (pg.hasChecksum ? 8 : 0) <= fileSize && pg.offset >= 100;
    if (ok) L.relocated.push_back(ri);
  }
  if (L.relocated.empty()) return decline("no convertible column range");

  // The page list and footer come first; their reservation is their RAW size in
  // an RBlob key each, since the patched envelopes keep their length and their
  // compressed size is not known until the slot offsets inside them are.
  L.originSize = fileSize;
  L.metaSeek = (fileSize + 4095) / 4096 * 4096;
  const uint64_t reserve = 2 * kRBlobKeyLen + m.pageList.size() + m.footer.size();
  L.slotsBegin = L.metaSeek + reserve;

  std::vector<uint8_t> pl = m.pageList;
  std::map<uint64_t, uint64_t> seen; // original page offset -> slot offset
  uint64_t at = L.slotsBegin;
  for (uint32_t ri : L.relocated) {
    const auto& r = m.ranges[ri];
    for (uint32_t pi = 0; pi < r.pages.size(); ++pi) {
      const auto& pg = r.pages[pi];
      uint64_t v;
      auto it = seen.find(pg.offset);
      if (it != seen.end()) {
        v = it->second;
      } else {
        FillSlot s;
        s.origSeek = pg.offset;
        s.origLen = pg.nbytes + (pg.hasChecksum ? 8u : 0u);
        s.vSeek = at;
        s.vLen = static_cast<uint32_t>(pg.uncompressedBytes + kSlotChecksumBytes);
        s.branch = ri;
        s.basket = pi;
        L.slots.push_back(s);
        v = at;
        seen.emplace(pg.offset, at);
        at += s.vLen;
      }
      // Negative: the page carries a checksum (the decoded page's, served after it).
      putLE(pl.data() + pg.recordOffset, static_cast<uint32_t>(-static_cast<int64_t>(pg.nElements)), 4);
      putLE(pl.data() + pg.recordOffset + 4, pg.uncompressedBytes, 4);
      putLE(pl.data() + pg.recordOffset + 8, v, 8);
    }
    putLE(pl.data() + r.compressionOffset, 0, 4);
  }
  L.virtualSize = at;

  sealEnvelope(pl.data(), pl.size());
  const auto plStored = storeEnvelope(pl, 1);
  std::vector<uint8_t> blobs;
  const uint64_t plAt = appendBlob(blobs, L.metaSeek, plStored, pl.size());
  std::vector<uint8_t> ftr = m.footer;
  if (!patchFooterLink(m, ftr, pl.size(), plStored.size(), plAt))
    return decline("footer page-list locator out of bounds");
  sealEnvelope(ftr.data(), ftr.size());
  const auto ftrStored = storeEnvelope(ftr, 1);
  const uint64_t ftrAt = appendBlob(blobs, L.metaSeek, ftrStored, ftr.size());
  if (blobs.size() > reserve) return decline("metadata outgrew its reservation");
  L.metaRecord = std::move(blobs);

  FillLayout::Window hw;
  std::string err;
  if (!headerWindowForEnd(header, L.virtualSize, hw.off, hw.bytes, err)) return decline(err);
  RNTuplePatch ap = anchorPatch(m, ftrAt, ftrStored.size(), ftr.size());
  L.windows.push_back(std::move(hw));
  L.windows.push_back({ap.offset, std::move(ap.bytes)});
  std::sort(L.windows.begin(), L.windows.end(),
            [](const FillLayout::Window& a, const FillLayout::Window& b) { return a.off < b.off; });
  if (L.windows[0].off + L.windows[0].bytes.size() > L.windows[1].off)
    return decline("anchor overlaps the file header");
  return L;
}

void sealDecodedPage(const uint8_t* page, size_t n, uint8_t out[kSlotChecksumBytes]) {
  const uint64_t h = xxh3_64(page, n);
  for (uint32_t i = 0; i < kSlotChecksumBytes; ++i) out[i] = static_cast<uint8_t>(h >> (8 * i));
}

ConvertedPage convertPage(const uint8_t* onDisk, size_t n, uint32_t nbytes, bool hasChecksum,
                          uint64_t uncompressed) {
  ConvertedPage c;
  if (n != nbytes + (hasChecksum ? 8u : 0u) || uncompressed == 0) {
    c.error = "page geometry";
    return c;
  }
  if (hasChecksum) {
    uint64_t stored = 0;
    for (int i = 7; i >= 0; --i) stored = (stored << 8) | onDisk[nbytes + (size_t)i];
    if (xxh3_64(onDisk, nbytes) != stored) {
      c.error = "page checksum mismatch";
      return c;
    }
  }
  if (nbytes == uncompressed)
    c.raw.assign(onDisk, onDisk + nbytes);
  else
    c.raw = decompressFrames(onDisk, nbytes, uncompressed);
  if (c.raw.size() != uncompressed) {
    c.error = "page decompression failed";
    c.raw.clear();
    return c;
  }
  c.enc = encodeZstdFrames(c.raw.data(), c.raw.size(), 1);
  if (c.enc.empty() || c.enc.size() >= c.raw.size()) c.enc = c.raw;
  return c;
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
  {
    // Patch windows are rewritten in place, so each IS its own original range.
    std::vector<ReplicaMeta::OrigRange> om = rw.origMap;
    for (const auto& pc : pieces)
      om.push_back({pc.virtOff, pc.bytes->size(), pc.virtOff, pc.bytes->size()});
    std::sort(om.begin(), om.end(),
              [](const ReplicaMeta::OrigRange& a, const ReplicaMeta::OrigRange& b) {
                return a.virtOff < b.virtOff;
              });
    ov.meta.origMap = std::move(om);
  }
  ov.tdata.insert(ov.tdata.end(), rw.extension.begin(), rw.extension.end());

  // Superseded: the pages the rewrite replaced, plus the page list and footer
  // it repointed away from. The HEADER envelope is not superseded — the
  // rewrite still points at it.
  std::vector<ReplicaMeta::Range> sup = rw.superseded;
  if (m.pageListNbytes) sup.push_back({m.pageListOffset, m.pageListNbytes});
  if (m.anchor.nbytesFooter) sup.push_back({m.anchor.seekFooter, m.anchor.nbytesFooter});

  // RESERVE THE FILE HEAD, even where it is genuinely superseded. A reader takes
  // the head as ONE OPAQUE BLOCK and asks for bytes it will never interpret:
  // ROOT's raw-file layer requests 4 KiB and then 128 KiB at offset 0, measured
  // identically on a 1.4 MB file and a 2.4 GB one. Reclaiming inside that window
  // buys nothing and costs a round trip — the block read spans the hole, misses,
  // refetches the whole 128 KiB from the origin, and writes the same pages back,
  // so the range ends up resident anyway with a fetch paid for it. Keeping it is
  // therefore free in the steady state, and it is what makes a warm pass over a
  // replicated entry reach origin_bytes == 0.
  //
  // The pages themselves ARE dead: their recompressed copies live in the appended
  // extent, and nothing in the rewritten file points here. That is exactly why
  // the distinction matters — deadness is a property of the bytes, and this is a
  // property of how they are asked for.
  constexpr uint64_t kHeadReserve = 128 * 1024;
  for (auto& r : sup) {
    if (r.off < kHeadReserve) {
      const uint64_t end = r.off + r.len;
      r.off = kHeadReserve;
      r.len = end > kHeadReserve ? end - kHeadReserve : 0;
    }
  }
  sup.erase(std::remove_if(sup.begin(), sup.end(),
                           [](const ReplicaMeta::Range& r) { return r.len == 0; }),
            sup.end());

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
