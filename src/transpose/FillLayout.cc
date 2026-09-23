#include "FillLayout.h"

#include "Transposer.h"

#include <algorithm>
#include <climits>
#include <string>
#include <cmath>
#include <cstring>

namespace ucache::transpose {
namespace {

// The metadata key is compressed on the path of the reader's first read, so
// speed decides the level here; on a large NanoAOD tree record ZSTD-1 came out
// no bigger than ZSTD-9 as well. The published replica keeps its own level.
constexpr int kFillMetaZstdLevel = 1;
constexpr uint64_t kAlign = 4096; // the extension starts on a page boundary

template <typename T> void bePut(uint8_t* p, T v) {
  using U = std::make_unsigned_t<T>;
  U u;
  std::memcpy(&u, &v, sizeof(T));
  for (size_t i = sizeof(T); i-- > 0;) {
    p[i] = static_cast<uint8_t>(u & 0xFF);
    u >>= 8;
  }
}
template <typename T> T beGet(const uint8_t* p) {
  using U = std::make_unsigned_t<T>;
  U u = 0;
  for (size_t i = 0; i < sizeof(T); ++i)
    u = static_cast<U>((u << 8) | p[i]);
  T v;
  std::memcpy(&v, &u, sizeof(T));
  return v;
}

// fSeekKey of a key header at `rec`, written at the key's own width: 64-bit
// for a version above 1000, 32-bit otherwise. False when a 32-bit key cannot
// hold `seek`.
bool putKeySeek(uint8_t* rec, uint16_t ver, int64_t seek) {
  if (ver > 1000) {
    bePut<int64_t>(rec + 18, seek);
    return true;
  }
  if (seek < 0 || seek >= (1ll << 31))
    return false;
  bePut<int32_t>(rec + 18, static_cast<int32_t>(seek));
  return true;
}

// The frame header names the codec a payload was actually written with.
std::string frameCodec(const uint8_t* p, size_t n) {
  if (n < 9)
    return "";
  if (p[0] == 'X' && p[1] == 'Z') return "lzma";
  if (p[0] == 'Z' && p[1] == 'L') return "zlib";
  if (p[0] == 'Z' && p[1] == 'S') return "zstd";
  if (p[0] == 'L' && p[1] == '4') return "lz4";
  return "";
}

bool listed(const std::vector<std::string>& codecs, const std::string& c) {
  return !c.empty() && std::find(codecs.begin(), codecs.end(), c) != codecs.end();
}

// The file header, in either layout. Only what a rewrite needs.
struct Header {
  int32_t version = 0, begin = 0;
  int64_t end = 0, seekFree = 0, seekInfo = 0;
  int32_t nbytesFree = 0, nfree = 0, nbytesName = 0, compress = 0, nbytesInfo = 0;
  uint8_t units = 0;
  const uint8_t* uuid = nullptr; // 18 bytes
  bool large = false;
};

bool parseHeader(const std::vector<uint8_t>& h, Header& out) {
  if (h.size() < 75 || std::memcmp(h.data(), "root", 4) != 0)
    return false;
  out.version = beGet<int32_t>(&h[4]);
  out.begin = beGet<int32_t>(&h[8]);
  out.large = out.version >= 1000000;
  if (out.large) {
    out.end = beGet<int64_t>(&h[12]);
    out.seekFree = beGet<int64_t>(&h[20]);
    out.nbytesFree = beGet<int32_t>(&h[28]);
    out.nfree = beGet<int32_t>(&h[32]);
    out.nbytesName = beGet<int32_t>(&h[36]);
    out.units = h[40];
    out.compress = beGet<int32_t>(&h[41]);
    out.seekInfo = beGet<int64_t>(&h[45]);
    out.nbytesInfo = beGet<int32_t>(&h[53]);
    out.uuid = &h[57];
  } else {
    out.end = beGet<int32_t>(&h[12]);
    out.seekFree = beGet<int32_t>(&h[16]);
    out.nbytesFree = beGet<int32_t>(&h[20]);
    out.nfree = beGet<int32_t>(&h[24]);
    out.nbytesName = beGet<int32_t>(&h[28]);
    out.units = h[32];
    out.compress = beGet<int32_t>(&h[33]);
    out.seekInfo = beGet<int32_t>(&h[37]);
    out.nbytesInfo = beGet<int32_t>(&h[41]);
    out.uuid = &h[45];
  }
  return out.begin >= 75;
}

// The 64-bit header layout — what ROOT itself writes once a file passes 2 GB
// (TFile::WriteHeader) — for a header that was written 32-bit. 75 bytes,
// which fit before fBEGIN in any file ROOT writes.
std::vector<uint8_t> largeHeader(const Header& h, int64_t newEnd) {
  std::vector<uint8_t> w(75, 0);
  std::memcpy(&w[0], "root", 4);
  bePut<int32_t>(&w[4], h.large ? h.version : h.version + 1000000);
  bePut<int32_t>(&w[8], h.begin);
  bePut<int64_t>(&w[12], newEnd);
  bePut<int64_t>(&w[20], h.seekFree);
  bePut<int32_t>(&w[28], h.nbytesFree);
  bePut<int32_t>(&w[32], h.nfree);
  bePut<int32_t>(&w[36], h.nbytesName);
  w[40] = 8; // fUnits: pointer width
  bePut<int32_t>(&w[41], h.compress);
  bePut<int64_t>(&w[45], h.seekInfo);
  bePut<int32_t>(&w[53], h.nbytesInfo);
  std::memcpy(&w[57], h.uuid, 18);
  return w;
}

} // namespace

std::string codecOfSetting(int32_t compress, int32_t fileCompress) {
  int32_t c = compress < 0 ? fileCompress : compress;
  if (c <= 0 || c % 100 == 0) // unset, or level 0: stored uncompressed
    return "";
  switch (c / 100) {
  case 1: return "zlib";
  case 2: return "lzma";
  case 4: return "lz4";
  case 5: return "zstd";
  default: return ""; // 0 (global default, meaning unknown here), 3 (the old
                      // ROOT algorithm): not transcoded
  }
}

FillLayout layoutForFill(const FileMeta& fm, uint64_t fileSize, const std::vector<uint8_t>& header,
                         const std::vector<uint8_t>& treeKeyHeader,
                         const std::vector<uint8_t>& keysList,
                         const std::vector<std::string>& codecs, uint32_t k) {
  FillLayout L;
  auto decline = [&](std::string why) {
    L.error = std::move(why);
    L.windows.clear();
    L.slots.clear();
    L.relocated.clear();
    L.metaRecord.clear();
    return L;
  };
  if (!fm.error.empty())
    return decline("parse: " + fm.error);
  if (k < 1)
    return decline("slot factor must be at least 1");
  Header H;
  if (!parseHeader(header, H))
    return decline("file header unreadable or fBEGIN below 75");
  if (H.end != fm.fend)
    return decline("file header disagrees with the parse on fEND");
  if (treeKeyHeader.size() != fm.treeKey.keylen || fm.treeKey.keylen < 26)
    return decline("tree key header missing");
  if (fm.zipBytesOff == 0 || fm.autoFlushOff == 0 || fm.zipBytes <= 0)
    return decline("tree record fields not located");

  // Which branches, in which order. Branch-major and in tree order, so a
  // column's slots are adjacent in the virtual file.
  for (uint32_t b = 0; b < fm.branches.size(); ++b) {
    const BranchInfo& br = fm.branches[b];
    if (br.writeBasket <= 0 || br.externalFile || br.zipBytesOff == 0 ||
        !listed(codecs, codecOfSetting(br.compress, H.compress)))
      continue;
    bool inside = true;
    for (int32_t i = 0; i < br.writeBasket && inside; ++i)
      inside = br.basketSeek[i] > 0 && br.basketBytes[i] > 0 &&
               static_cast<uint64_t>(br.basketSeek[i]) + static_cast<uint64_t>(br.basketBytes[i]) <=
                   static_cast<uint64_t>(fm.fend);
    if (inside) // a basket with no seek lives inside the tree record itself;
                // one past EOF is a parse nothing should act on
      L.relocated.push_back(b);
  }
  if (L.relocated.empty())
    return decline("no relocatable branch");

  // Geometry: [fEND, metaSeek) nothing, [metaSeek, slotsBegin) the metadata
  // key's reservation, then the slots. The reservation is the key header plus
  // the RAW tree record: the patched record has the same length (every patched
  // field is fixed-width), and its compressed size is not known until the
  // slot positions it contains are.
  if (fileSize != static_cast<uint64_t>(fm.fend)) // bytes past fEND would sit under the extension
    return decline("file size " + std::to_string(fileSize) + " differs from fEND " +
                   std::to_string(fm.fend));
  L.originSize = fileSize;
  L.metaSeek = (L.originSize + kAlign - 1) / kAlign * kAlign;
  const uint64_t reserve = fm.treeKey.keylen + fm.treeBlob.size();
  L.slotsBegin = L.metaSeek + reserve;

  std::vector<uint8_t> blob = fm.treeBlob;
  uint64_t at = L.slotsBegin;
  int64_t treeGrowth = 0;
  for (uint32_t b : L.relocated) {
    const BranchInfo& br = fm.branches[b];
    int64_t branchGrowth = 0;
    for (int32_t i = 0; i < br.writeBasket; ++i) {
      FillSlot s;
      s.branch = b;
      s.basket = static_cast<uint32_t>(i);
      s.origSeek = static_cast<uint64_t>(br.basketSeek[i]);
      s.origLen = static_cast<uint32_t>(br.basketBytes[i]);
      const uint64_t want = static_cast<uint64_t>(s.origLen) * k;
      s.vLen = static_cast<uint32_t>(std::min<uint64_t>(want, INT32_MAX));
      s.vSeek = at;
      at += s.vLen;
      branchGrowth += static_cast<int64_t>(s.vLen) - static_cast<int64_t>(s.origLen);
      bePut<int64_t>(blob.data() + br.seekArrayOff + 8ull * i, static_cast<int64_t>(s.vSeek));
      bePut<int32_t>(blob.data() + br.bytesArrayOff + 4ull * i, static_cast<int32_t>(s.vLen));
      L.slots.push_back(s);
    }
    bePut<int64_t>(blob.data() + br.zipBytesOff, br.zipBytes + branchGrowth);
    treeGrowth += branchGrowth;
  }
  L.virtualSize = at;
  const int64_t newZip = fm.zipBytes + treeGrowth;
  bePut<int64_t>(blob.data() + fm.zipBytesOff, newZip);
  if (fm.autoFlush < 0) {
    // Flushed by size: the cache is |fAutoFlush| bytes and a cluster's length
    // in entries is estimated from fZipBytes. Scaling both by the same factor
    // keeps the estimate and lets a cluster's slots fit the cache as its
    // baskets did.
    const double f = static_cast<double>(newZip) / static_cast<double>(fm.zipBytes);
    bePut<int64_t>(blob.data() + fm.autoFlushOff,
                   static_cast<int64_t>(std::llround(static_cast<double>(fm.autoFlush) * f)));
  }

  // The relocated metadata key: the original header bytes (same length),
  // fNbytes = the record's length, fSeekKey = metaSeek; fObjlen is untouched,
  // so ROOT still inflates it to the same size. Stored raw if compressing does
  // not shrink it.
  std::vector<uint8_t> payload = encodeZstdFrames(blob.data(), blob.size(), kFillMetaZstdLevel);
  if (payload.empty() || payload.size() >= blob.size())
    payload = blob;
  L.metaRecord = treeKeyHeader;
  L.metaRecord.insert(L.metaRecord.end(), payload.begin(), payload.end());
  if (L.metaRecord.size() > reserve)
    return decline("metadata key outgrew its reservation");
  bePut<int32_t>(L.metaRecord.data(), static_cast<int32_t>(L.metaRecord.size()));
  if (!putKeySeek(L.metaRecord.data(), fm.treeKey.ver, static_cast<int64_t>(L.metaSeek)))
    return decline("tree key is 32-bit and the extension starts past 2 GiB");

  // The keys list, patched in place: its size never changes, so neither does
  // anything that points at it (see Transposer.cc for why it is never moved).
  auto kl = parseKey(keysList.data(), keysList.size(), 0);
  if (!kl || kl->nbytes <= 0 || static_cast<size_t>(kl->nbytes) != keysList.size() ||
      kl->keylen + 4u > keysList.size())
    return decline("keys-list record malformed");
  if (fm.keyslistSeek < 100 ||
      static_cast<uint64_t>(fm.keyslistSeek) + keysList.size() > L.originSize)
    return decline("keys-list record does not lie inside the original file");
  std::vector<uint8_t> klist = keysList;
  const int32_t nkeys = beGet<int32_t>(klist.data() + kl->keylen);
  size_t pos = kl->keylen + 4;
  bool patched = false;
  for (int32_t i = 0; i < nkeys; ++i) {
    auto e = parseKey(klist.data(), klist.size(), pos);
    if (!e)
      return decline("keys-list entry malformed");
    if (e->cls == "TTree" && e->name == fm.treeKey.name && e->seekkey == fm.treeKey.seekkey) {
      bePut<int32_t>(klist.data() + pos, static_cast<int32_t>(L.metaRecord.size()));
      if (!putKeySeek(klist.data() + pos, e->ver, static_cast<int64_t>(L.metaSeek)))
        return decline("keys-list entry is 32-bit and the metadata key lies past 2 GiB");
      patched = true;
    }
    pos += e->keylen;
  }
  if (!patched)
    return decline("live tree entry not found in the keys list");

  // The header: fEND at its own width, or the whole header rewritten in the
  // 64-bit layout when a 32-bit header cannot hold the virtual end.
  if (H.large) {
    std::vector<uint8_t> w(8);
    bePut<int64_t>(w.data(), static_cast<int64_t>(L.virtualSize));
    L.windows.push_back({12, std::move(w)});
  } else if (L.virtualSize < (1ull << 31)) {
    std::vector<uint8_t> w(4);
    bePut<int32_t>(w.data(), static_cast<int32_t>(L.virtualSize));
    L.windows.push_back({12, std::move(w)});
  } else {
    L.windows.push_back({0, largeHeader(H, static_cast<int64_t>(L.virtualSize))});
  }
  L.windows.push_back({static_cast<uint64_t>(fm.keyslistSeek), std::move(klist)});
  return L;
}

ConvertedBasket convertBasket(const uint8_t* rec, size_t n, uint32_t slotLen,
                              const std::vector<std::string>& codecs) {
  ConvertedBasket c;
  auto k = parseKey(rec, n, 0);
  if (!k || k->cls != "TBasket" || k->nbytes <= 0 || static_cast<size_t>(k->nbytes) != n ||
      k->keylen > n) {
    c.error = "not a basket record";
    return c;
  }
  auto original = [&] {
    c.kind = ConvertedBasket::kOriginal;
    c.record.assign(rec, rec + n);
    return c;
  };
  if (n > slotLen) {
    c.error = "record longer than its slot";
    return c;
  }
  const uint8_t* pay = rec + k->keylen;
  const size_t payLen = n - k->keylen;
  const bool storedRaw = k->objlen == k->nbytes - static_cast<int32_t>(k->keylen);
  if (storedRaw)
    return original(); // nothing for the reader to decode already
  c.codec = frameCodec(pay, payLen);
  if (!listed(codecs, c.codec) || k->objlen <= 0)
    return original();
  std::vector<uint8_t> raw = decompressFrames(pay, payLen, static_cast<uint64_t>(k->objlen));
  if (raw.empty())
    return original();
  std::vector<uint8_t> z = encodeZstdFrames(raw.data(), raw.size(), 1);
  const std::vector<uint8_t>* use = nullptr;
  if (!z.empty() && z.size() < raw.size()) {
    if (k->keylen + z.size() <= slotLen) {
      use = &z;
      c.kind = ConvertedBasket::kZstd;
    }
  } else if (k->keylen + raw.size() <= slotLen) { // ZSTD cannot shrink it: raw
    use = &raw;
    c.kind = ConvertedBasket::kRaw;
  }
  if (!use)
    return original();
  c.record.assign(rec, rec + k->keylen); // the key header keeps its length
  c.record.insert(c.record.end(), use->begin(), use->end());
  bePut<int32_t>(c.record.data(), static_cast<int32_t>(c.record.size())); // fNbytes
  return c;
}

bool patchKeySeek(uint8_t* record, size_t n, uint64_t seek) {
  if (n < 26 || seek > static_cast<uint64_t>(INT64_MAX))
    return false;
  const uint16_t ver = beGet<uint16_t>(record + 4);
  if (ver > 1000 && n < 34)
    return false;
  return putKeySeek(record, ver, static_cast<int64_t>(seek));
}

bool placeInSlot(const ConvertedBasket& c, const FillSlot& slot, uint8_t* out, std::string& err) {
  if (!c.error.empty() || c.record.size() < 26) {
    err = c.error.empty() ? "empty record" : c.error;
    return false;
  }
  if (c.record.size() > slot.vLen) {
    err = "record longer than its slot";
    return false;
  }
  std::memcpy(out, c.record.data(), c.record.size());
  const uint16_t ver = beGet<uint16_t>(out + 4);
  if (!putKeySeek(out, ver, static_cast<int64_t>(slot.vSeek))) {
    err = "32-bit basket key cannot address a slot past 2 GiB";
    return false;
  }
  std::memset(out + c.record.size(), 0, slot.vLen - c.record.size());
  return true;
}

} // namespace ucache::transpose
