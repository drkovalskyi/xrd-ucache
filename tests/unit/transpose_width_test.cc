// Seek-width contract tests: the file header and the root directory record
// carry INDEPENDENT widths, and a 64-bit header over a 32-bit directory record
// is a valid in-the-wild layout (a file that grew past 2 GB after its keys list
// had been written keeps a narrow directory record pointing at an early keys
// list). The parser must accept it, and the overlay builder must never write a
// seek at the wrong site's width — the keys-list record is patched in place
// precisely so that a narrow directory field never has to address the
// extension, which for such a file necessarily starts past 2 GiB.
#include "TreeMeta.h"
#include "Transposer.h"

#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>

#include "TestUtil.h"

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

// A TKey header. `wide` = version + 1000 => 64-bit fSeekKey/fSeekPdir (ROOT
// widens a key by its own placement, independently of the directory record).
std::vector<uint8_t> keyHeader(bool wide, const std::string& cls, const std::string& name,
                              int32_t nbytes, int32_t objlen, int64_t seekkey) {
  const size_t fixed = wide ? 34u : 26u;
  const size_t keylen = fixed + 1 + cls.size() + 1 + name.size() + 1; // + empty title
  std::vector<uint8_t> k(keylen, 0);
  bePut32(k.data(), static_cast<uint32_t>(nbytes));
  bePut16(k.data() + 4, static_cast<uint16_t>(wide ? 1004 : 4));
  bePut32(k.data() + 6, static_cast<uint32_t>(objlen));
  bePut16(k.data() + 14, static_cast<uint16_t>(keylen));
  bePut16(k.data() + 16, 1); // cycle
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
  q += name.size();
  k[q] = 0; // title ""
  return k;
}

// 512-byte crafted TFile header + root directory record. `large` sets the
// 64-bit HEADER layout, `wideDir` the 64-bit DIRECTORY record — deliberately
// independent, which is the whole point of these tests.
std::vector<uint8_t> craftHeader(bool large, bool wideDir, int64_t fend, int64_t keyslistSeek) {
  std::vector<uint8_t> h(512, 0);
  std::memcpy(h.data(), "root", 4);
  bePut32(h.data() + 4, large ? 1062604u : 62604u);
  if (large)
    bePut64(h.data() + 12, static_cast<uint64_t>(fend));
  else
    bePut32(h.data() + 12, static_cast<uint32_t>(fend));
  // Root directory key at fBEGIN = 100 (a narrow key; only its keylen is read).
  auto dk = keyHeader(false, "TFile", "f.root", 100, 0, 100);
  std::memcpy(h.data() + 100, dk.data(), dk.size());
  size_t q = 100 + dk.size();
  h[q++] = 0; // TNamed: fName  ""
  h[q++] = 0; // TNamed: fTitle ""
  bePut16(h.data() + q, static_cast<uint16_t>(wideDir ? 1005 : 5));
  q += 2 + 4 + 4 + 4 + 4;              // version, 2x datime, fNbytesKeys, fNbytesName
  const size_t dw = wideDir ? 8u : 4u; // fSeekDir, fSeekParent, then fSeekKeys
  q += 2 * dw;
  if (wideDir)
    bePut64(h.data() + q, static_cast<uint64_t>(keyslistSeek));
  else
    bePut32(h.data() + q, static_cast<uint32_t>(keyslistSeek));
  return h;
}

std::string writeTemp(const ucache::test::TempDir& td, const std::vector<uint8_t>& bytes) {
  std::string path = td.path() + "/crafted.root";
  FILE* f = ::fopen(path.c_str(), "wb");
  ::fwrite(bytes.data(), 1, bytes.size(), f);
  ::fclose(f);
  return path;
}

// Byte source backed by explicit ranges (same shape as the learner's mock).
struct MockSource : Source {
  std::map<uint64_t, std::vector<uint8_t>> ranges;
  bool has(uint64_t off, uint64_t n) override {
    auto it = ranges.find(off);
    return it != ranges.end() && it->second.size() >= n;
  }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    auto it = ranges.find(off);
    if (it == ranges.end() || it->second.size() < n)
      return false;
    std::memcpy(dst, it->second.data(), n);
    return true;
  }
};

// Geometry of the mixed-width fixture: a 3 GB file (64-bit header) whose keys
// list was written at 512 MB and whose directory record therefore stayed
// 32-bit. Every relocation target lies past 2 GiB.
constexpr int64_t kFend = 3000000000ll;
constexpr int64_t kKeysListSeek = 511823714ll;
constexpr int64_t kTreeKeySeek = 2900000000ll;
constexpr int64_t kBasketSeek = 1000000ll;
constexpr int32_t kBasketPayload = 128;

struct Fixture {
  FileMeta fm;
  MockSource src;
  size_t klNbytes = 0;   // keys-list record length
  size_t klKeylen = 0;   // its own key header length
  size_t basketLen = 0;  // relocated basket record length
};

// `wideBasketKey` = the real-world case (ROOT widened the basket keys); a narrow
// basket key cannot be re-pointed past 2 GiB at all and must fail loudly.
Fixture mixedWidthFixture(bool wideBasketKey) {
  Fixture fx;
  FileMeta& fm = fx.fm;
  fm.large = true;
  fm.headerSeekWidth = 8; // 64-bit header (fEND past 2 GB)
  fm.dirSeekWidth = 4;    // 32-bit root directory record
  fm.fend = kFend;
  fm.keyslistSeek = kKeysListSeek;
  fm.dirSeekKeysOff = 130; // inside the crafted directory record
  fm.treeBlob.assign(128, 0xAB);

  BranchInfo b;
  b.name = "Muon_pt";
  b.writeBasket = 1;
  b.basketSeek = {kBasketSeek};
  b.basketBytes = {0}; // set below, once the record length is known
  b.seekArrayOff = 8;
  b.bytesArrayOff = 40;

  auto bk = keyHeader(wideBasketKey, "TBasket", "Muon_pt", 0, kBasketPayload, kBasketSeek);
  fx.basketLen = bk.size() + kBasketPayload;
  bePut32(bk.data(), static_cast<uint32_t>(fx.basketLen)); // fNbytes
  // fObjlen == fNbytes - fKeylen => stored raw, so the builder relocates it
  // verbatim (no codec needed to exercise the seek-width logic).
  std::vector<uint8_t> basket = bk;
  basket.resize(fx.basketLen, 0x5A);
  b.basketBytes = {static_cast<int32_t>(fx.basketLen)};
  fm.branches.push_back(b);
  fx.src.ranges[kBasketSeek] = basket;

  // Live tree key (wide: it sits past 2 GB in a 3 GB file).
  auto tk = keyHeader(true, "TTree", "Events", 4096, 8192, kTreeKeySeek);
  fm.treeKey.nbytes = 4096;
  fm.treeKey.objlen = 8192;
  fm.treeKey.keylen = static_cast<uint16_t>(tk.size());
  fm.treeKey.ver = 1004;
  fm.treeKey.cycle = 1;
  fm.treeKey.seekkey = kTreeKeySeek;
  fm.treeKey.cls = "TTree";
  fm.treeKey.name = "Events";
  std::vector<uint8_t> treeRec = tk;
  treeRec.resize(4096, 0);
  fx.src.ranges[kTreeKeySeek] = treeRec;

  // Keys-list record: own key header, nkeys, one entry for the live tree.
  auto own = keyHeader(true, "", "", 0, 0, kKeysListSeek);
  auto entry = keyHeader(true, "TTree", "Events", 4096, 8192, kTreeKeySeek);
  fx.klKeylen = own.size();
  fx.klNbytes = own.size() + 4 + entry.size();
  bePut32(own.data(), static_cast<uint32_t>(fx.klNbytes));
  std::vector<uint8_t> kl = own;
  kl.resize(own.size() + 4);
  bePut32(kl.data() + own.size(), 1); // nkeys
  kl.insert(kl.end(), entry.begin(), entry.end());
  kl.resize(512, 0); // the builder probes 512 bytes before reading fNbytes
  fx.src.ranges[kKeysListSeek] = kl;
  return fx;
}

} // namespace

// ---------------------------------------------------------------- parser

TEST(TransposeWidth, WideHeaderOverNarrowDirectoryIsAccepted) {
  ucache::test::TempDir td;
  auto path = writeTemp(td, craftHeader(/*large=*/true, /*wideDir=*/false, kFend, kKeysListSeek));
  FileMeta fm = parseFile(path);
  // The mismatch itself must no longer be a verdict: parsing gets past the
  // header/directory walk and only stops for want of the (absent) keys list.
  EXPECT_EQ(fm.error, "cannot read keys list");
  EXPECT_TRUE(fm.large);
  EXPECT_EQ(fm.headerSeekWidth, 8);
  EXPECT_EQ(fm.dirSeekWidth, 4);
  EXPECT_EQ(fm.fend, kFend);
  EXPECT_EQ(fm.keyslistSeek, kKeysListSeek); // read at the DIRECTORY's width
}

TEST(TransposeWidth, MatchingWidthsStillParse) {
  ucache::test::TempDir td;
  { // both wide
    auto p = writeTemp(td, craftHeader(true, true, kFend, kKeysListSeek));
    FileMeta fm = parseFile(p);
    EXPECT_EQ(fm.error, "cannot read keys list");
    EXPECT_EQ(fm.headerSeekWidth, 8);
    EXPECT_EQ(fm.dirSeekWidth, 8);
    EXPECT_EQ(fm.fend, kFend);
    EXPECT_EQ(fm.keyslistSeek, kKeysListSeek);
  }
  { // both narrow (the small-file layout)
    auto p = writeTemp(td, craftHeader(false, false, 1000000, 900000));
    FileMeta fm = parseFile(p);
    EXPECT_EQ(fm.error, "cannot read keys list");
    EXPECT_EQ(fm.headerSeekWidth, 4);
    EXPECT_EQ(fm.dirSeekWidth, 4);
    EXPECT_EQ(fm.fend, 1000000);
    EXPECT_EQ(fm.keyslistSeek, 900000);
  }
}

TEST(TransposeWidth, NarrowHeaderOverWideDirectoryIsAccepted) {
  // The converse mismatch (a 32-bit header with a 64-bit directory record) is
  // just as much a per-site fact; neither is a reason to refuse the file.
  ucache::test::TempDir td;
  auto p = writeTemp(td, craftHeader(/*large=*/false, /*wideDir=*/true, 1000000, 900000));
  FileMeta fm = parseFile(p);
  EXPECT_EQ(fm.error, "cannot read keys list");
  EXPECT_EQ(fm.headerSeekWidth, 4);
  EXPECT_EQ(fm.dirSeekWidth, 8);
  EXPECT_EQ(fm.keyslistSeek, 900000);
}

// ---------------------------------------------------------------- builder

TEST(TransposeWidth, MixedWidthOverlayLeavesTheDirectoryUntouched) {
  Fixture fx = mixedWidthFixture(/*wideBasketKey=*/true);
  Overlay ov = buildOverlay(fx.fm, fx.src, {"Muon_pt"});
  ASSERT_EQ(ov.error, "");
  ASSERT_EQ(ov.meta.extents.size(), 3u);

  // 1. fEND, at the HEADER's width — 8 bytes, not the directory's 4.
  EXPECT_EQ(ov.meta.extents[0].virtOff, 12u);
  EXPECT_EQ(ov.meta.extents[0].len, 8u);
  EXPECT_EQ(ov.meta.extents[0].tdataOff, 0u);
  // 2. the keys-list record, patched IN PLACE at its original offset.
  EXPECT_EQ(ov.meta.extents[1].virtOff, static_cast<uint64_t>(kKeysListSeek));
  EXPECT_EQ(ov.meta.extents[1].len, fx.klNbytes);
  EXPECT_EQ(ov.meta.extents[1].tdataOff, 8u);
  // 3. the extension, appended at fEND.
  EXPECT_EQ(ov.meta.extents[2].virtOff, static_cast<uint64_t>(kFend));
  EXPECT_EQ(ov.meta.extents[2].tdataOff, 8u + fx.klNbytes);

  const uint64_t newFend = kFend + ov.meta.extents[2].len;
  EXPECT_EQ(ov.meta.virtualSize, newFend);
  EXPECT_EQ(beGet64(ov.tdata.data()), newFend); // full 64-bit fEND

  // Nothing is written into the root directory record: a 4-byte fSeekKeys
  // could not have expressed a target past 2 GiB, and now it needn't.
  for (const auto& e : ov.meta.extents)
    EXPECT_FALSE(e.virtOff <= fx.fm.dirSeekKeysOff &&
                 fx.fm.dirSeekKeysOff < e.virtOff + e.len);

  // The keys-list record keeps its own fSeekKey (it did not move) and its live
  // tree entry now points at the relocated metadata key, past 2 GiB.
  const uint8_t* klWin = ov.tdata.data() + 8;
  EXPECT_EQ(beGet64(klWin + 18), static_cast<uint64_t>(kKeysListSeek));
  const uint8_t* entry = klWin + fx.klKeylen + 4;
  const uint64_t newMetaSeek = static_cast<uint64_t>(kFend) + fx.basketLen;
  EXPECT_EQ(beGet64(entry + 18), newMetaSeek);
  EXPECT_GT(newMetaSeek, 1ull << 31);
  EXPECT_EQ(beGet32(entry), static_cast<uint32_t>(fx.fm.treeKey.keylen) + ov.metaStoredBytes);

  // Relocations are punchable; in-place patch windows are not relocations, so
  // the keys list is not offered up as superseded.
  for (const auto& r : ov.meta.superseded)
    EXPECT_NE(r.off, static_cast<uint64_t>(kKeysListSeek));
  EXPECT_EQ(ov.baskets, 1u);
  EXPECT_EQ(ov.verbatim, 1u);
}

TEST(TransposeWidth, NarrowKeyCannotBeRelocatedPastTwoGiB) {
  // A 32-bit key has nowhere to point once the extension starts past 2 GiB.
  // The builder must say so, never truncate the seek into the record.
  Fixture fx = mixedWidthFixture(/*wideBasketKey=*/false);
  Overlay ov = buildOverlay(fx.fm, fx.src, {"Muon_pt"});
  EXPECT_EQ(ov.error, "32-bit key cannot point past 2 GiB");
  EXPECT_TRUE(ov.meta.extents.empty());
}
