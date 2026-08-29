#include "Transposer.h"

#include <algorithm>
#include <cstring>
#include <set>

#include <utility>
#include <zstd.h>

namespace ucache::transpose {
namespace {

constexpr uint64_t kMaxFrame = 0xFFFFFF; // 16 MiB - 1, ROOT per-frame limit

// Relocated tree-metadata ZSTD level at rest. Chosen by
// a measured level sweep: highest level under the 1.5× sweep cap;
// on real NanoAOD metadata ZSTD-9 reaches R≈6.7 (level 1 only ≈5.1, level 19
// blows the sweep budget). Baskets stay level 1 (a bump buys <1%).
constexpr int kMetaZstdLevel = 9;

template <typename T> void bePut(uint8_t* p, T v) {
  using U = std::make_unsigned_t<T>;
  U u;
  std::memcpy(&u, &v, sizeof(T));
  for (size_t i = sizeof(T); i-- > 0;) {
    p[i] = static_cast<uint8_t>(u & 0xFF);
    u >>= 8;
  }
}

// raw bytes -> ROOT multi-frame ZSTD container ('ZS\x01' framing, exactly the
// Python builder's zstd_frames — zstd versions are pinned equal, so the
// byte-identical differential still holds as an advisory, and the semantic
// digest holds regardless. The method byte stays 1 at every level
// (ROOT keys decompression on the 'ZS' magic, not the method byte).
std::vector<uint8_t> zstdFrames(const uint8_t* raw, size_t n, int level) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < n; i += kMaxFrame) {
    size_t chunk = std::min<size_t>(kMaxFrame, n - i);
    size_t cap = ZSTD_compressBound(chunk);
    size_t pre = out.size();
    out.resize(pre + 9 + cap);
    size_t got = ZSTD_compress(out.data() + pre + 9, cap, raw + i, chunk, level);
    if (ZSTD_isError(got))
      return {};
    out.resize(pre + 9 + got);
    uint8_t* h = out.data() + pre;
    h[0] = 'Z';
    h[1] = 'S';
    h[2] = 1;
    h[3] = got & 0xFF;
    h[4] = (got >> 8) & 0xFF;
    h[5] = (got >> 16) & 0xFF;
    h[6] = chunk & 0xFF;
    h[7] = (chunk >> 8) & 0xFF;
    h[8] = (chunk >> 16) & 0xFF;
  }
  return out;
}

void patchSeek(uint8_t* rec, uint16_t ver, int64_t seek, std::string& err) {
  if (ver > 1000) {
    bePut<int64_t>(rec + 18, seek);
  } else if (seek < (1ll << 31)) {
    bePut<int32_t>(rec + 18, static_cast<int32_t>(seek));
  } else {
    err = "32-bit key cannot point past 2 GiB";
  }
}

} // namespace

// Every basket of `b` fully present in `src`?
bool fullyCached(const BranchInfo& b, Source& src) {
  if (b.writeBasket <= 0)
    return false;
  for (int32_t i = 0; i < b.writeBasket; ++i)
    if (!src.has(static_cast<uint64_t>(b.basketSeek[i]),
                 static_cast<uint64_t>(b.basketBytes[i])))
      return false;
  return true;
}

std::vector<std::string> withCounters(const FileMeta& fm,
                                      const std::vector<std::string>& names, Source& src) {
  std::vector<std::string> out = names;
  std::set<std::string> have(names.begin(), names.end());
  for (const auto& n : names)
    for (const auto& b : fm.branches)
      if (b.name == n && !b.leafCount.empty() && !have.count(b.leafCount)) {
        // A counter rides along only if it is buildable: its baskets must be
        // fully cached too. Riding it along unchecked made buildOverlay fail
        // the WHOLE file on a partially-read counter (real-cache
        // finding) — leaving it out is safe: counter bytes still serve from
        // the ordinary v1 path; inclusion is an optimization, not a need.
        for (const auto& c : fm.branches)
          if (c.name == b.leafCount && fullyCached(c, src)) {
            out.push_back(b.leafCount);
            have.insert(b.leafCount);
            break;
          }
        have.insert(b.leafCount); // don't re-probe per dependent branch
      }
  return out;
}

std::string branchCodec(const FileMeta& fm, const BranchInfo& b, Source& src) {
  (void)fm;
  if (b.writeBasket <= 0)
    return "";
  const uint64_t off = static_cast<uint64_t>(b.basketSeek[0]);
  const uint64_t nb = static_cast<uint64_t>(b.basketBytes[0]);
  uint8_t head[512];
  const uint64_t want = std::min<uint64_t>(nb, sizeof head);
  if (!src.has(off, want) || !src.read(head, want, off))
    return "";
  auto k = parseKey(head, static_cast<size_t>(want), 0); // head[0] IS the key start
  if (!k || k->keylen + 2u > want)
    return "";
  // Uncompressed basket: payload == object bytes.
  if (static_cast<uint64_t>(k->objlen) + k->keylen == static_cast<uint64_t>(k->nbytes))
    return "none";
  const char m0 = static_cast<char>(head[k->keylen]), m1 = static_cast<char>(head[k->keylen + 1]);
  if (m0 == 'X' && m1 == 'Z')
    return "lzma";
  if (m0 == 'Z' && m1 == 'L')
    return "zlib";
  if (m0 == 'Z' && m1 == 'S')
    return "zstd";
  if (m0 == 'L' && m1 == '4')
    return "lz4";
  return "";
}

uint64_t hotUncompressedBytes(const FileMeta& fm, const std::vector<std::string>& hot,
                              Source& src) {
  uint64_t total = 0;
  for (const auto& name : hot)
    for (const auto& b : fm.branches) {
      if (b.name != name)
        continue;
      for (int32_t i = 0; i < b.writeBasket; ++i) {
        const uint64_t off = static_cast<uint64_t>(b.basketSeek[i]);
        const uint64_t nb = static_cast<uint64_t>(b.basketBytes[i]);
        uint8_t head[64];
        const uint64_t want = std::min<uint64_t>(nb, sizeof head);
        if (!src.has(off, want) || !src.read(head, want, off))
          continue;
        if (auto k = parseKey(head, static_cast<size_t>(want), 0))
          total += static_cast<uint64_t>(k->objlen);
      }
      break;
    }
  return total;
}

std::vector<std::string> deriveHotBranches(const FileMeta& fm, Source& src,
                                           const std::vector<std::string>& codecs) {
  std::vector<std::string> hot;
  for (const auto& b : fm.branches) {
    if (!fullyCached(b, src))
      continue;
    if (!codecs.empty()) { // recompress_codecs filter: only listed
      std::string c = branchCodec(fm, b, src); // source codecs are worth
      bool listed = false;                     // paying the transcode for
      for (const auto& want : codecs)
        if (c == want) {
          listed = true;
          break;
        }
      if (!listed)
        continue;
    }
    hot.push_back(b.name);
  }
  return withCounters(fm, hot, src);
}

std::vector<std::string> deriveHotBranches(const FileMeta& fm, Source& src) {
  return deriveHotBranches(fm, src, {});
}

Overlay buildOverlay(const FileMeta& fm, Source& src, const std::vector<std::string>& hot) {
  Overlay ov;
  auto fail = [&](std::string why) {
    ov.error = std::move(why);
    return ov;
  };
  // The source could not vouch for bytes we need: retryable, not a verdict on
  // the file (see Overlay::transient).
  auto unavailable = [&](std::string why) {
    ov.transient = true;
    ov.error = std::move(why);
    return ov;
  };
  if (!fm.error.empty())
    return fail("parse: " + fm.error);

  std::vector<uint8_t> blob = fm.treeBlob; // patched in place below
  std::vector<uint8_t> ext;
  const int64_t extBase = fm.fend;
  // Where each relocated piece came from in the ORIGINAL file. Serving never
  // consults this; it exists so a read of the replica can be named in the
  // coordinates the origin and the byte tier use, which is what lets the same
  // analysis over two tiers be recognised as the same work.
  std::vector<ReplicaMeta::OrigRange> origMap;
  std::vector<ReplicaMeta::Range> superseded;

  for (const auto& name : hot) {
    const BranchInfo* b = nullptr;
    for (const auto& x : fm.branches)
      if (x.name == name) {
        b = &x;
        break;
      }
    if (!b)
      return fail("hot branch not in tree: " + name);
    for (int32_t i = 0; i < b->writeBasket; ++i) {
      const uint64_t off = static_cast<uint64_t>(b->basketSeek[i]);
      const uint64_t nb = static_cast<uint64_t>(b->basketBytes[i]);
      if (!src.has(off, nb))
        return unavailable(name + ": basket " + std::to_string(i) + " not available");
      std::vector<uint8_t> rec(nb);
      // A verifying source also fails here when the bytes were reclaimed or
      // rotted after the coverage check above — same retryable condition.
      if (!src.read(rec.data(), nb, off))
        return unavailable(name + ": basket " + std::to_string(i) + " no longer readable");
      auto k = parseKey(rec.data(), rec.size(), 0);
      if (!k || k->cls != "TBasket" || k->name != name)
        return fail(name + ": unexpected basket key");
      const bool storedRaw =
          k->objlen == k->nbytes - static_cast<int32_t>(k->keylen);
      if (storedRaw) {
        ++ov.verbatim;
      } else {
        struct timespec t0, t1;
        ::clock_gettime(CLOCK_MONOTONIC, &t0);
        auto raw = decompressFrames(rec.data() + k->keylen, rec.size() - k->keylen,
                                    static_cast<uint64_t>(k->objlen));
        if (raw.empty())
          return fail(name + ": basket decompression failed");
        ::clock_gettime(CLOCK_MONOTONIC, &t1);
        ov.decodeBytes += raw.size();
        ov.decodeNs += static_cast<uint64_t>(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                       static_cast<uint64_t>(t1.tv_nsec - t0.tv_nsec);
        auto pay = zstdFrames(raw.data(), raw.size(), 1);
        if (pay.empty()) // an encoder failure is environmental (e.g. OOM), so
                         // the file is still buildable on a later pass
          return unavailable(name + ": zstd encode failed");
        if (pay.size() >= raw.size()) { // must shrink below fObjlen, else raw
          pay = std::move(raw);
          ++ov.fallbackRaw;
        } else {
          ++ov.transcoded;
        }
        rec.resize(k->keylen);
        rec.insert(rec.end(), pay.begin(), pay.end());
        bePut<int32_t>(rec.data(), static_cast<int32_t>(rec.size())); // fNbytes
      }
      const int64_t newSeek = extBase + static_cast<int64_t>(ext.size());
      patchSeek(rec.data(), k->ver, newSeek, ov.error);
      if (!ov.error.empty())
        return ov;
      // Patch the live arrays inside the metadata blob (exact offsets from
      // the parser — the Python builder needed value-pattern search here).
      bePut<int64_t>(blob.data() + b->seekArrayOff + 8 * i, newSeek);
      bePut<int32_t>(blob.data() + b->bytesArrayOff + 4 * i,
                     static_cast<int32_t>(rec.size()));
      superseded.push_back({off, nb});
      origMap.push_back({static_cast<uint64_t>(newSeek), rec.size(), off, nb});
      ov.oldBytes += nb;
      ov.newBytes += rec.size();
      ++ov.baskets;
      ext.insert(ext.end(), rec.begin(), rec.end());
    }
  }

  // Relocated metadata key: ROOT-compress the (patched) blob
  // at rest with kMetaZstdLevel. fObjlen in the key header is left at the
  // decompressed size (copied from the original key), so ROOT detects the
  // compressed record (fObjlen > fNbytes − fKeylen) and inflates it exactly as
  // it does the original LZMA key. fNbytes (here and in the keys-list entry
  // below) becomes keylen + stored size. Raw fallback if it does not shrink
  // (never on real metadata; keeps the writer honest). Serving is unchanged —
  // .tdata stays opaque CRC'd bytes, .tmeta format_version stays 1.
  {
    std::vector<uint8_t> mkey(fm.treeKey.keylen);
    if (!src.has(fm.treeKey.seekkey, fm.treeKey.nbytes) ||
        !src.read(mkey.data(), mkey.size(), fm.treeKey.seekkey))
      return unavailable("tree key header not available");
    ov.metaRawBytes = blob.size();
    std::vector<uint8_t> metaPayload = zstdFrames(blob.data(), blob.size(), kMetaZstdLevel);
    if (metaPayload.empty() || metaPayload.size() >= blob.size())
      metaPayload = blob; // raw fallback (encode error or non-shrinking)
    ov.metaStoredBytes = metaPayload.size();
    const int64_t newSeek = extBase + static_cast<int64_t>(ext.size());
    bePut<int32_t>(mkey.data(),
                   static_cast<int32_t>(fm.treeKey.keylen + metaPayload.size()));
    patchSeek(mkey.data(), fm.treeKey.ver, newSeek, ov.error);
    if (!ov.error.empty())
      return ov;
    ext.insert(ext.end(), mkey.begin(), mkey.end());
    ext.insert(ext.end(), metaPayload.begin(), metaPayload.end());
    superseded.push_back({static_cast<uint64_t>(fm.treeKey.seekkey),
                          static_cast<uint64_t>(fm.treeKey.nbytes)});
    origMap.push_back({static_cast<uint64_t>(newSeek), mkey.size() + metaPayload.size(),
                       static_cast<uint64_t>(fm.treeKey.seekkey),
                       static_cast<uint64_t>(fm.treeKey.nbytes)});
    // Keys-list record: repoint the live tree entry at the relocated metadata
    // key, in place (same size, same offset — see below).
    // Probe clamped at fend: in a small file the record can sit closer than
    // 512 bytes to EOF (a strict Source fails a past-EOF read).
    std::vector<uint8_t> klHead(512);
    uint64_t probe = fm.fend > fm.keyslistSeek
                         ? std::min<uint64_t>(klHead.size(),
                                              static_cast<uint64_t>(fm.fend - fm.keyslistSeek))
                         : 0;
    if (probe == 0) // geometry, not availability: fEND lies at or below the
                    // keys list, so no later pass can make this file buildable
      return fail("keys-list lies at or past the end of the file");
    if (!src.read(klHead.data(), probe, fm.keyslistSeek))
      return unavailable("keys-list header not readable");
    auto kl = parseKey(klHead.data(), probe, 0);
    if (!kl)
      return fail("keys-list key parse failed");
    // Geometry must hold before it sizes an allocation or indexes the record
    // (hostile nbytes/keylen: negative alloc, OOB nkeys read).
    if (kl->nbytes <= 0 || kl->keylen + 4u > static_cast<uint32_t>(kl->nbytes))
      return fail("keys-list key geometry implausible");
    std::vector<uint8_t> klist(kl->nbytes);
    if (!src.has(fm.keyslistSeek, kl->nbytes) ||
        !src.read(klist.data(), klist.size(), fm.keyslistSeek))
      return unavailable("keys-list not available");
    size_t at = kl->keylen;
    int32_t nkeys = 0;
    std::memcpy(&nkeys, klist.data() + at, 4);
    nkeys = __builtin_bswap32(nkeys);
    at += 4;
    bool patched = false;
    for (int32_t i = 0; i < nkeys; ++i) {
      auto e = parseKey(klist.data(), klist.size(), at);
      if (!e)
        return fail("keys-list entry parse failed");
      if (e->cls == "TTree" && e->name == fm.treeKey.name &&
          e->seekkey == fm.treeKey.seekkey) {
        bePut<int32_t>(klist.data() + at,
                       static_cast<int32_t>(fm.treeKey.keylen + metaPayload.size()));
        patchSeek(klist.data() + at, e->ver, newSeek, ov.error);
        if (!ov.error.empty())
          return ov;
        patched = true;
      }
      at += e->keylen;
    }
    if (!patched)
      return fail("live tree entry not found in keys list");
    // The keys-list record stays WHERE IT IS: patching it never changes its
    // size (only the live tree entry's fNbytes/fSeekKey), so it is served as an
    // in-place patch window rather than relocated into the extension. Moving it
    // would have to be published through the root directory's fSeekKeys, and
    // that field is 32 bits wide whenever the directory record's own seeks fit
    // in 32 bits — precisely the layout of a file that grew past 2 GB after its
    // keys list was written, where the extension necessarily starts past 2 GiB
    // and a 4-byte field cannot address it. Left in place, the directory record
    // is never written at all and its width cannot matter.
    if (fm.keyslistSeek < 100 || static_cast<uint64_t>(fm.keyslistSeek) + klist.size() >
                                     static_cast<uint64_t>(extBase))
      return fail("keys-list record does not lie inside the original file");

    // The in-place patch windows + the extension = the overlay. fEND is a
    // HEADER field, so it is written at the header's width — never the
    // directory's, which is independent (see FileMeta).
    const int hw = fm.headerSeekWidth;
    const int64_t newFend = extBase + static_cast<int64_t>(ext.size());
    if (hw == 4 && newFend >= (1ll << 31))
      return fail("small-layout file would grow past 2 GiB");
    std::vector<uint8_t> hdrWin(hw);
    if (hw == 8)
      bePut<int64_t>(hdrWin.data(), newFend);
    else
      bePut<int32_t>(hdrWin.data(), static_cast<int32_t>(newFend));
    ov.tdata.reserve(hdrWin.size() + klist.size() + ext.size());
    ov.tdata.insert(ov.tdata.end(), hdrWin.begin(), hdrWin.end());
    ov.tdata.insert(ov.tdata.end(), klist.begin(), klist.end());
    ov.tdata.insert(ov.tdata.end(), ext.begin(), ext.end());
    ov.meta.extents = {
        {12, static_cast<uint64_t>(hw), 0},
        {static_cast<uint64_t>(fm.keyslistSeek), klist.size(), static_cast<uint64_t>(hw)},
        {static_cast<uint64_t>(extBase), ext.size(), static_cast<uint64_t>(hw) + klist.size()}};
    // Patched in place: these windows ARE their own original range.
    origMap.push_back({12, static_cast<uint64_t>(hw), 12, static_cast<uint64_t>(hw)});
    origMap.push_back({static_cast<uint64_t>(fm.keyslistSeek), klist.size(),
                       static_cast<uint64_t>(fm.keyslistSeek), klist.size()});
    std::sort(origMap.begin(), origMap.end(),
              [](const ReplicaMeta::OrigRange& a, const ReplicaMeta::OrigRange& b) {
                return a.virtOff < b.virtOff;
              });
    ov.meta.origMap = std::move(origMap);
    ov.meta.virtualSize = static_cast<uint64_t>(newFend);
  }

  // Merge superseded ranges (few large punches instead of ~1e5 tiny ones).
  std::sort(superseded.begin(), superseded.end(),
            [](const auto& a, const auto& b) { return a.off < b.off; });
  std::vector<ReplicaMeta::Range> merged;
  for (const auto& r : superseded) {
    if (!merged.empty() && r.off <= merged.back().off + merged.back().len) {
      uint64_t end = std::max(merged.back().off + merged.back().len, r.off + r.len);
      merged.back().len = end - merged.back().off;
    } else {
      merged.push_back(r);
    }
  }
  ov.meta.superseded = std::move(merged);
  ov.meta.encoding = ReplicaMeta::kZstd1;
  ov.meta.encoderVersion = ZSTD_VERSION_NUMBER; // e.g. 10505
  ov.meta.originSize = static_cast<uint64_t>(fm.fend);
  return ov;
}

std::vector<uint8_t> encodeZstdFrames(const uint8_t* raw, size_t n, int level) {
  return zstdFrames(raw, n, level);
}

} // namespace ucache::transpose
