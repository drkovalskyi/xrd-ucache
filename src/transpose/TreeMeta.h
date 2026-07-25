// ROOT-free structural parser for the transposer:
// extracts exactly what replica building needs from a TFile — header
// geometry, keys, the keys-list, and the streamed TTree's per-branch basket
// tables (name, fWriteBasket, fMaxBaskets, fBasketSeek/fBasketBytes arrays,
// leaf class/title, counter leaf) — with NO ROOT dependency.
//
// The walk is VERSION-EXACT and deliberately narrow: TTree v20, TBranch v13,
// TLeaf v2 (+TLeaf?_v1 subclasses), TObjArray v3 — the layouts present in
// every supported input (verified against an uproot reference dump, whose
// output is this parser's differential ground truth). Anything else
// => parse() fails with a reason: the caller fails open (no transpose),
// never guesses.
//
// Thread-safety: pure functions over caller-owned buffers; no shared state.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucache::transpose {

struct KeyInfo {
  int32_t nbytes = 0;
  uint16_t ver = 0;
  int32_t objlen = 0;
  uint16_t keylen = 0;
  uint16_t cycle = 0;
  int64_t seekkey = 0;
  std::string cls, name, title;
};
// Parses a TKey header at `off`; nullopt on malformed/out-of-bounds.
std::optional<KeyInfo> parseKey(const uint8_t* p, size_t n, uint64_t off);

// Decompress a ROOT multi-frame compression container (XZ/ZL/ZS/L4) into
// exactly `objlen` bytes; empty vector on failure or unknown frames.
std::vector<uint8_t> decompressFrames(const uint8_t* payload, size_t n, uint64_t objlen);

struct BranchInfo {
  std::string name;
  int32_t writeBasket = 0;
  uint32_t maxBaskets = 0;
  std::vector<int64_t> basketSeek;  // [writeBasket]
  std::vector<int32_t> basketBytes; // [writeBasket]
  std::string leafClass;            // e.g. "TLeafF"
  std::string leafTitle;            // e.g. "Muon_pt[nMuon]/F"
  std::string leafCount;            // counter leaf name ("" = scalar)
  // Blob offsets of the live array values (for the transcoder's patches).
  uint64_t seekArrayOff = 0;  // fBasketSeek[0] within the decompressed blob
  uint64_t bytesArrayOff = 0; // fBasketBytes[0]
};

struct FileMeta {
  bool large = false; // 64-bit header layout (file version > 1000000)
  int64_t fend = 0;
  int64_t keyslistSeek = 0;
  uint64_t dirSeekKeysOff = 0; // file offset of the directory's fSeekKeys
  // The file header and the root-directory record carry INDEPENDENT seek
  // widths, and they are not required to agree. The header goes 64-bit once
  // fEND passes 2 GB; the root directory record goes 64-bit only if its own
  // three seeks (fSeekDir/fSeekParent/fSeekKeys) need it. A file that grew
  // past 2 GB after its keys list had already been written keeps a narrow
  // directory record pointing at an early keys list — a legitimate in-the-wild
  // layout. Each width is therefore only valid at its own site.
  int headerSeekWidth = 8; // fEND/fSeekFree in the header
  int dirSeekWidth = 8;    // fSeekDir/fSeekParent/fSeekKeys in the directory
  KeyInfo treeKey;                // the live (highest-cycle) tree key
  std::vector<uint8_t> treeBlob;  // decompressed tree metadata
  std::vector<BranchInfo> branches;
  std::string error; // non-empty => parse failed (fail open, no transpose)
};

// Parse `path` (whole file is pread as needed; the tree metadata blob is
// decompressed in memory). On any unsupported version/layout, returns a
// FileMeta with `error` set and no branches.
FileMeta parseFile(const std::string& path, const std::string& tree = "Events");

} // namespace ucache::transpose
