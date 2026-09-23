// The cold-run layout: every basket of a relocatable branch gets a slot k times
// its stored length past fEND, and the metadata the reader sees points at the
// slots. These tests pin the geometry, every metadata patch, the header and
// keys-list rewrites, the declines, and how a basket is prepared for its slot.
// ROOT's own verdict on a materialized file is the integration check; these
// catch a wrong offset or width without it.
#include "FillLayout.h"
#include "Transposer.h"
#include "TreeMeta.h"

#include <climits>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace ucache::transpose;

namespace {

void bePut16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
void bePut32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
void bePut64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
}
uint64_t beGet64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v = (v << 8) | p[i];
  return v;
}
uint32_t beGet32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i)
    v = (v << 8) | p[i];
  return v;
}

std::vector<uint8_t> keyHeader(bool wide, const std::string& cls, const std::string& name,
                              int32_t nbytes, int32_t objlen, int64_t seekkey) {
  const size_t fixed = wide ? 34u : 26u;
  const size_t keylen = fixed + 1 + cls.size() + 1 + name.size() + 1;
  std::vector<uint8_t> k(keylen, 0);
  bePut32(k.data(), static_cast<uint32_t>(nbytes));
  bePut16(k.data() + 4, static_cast<uint16_t>(wide ? 1004 : 4));
  bePut32(k.data() + 6, static_cast<uint32_t>(objlen));
  bePut16(k.data() + 14, static_cast<uint16_t>(keylen));
  bePut16(k.data() + 16, 1);
  if (wide)
    bePut64(k.data() + 18, static_cast<uint64_t>(seekkey));
  else
    bePut32(k.data() + 18, static_cast<uint32_t>(seekkey));
  size_t q = fixed;
  k[q++] = static_cast<uint8_t>(cls.size());
  std::memcpy(k.data() + q, cls.data(), cls.size());
  q += cls.size();
  k[q++] = static_cast<uint8_t>(name.size());
  std::memcpy(k.data() + q, name.data(), name.size());
  return k;
}

// Offsets of the fields inside the crafted tree record.
constexpr uint64_t kTreeZipOff = 10, kAutoFlushOff = 20;
constexpr uint64_t kB0Seek = 100, kB0Bytes = 130, kB0Zip = 140;
constexpr uint64_t kB1Seek = 200, kB1Bytes = 230, kB1Zip = 240;
constexpr uint64_t kB2Seek = 300, kB2Bytes = 330, kB2Zip = 340;
constexpr int64_t kTreeKeySeek = 50000, kKeysListSeek = 60000;

struct Fx {
  FileMeta fm;
  std::vector<uint8_t> header, treeKeyHeader, keysList;
  uint64_t fileSize = 0;
  size_t entryOff = 0; // the tree entry inside keysList
};

// Three branches: b0 LZMA (two baskets), b1 ZSTD, b2 inheriting the file's
// setting. `fend` sets the file size, `largeHeader` the header layout,
// `wideEntry` the width of the keys-list entry for the tree.
Fx fixture(int64_t fend = 70000, bool largeHeader = false, bool wideEntry = true,
           int32_t fileCompress = 207, int64_t autoFlush = -30000000) {
  Fx fx;
  FileMeta& fm = fx.fm;
  fm.fend = fend;
  fm.large = largeHeader;
  fm.keyslistSeek = kKeysListSeek;
  fm.treeBlob.assign(400, 0);
  fm.zipBytes = 10000;
  fm.zipBytesOff = kTreeZipOff;
  fm.autoFlush = autoFlush;
  fm.autoFlushOff = kAutoFlushOff;
  auto branch = [&](const char* name, int32_t compress, std::vector<int64_t> seeks,
                    std::vector<int32_t> bytes, uint64_t so, uint64_t bo, uint64_t zo) {
    BranchInfo b;
    b.name = name;
    b.compress = compress;
    b.writeBasket = static_cast<int32_t>(seeks.size());
    b.maxBaskets = static_cast<uint32_t>(seeks.size());
    b.basketSeek = seeks;
    b.basketBytes = bytes;
    b.seekArrayOff = so;
    b.bytesArrayOff = bo;
    b.zipBytesOff = zo;
    for (int32_t x : bytes) b.zipBytes += x;
    fm.branches.push_back(b);
  };
  branch("Muon_pt", 207, {1000, 3000}, {1500, 2500}, kB0Seek, kB0Bytes, kB0Zip);
  branch("Muon_eta", 505, {6000}, {800}, kB1Seek, kB1Bytes, kB1Zip);
  branch("Muon_phi", -1, {7000}, {900}, kB2Seek, kB2Bytes, kB2Zip);

  fx.header.assign(100, 0);
  std::memcpy(fx.header.data(), "root", 4);
  bePut32(&fx.header[8], 100);
  if (largeHeader) {
    bePut32(&fx.header[4], 1062604);
    bePut64(&fx.header[12], static_cast<uint64_t>(fend));
    bePut64(&fx.header[20], 1234);          // fSeekFree
    bePut32(&fx.header[28], 56);            // fNbytesFree
    bePut32(&fx.header[41], static_cast<uint32_t>(fileCompress));
    bePut64(&fx.header[45], 4321);          // fSeekInfo
  } else {
    bePut32(&fx.header[4], 62604);
    bePut32(&fx.header[12], static_cast<uint32_t>(fend));
    bePut32(&fx.header[16], 1234);          // fSeekFree
    bePut32(&fx.header[20], 56);            // fNbytesFree
    bePut32(&fx.header[24], 1);             // nfree
    bePut32(&fx.header[28], 77);            // fNbytesName
    fx.header[32] = 4;                      // fUnits
    bePut32(&fx.header[33], static_cast<uint32_t>(fileCompress));
    bePut32(&fx.header[37], 4321);          // fSeekInfo
    bePut32(&fx.header[41], 99);            // fNbytesInfo
    for (int i = 0; i < 18; ++i) fx.header[45 + i] = static_cast<uint8_t>(0xC0 + i); // UUID
  }

  fx.treeKeyHeader = keyHeader(true, "TTree", "Events", 2000, 400, kTreeKeySeek);
  fm.treeKey.nbytes = 2000;
  fm.treeKey.objlen = 400;
  fm.treeKey.keylen = static_cast<uint16_t>(fx.treeKeyHeader.size());
  fm.treeKey.ver = 1004;
  fm.treeKey.cycle = 1;
  fm.treeKey.seekkey = kTreeKeySeek;
  fm.treeKey.cls = "TTree";
  fm.treeKey.name = "Events";

  auto own = keyHeader(false, "TFile", "f.root", 0, 0, kKeysListSeek);
  auto other = keyHeader(false, "TNamed", "note", 50, 20, 900);
  auto entry = keyHeader(wideEntry, "TTree", "Events", 2000, 400, kTreeKeySeek);
  fx.keysList = own;
  fx.keysList.resize(own.size() + 4);
  bePut32(fx.keysList.data() + own.size(), 2);
  fx.keysList.insert(fx.keysList.end(), other.begin(), other.end());
  fx.entryOff = fx.keysList.size();
  fx.keysList.insert(fx.keysList.end(), entry.begin(), entry.end());
  bePut32(fx.keysList.data(), static_cast<uint32_t>(fx.keysList.size()));
  fx.fileSize = static_cast<uint64_t>(fend);
  return fx;
}

FillLayout layout(const Fx& fx, std::vector<std::string> codecs = {"lzma", "zlib"}, uint32_t k = 4) {
  return layoutForFill(fx.fm, fx.fileSize, fx.header, fx.treeKeyHeader, fx.keysList, codecs, k);
}

std::vector<uint8_t> metaBlob(const FillLayout& L, const Fx& fx) {
  const uint8_t* pay = L.metaRecord.data() + fx.fm.treeKey.keylen;
  const size_t n = L.metaRecord.size() - fx.fm.treeKey.keylen;
  if (n == fx.fm.treeBlob.size())
    return std::vector<uint8_t>(pay, pay + n); // stored raw
  return decompressFrames(pay, n, fx.fm.treeBlob.size());
}

// A basket record: key header + payload. `payload` is written as given.
std::vector<uint8_t> basketRecord(int32_t objlen, const std::vector<uint8_t>& payload, int64_t seek = 1000) {
  auto k = keyHeader(true, "TBasket", "Muon_pt", 0, objlen, seek);
  std::vector<uint8_t> r = k;
  r.insert(r.end(), payload.begin(), payload.end());
  bePut32(r.data(), static_cast<uint32_t>(r.size()));
  return r;
}

// Two bits of entropy per byte: every ZSTD level lands near 4:1, like physics
// data does between codecs.
std::vector<uint8_t> compressible(size_t n) {
  std::vector<uint8_t> v(n);
  uint64_t x = 0x2545F4914F6CDD1Dull;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v[i] = static_cast<uint8_t>(x & 3);
  }
  return v;
}
// A short period: ZSTD-19 finds it and ZSTD-1 mostly does not.
std::vector<uint8_t> periodic(size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>((i / 7) % 13);
  return v;
}
std::vector<uint8_t> incompressible(size_t n) {
  std::vector<uint8_t> v(n);
  uint64_t x = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v[i] = static_cast<uint8_t>(x);
  }
  return v;
}

} // namespace

TEST(FillLayout, CodecOfSetting) {
  EXPECT_EQ(codecOfSetting(207, 0), "lzma");
  EXPECT_EQ(codecOfSetting(101, 0), "zlib");
  EXPECT_EQ(codecOfSetting(404, 0), "lz4");
  EXPECT_EQ(codecOfSetting(505, 0), "zstd");
  EXPECT_EQ(codecOfSetting(-1, 209), "lzma"); // inherits the file's
  EXPECT_EQ(codecOfSetting(-1, 0), "");
  EXPECT_EQ(codecOfSetting(200, 0), "");      // level 0 = stored
  EXPECT_EQ(codecOfSetting(0, 207), "");      // explicit 0 does NOT inherit
  EXPECT_EQ(codecOfSetting(301, 0), "");      // old ROOT algorithm: not transcoded
  EXPECT_EQ(codecOfSetting(1, 0), "");        // algorithm 0 = global default, unknown here
}

TEST(FillLayout, SlotsAreContiguousAndSized) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  // b0 (lzma) and b2 (inherits 207); b1 is zstd, not listed.
  ASSERT_EQ(L.relocated, (std::vector<uint32_t>{0, 2}));
  EXPECT_EQ(L.originSize, 70000u);
  EXPECT_EQ(L.metaSeek, 73728u); // fEND rounded up to 4 KiB
  EXPECT_EQ(L.slotsBegin, L.metaSeek + fx.fm.treeKey.keylen + fx.fm.treeBlob.size());
  ASSERT_EQ(L.slots.size(), 3u);
  uint64_t at = L.slotsBegin;
  const uint32_t orig[] = {1500, 2500, 900};
  const uint64_t oseek[] = {1000, 3000, 7000};
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(L.slots[i].vSeek, at);
    EXPECT_EQ(L.slots[i].origLen, orig[i]);
    EXPECT_EQ(L.slots[i].origSeek, oseek[i]);
    EXPECT_EQ(L.slots[i].vLen, 4 * orig[i]);
    at += L.slots[i].vLen;
  }
  EXPECT_EQ(L.slots[2].branch, 2u);
  EXPECT_EQ(L.slots[1].basket, 1u);
  EXPECT_EQ(L.virtualSize, at);
  EXPECT_GE(L.metaSeek + L.metaRecord.size(), L.metaSeek);
  EXPECT_LE(L.metaSeek + L.metaRecord.size(), L.slotsBegin);
}

TEST(FillLayout, MetadataPointsAtSlotsAndScalesSizes) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  auto blob = metaBlob(L, fx);
  ASSERT_EQ(blob.size(), fx.fm.treeBlob.size());
  EXPECT_EQ(beGet64(&blob[kB0Seek]), L.slots[0].vSeek);
  EXPECT_EQ(beGet64(&blob[kB0Seek + 8]), L.slots[1].vSeek);
  EXPECT_EQ(beGet32(&blob[kB0Bytes]), L.slots[0].vLen);
  EXPECT_EQ(beGet32(&blob[kB0Bytes + 4]), L.slots[1].vLen);
  EXPECT_EQ(beGet64(&blob[kB2Seek]), L.slots[2].vSeek);
  // The unlisted branch is untouched.
  EXPECT_EQ(beGet64(&blob[kB1Seek]), 0u);
  EXPECT_EQ(beGet64(&blob[kB1Zip]), 0u);
  // fZipBytes grows by exactly the padding, per branch and for the tree.
  EXPECT_EQ(beGet64(&blob[kB0Zip]), 4u * 4000);
  EXPECT_EQ(beGet64(&blob[kB2Zip]), 4u * 900);
  const int64_t growth = 3 * 4000 + 3 * 900;
  EXPECT_EQ(static_cast<int64_t>(beGet64(&blob[kTreeZipOff])), 10000 + growth);
  // Flushed by size: fAutoFlush scales with fZipBytes, so the cache holds what
  // it held before and the cluster estimate (entries x cache / zip) is unchanged.
  const double f = (10000.0 + growth) / 10000.0;
  EXPECT_EQ(static_cast<int64_t>(beGet64(&blob[kAutoFlushOff])), std::llround(-30000000 * f));
}

TEST(FillLayout, AutoFlushByEntriesIsLeftAlone) {
  Fx fx = fixture(70000, false, true, 207, 1000);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  auto blob = metaBlob(L, fx);
  EXPECT_EQ(beGet64(&blob[kAutoFlushOff]), 0u); // never written: the cache scales via fZipBytes
}

TEST(FillLayout, MetadataRecordKeepsItsKeyHeaderLength) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  const size_t kl = fx.fm.treeKey.keylen;
  ASSERT_GT(L.metaRecord.size(), kl);
  EXPECT_EQ(beGet32(L.metaRecord.data()), L.metaRecord.size());           // fNbytes
  EXPECT_EQ(beGet64(L.metaRecord.data() + 18), L.metaSeek);               // fSeekKey
  EXPECT_EQ(beGet32(L.metaRecord.data() + 6), fx.fm.treeBlob.size());     // fObjlen unchanged
  EXPECT_EQ((L.metaRecord[14] << 8) | L.metaRecord[15], static_cast<int>(kl)); // fKeylen unchanged
  // Everything else in the key header is the original's.
  EXPECT_EQ(0, std::memcmp(L.metaRecord.data() + 4, fx.treeKeyHeader.data() + 4, 14));
  EXPECT_EQ(0, std::memcmp(L.metaRecord.data() + 26, fx.treeKeyHeader.data() + 26, kl - 26));
}

TEST(FillLayout, KeysListEntryPointsAtTheMetadataKey) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_EQ(L.windows.size(), 2u);
  const auto& w = L.windows[1];
  EXPECT_EQ(w.off, static_cast<uint64_t>(kKeysListSeek));
  ASSERT_EQ(w.bytes.size(), fx.keysList.size()); // patched in place: same length
  EXPECT_EQ(beGet32(&w.bytes[fx.entryOff]), L.metaRecord.size());
  EXPECT_EQ(beGet64(&w.bytes[fx.entryOff + 18]), L.metaSeek);
  // Nothing outside the live entry's two fields changed.
  std::vector<uint8_t> a = fx.keysList, b = w.bytes;
  std::memset(&a[fx.entryOff], 0, 4);
  std::memset(&b[fx.entryOff], 0, 4);
  std::memset(&a[fx.entryOff + 18], 0, 8);
  std::memset(&b[fx.entryOff + 18], 0, 8);
  EXPECT_EQ(a, b);
}

TEST(FillLayout, NarrowKeysListEntryBelow2GiB) {
  Fx fx = fixture(70000, false, /*wideEntry=*/false);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(beGet32(&L.windows[1].bytes[fx.entryOff + 18]), L.metaSeek);
}

TEST(FillLayout, SmallHeaderKeepsItsWidthBelow2GiB) {
  Fx fx = fixture();
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.windows[0].off, 12u);
  ASSERT_EQ(L.windows[0].bytes.size(), 4u);
  EXPECT_EQ(beGet32(L.windows[0].bytes.data()), L.virtualSize);
}

TEST(FillLayout, LargeHeaderWritesFEndAt64Bits) {
  Fx fx = fixture(70000, /*largeHeader=*/true);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.windows[0].off, 12u);
  ASSERT_EQ(L.windows[0].bytes.size(), 8u);
  EXPECT_EQ(beGet64(L.windows[0].bytes.data()), L.virtualSize);
}

TEST(FillLayout, SmallHeaderIsPromotedPast2GiB) {
  // A 600 MB basket padded 4x ends past 2^31: the 32-bit header cannot hold fEND.
  Fx fx = fixture();
  fx.fm.branches[0].basketBytes = {1500, 600000000};
  fx.fm.fend = fx.fileSize = 700000000;
  bePut32(&fx.header[12], 700000000);
  FillLayout L = layout(fx);
  ASSERT_TRUE(L.error.empty()) << L.error;
  ASSERT_GE(L.virtualSize, 1ull << 31);
  const auto& w = L.windows[0].bytes;
  ASSERT_EQ(L.windows[0].off, 0u);
  ASSERT_EQ(w.size(), 75u);
  EXPECT_EQ(0, std::memcmp(w.data(), "root", 4));
  EXPECT_EQ(beGet32(&w[4]), 1062604u);
  EXPECT_EQ(beGet32(&w[8]), 100u);
  EXPECT_EQ(beGet64(&w[12]), L.virtualSize);
  EXPECT_EQ(beGet64(&w[20]), 1234u);  // fSeekFree
  EXPECT_EQ(beGet32(&w[28]), 56u);    // fNbytesFree
  EXPECT_EQ(beGet32(&w[32]), 1u);     // nfree
  EXPECT_EQ(beGet32(&w[36]), 77u);    // fNbytesName
  EXPECT_EQ(w[40], 8);                // fUnits
  EXPECT_EQ(beGet32(&w[41]), 207u);   // fCompress
  EXPECT_EQ(beGet64(&w[45]), 4321u);  // fSeekInfo
  EXPECT_EQ(beGet32(&w[53]), 99u);    // fNbytesInfo
  EXPECT_EQ(0, std::memcmp(&w[57], &fx.header[45], 18)); // UUID
}

TEST(FillLayout, SlotLengthIsCappedAtInt32Max) {
  Fx fx = fixture();
  fx.fm.branches[0].basketBytes = {1500, 600000000};
  fx.fm.fend = fx.fileSize = 700000000;
  bePut32(&fx.header[12], 700000000);
  FillLayout L = layout(fx, {"lzma"}, 4);
  ASSERT_TRUE(L.error.empty()) << L.error;
  EXPECT_EQ(L.slots[1].vLen, static_cast<uint32_t>(INT32_MAX));
}

TEST(FillLayout, Declines) {
  {
    Fx fx = fixture();
    fx.fileSize += 1; // bytes past fEND
    EXPECT_NE(layout(fx).error.find("differs from fEND"), std::string::npos);
  }
  {
    Fx fx = fixture();
    EXPECT_NE(layout(fx, {"lz4"}).error.find("no relocatable branch"), std::string::npos);
  }
  {
    Fx fx = fixture(); // a branch whose baskets are in another file is left alone
    fx.fm.branches[0].externalFile = true;
    FillLayout L = layout(fx);
    ASSERT_TRUE(L.error.empty());
    EXPECT_EQ(L.relocated, (std::vector<uint32_t>{2}));
  }
  {
    Fx fx = fixture(); // a basket embedded in the tree record (seek 0)
    fx.fm.branches[0].basketSeek[1] = 0;
    EXPECT_EQ(layout(fx).relocated, (std::vector<uint32_t>{2}));
  }
  {
    Fx fx = fixture(); // a basket past EOF
    fx.fm.branches[2].basketSeek[0] = 69500;
    EXPECT_EQ(layout(fx).relocated, (std::vector<uint32_t>{0}));
  }
  {
    Fx fx = fixture(); // header and parse disagree
    bePut32(&fx.header[12], 12345);
    EXPECT_FALSE(layout(fx).error.empty());
  }
  {
    Fx fx = fixture(); // the keys list has no entry for the live tree
    bePut64(&fx.keysList[fx.entryOff + 18], 1);
    EXPECT_NE(layout(fx).error.find("live tree entry"), std::string::npos);
  }
  {
    // A 32-bit tree key cannot address a metadata key past 2 GiB.
    Fx big = fixture(2200000000ll, true);
    big.treeKeyHeader = keyHeader(false, "TTree", "Events", 2000, 400, kTreeKeySeek);
    big.fm.treeKey.keylen = static_cast<uint16_t>(big.treeKeyHeader.size());
    big.fm.treeKey.ver = 4;
    FillLayout L = layout(big);
    EXPECT_NE(L.error.find("32-bit"), std::string::npos) << L.error;
  }
  {
    // A 32-bit keys-list entry, same.
    Fx big = fixture(2200000000ll, true, /*wideEntry=*/false);
    FillLayout L = layout(big);
    EXPECT_NE(L.error.find("keys-list entry is 32-bit"), std::string::npos) << L.error;
  }
  {
    Fx fx = fixture(); // declined layouts carry nothing
    fx.fileSize += 1;
    FillLayout L = layout(fx);
    EXPECT_TRUE(L.slots.empty());
    EXPECT_TRUE(L.windows.empty());
    EXPECT_TRUE(L.metaRecord.empty());
  }
}

TEST(FillLayout, ConvertsToZstdWhenItFits) {
  auto raw = compressible(20000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 19);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  ASSERT_TRUE(c.error.empty()) << c.error;
  EXPECT_EQ(c.kind, ConvertedBasket::kZstd);
  EXPECT_EQ(c.codec, "zstd");
  const size_t kl = (rec[14] << 8) | rec[15];
  EXPECT_EQ(beGet32(c.record.data()), c.record.size());
  EXPECT_EQ(0, std::memcmp(c.record.data() + 4, rec.data() + 4, kl - 4)); // header otherwise the original's
  auto back = decompressFrames(c.record.data() + kl, c.record.size() - kl, raw.size());
  EXPECT_EQ(back, raw);
  EXPECT_EQ(c.record[kl], 'Z');
  EXPECT_EQ(c.record[kl + 1], 'S');
}

TEST(FillLayout, IncompressibleBasketIsServedRaw) {
  // ZSTD cannot shrink it; uncompressed fits the slot and saves the reader a decode.
  auto raw = incompressible(4000);
  // The source is "compressed" in name only: frame header claims zstd, payload
  // is zstd of random bytes (larger than raw), which ROOT's writer would have
  // stored raw; construct it anyway to reach the branch.
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 1);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  ASSERT_TRUE(c.error.empty()) << c.error;
  EXPECT_EQ(c.kind, ConvertedBasket::kRaw);
  const size_t kl = (rec[14] << 8) | rec[15];
  EXPECT_EQ(c.record.size(), kl + raw.size()); // fObjlen == fNbytes - fKeylen: raw to ROOT
  EXPECT_EQ(0, std::memcmp(c.record.data() + kl, raw.data(), raw.size()));
}

TEST(FillLayout, KeepsTheOriginalWhenNothingElseFits) {
  auto raw = periodic(200000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 19);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  // A slot exactly the original's length: the level-1 re-encode is larger.
  auto l1 = encodeZstdFrames(raw.data(), raw.size(), 1);
  ASSERT_GT(l1.size(), zs.size());
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), static_cast<uint32_t>(rec.size()), {"zstd"});
  EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
  EXPECT_EQ(c.record, rec);
}

TEST(FillLayout, OriginalForUnlistedStoredOrUndecodable) {
  auto raw = compressible(5000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 3);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  {
    ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"lzma"});
    EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
    EXPECT_EQ(c.codec, "zstd");
    EXPECT_EQ(c.record, rec);
  }
  {
    auto stored = basketRecord(static_cast<int32_t>(raw.size()), raw); // fObjlen == payload
    ConvertedBasket c = convertBasket(stored.data(), stored.size(), 4 * stored.size(), {"zstd"});
    EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
    EXPECT_EQ(c.record, stored);
  }
  {
    auto bad = rec;
    const size_t kl = (bad[14] << 8) | bad[15];
    for (size_t i = kl + 9; i < bad.size(); ++i) bad[i] ^= 0x5A; // frame body garbage
    ConvertedBasket c = convertBasket(bad.data(), bad.size(), 4 * bad.size(), {"zstd"});
    EXPECT_TRUE(c.error.empty());
    EXPECT_EQ(c.kind, ConvertedBasket::kOriginal);
    EXPECT_EQ(c.record, bad); // the reader gets exactly the origin's bytes
  }
  {
    auto notBasket = rec;
    notBasket[35] = 'X'; // class name "TBasket" -> "XBasket"
    EXPECT_FALSE(convertBasket(notBasket.data(), notBasket.size(), 4 * rec.size(), {"zstd"}).error.empty());
    EXPECT_FALSE(convertBasket(rec.data(), rec.size(), static_cast<uint32_t>(rec.size() - 1), {"zstd"})
                     .error.empty());
  }
}

TEST(FillLayout, PlaceInSlotPatchesSeekAndZeroFills) {
  auto raw = compressible(3000);
  auto zs = encodeZstdFrames(raw.data(), raw.size(), 19);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), zs);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  ASSERT_EQ(c.kind, ConvertedBasket::kZstd);
  FillSlot s;
  s.vSeek = 5000000000ull; // past 4 GiB: a 64-bit basket key reaches it
  s.vLen = static_cast<uint32_t>(4 * rec.size());
  std::vector<uint8_t> out(s.vLen, 0xAA);
  std::string err;
  ASSERT_TRUE(placeInSlot(c, s, out.data(), err)) << err;
  EXPECT_EQ(beGet64(&out[18]), s.vSeek);
  EXPECT_EQ(0, std::memcmp(out.data(), c.record.data(), 18));
  EXPECT_EQ(0, std::memcmp(out.data() + 26, c.record.data() + 26, c.record.size() - 26));
  for (size_t i = c.record.size(); i < out.size(); ++i)
    ASSERT_EQ(out[i], 0) << "at " << i;
  // The stored record is position-independent: placing it elsewhere changes only fSeekKey.
  FillSlot s2 = s;
  s2.vSeek = 12345;
  std::vector<uint8_t> out2(s2.vLen, 0xAA);
  ASSERT_TRUE(placeInSlot(c, s2, out2.data(), err));
  EXPECT_EQ(beGet64(&out2[18]), 12345u);
}

TEST(FillLayout, PlaceInSlotRefuses) {
  auto raw = compressible(3000);
  auto rec = basketRecord(static_cast<int32_t>(raw.size()), raw);
  ConvertedBasket c = convertBasket(rec.data(), rec.size(), 4 * rec.size(), {"zstd"});
  std::string err;
  FillSlot s;
  s.vSeek = 100;
  s.vLen = static_cast<uint32_t>(rec.size() - 1);
  std::vector<uint8_t> out(rec.size(), 0);
  EXPECT_FALSE(placeInSlot(c, s, out.data(), err));
  // A 32-bit basket key cannot address a slot past 2 GiB.
  auto narrow = keyHeader(false, "TBasket", "Muon_pt", 0, static_cast<int32_t>(raw.size()), 1000);
  ConvertedBasket n;
  n.record = narrow;
  n.record.insert(n.record.end(), raw.begin(), raw.end());
  bePut32(n.record.data(), static_cast<uint32_t>(n.record.size()));
  s.vLen = static_cast<uint32_t>(n.record.size());
  s.vSeek = 3000000000ull;
  out.assign(s.vLen, 0);
  EXPECT_FALSE(placeInSlot(n, s, out.data(), err));
  s.vSeek = 2000;
  EXPECT_TRUE(placeInSlot(n, s, out.data(), err)) << err;
  EXPECT_EQ(beGet32(&out[18]), 2000u);
}
