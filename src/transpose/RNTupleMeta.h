// ROOT-free structural parser for RNTuple containers.
//
// Extracts exactly what recompression needs: the anchor, the schema (so a
// column can be named), and the page table (so every page can be located,
// decoded and relocated). No ROOT dependency, same as TreeMeta.
//
// The TFile-level walk — header geometry, root directory, keys list, TKey
// records, ROOT block decompression — is SHARED with TreeMeta and reused from
// there. An RNTuple container differs from a TTree one only in the class name
// of the key it hangs off: `ROOT::RNTuple` rather than `TTree`.
//
// FORMAT NOTES THAT ARE EASY TO GET WRONG. Each of these was found by getting
// it wrong first; every one produces a file that looks written and fails only
// when something reads it.
//
//   * The anchor is BIG-endian (ROOT streamer convention) while every envelope
//     and page record is LITTLE-endian. Mixing them yields plausible garbage.
//   * The anchor carries a trailing xxh3-64, big-endian, over payload[6:70]
//     (i.e. after the streamer header, through maxKeySize). Patching any seek
//     or size field invalidates it and ROOT then refuses the file.
//   * Header and footer are ROOT-COMPRESSED blocks. The anchor records both the
//     compressed (`nbytes`) and uncompressed (`len`) size of each.
//   * A page record is SIXTEEN bytes: int32 nElements, then the locator as
//     int32 size + uint64 offset. Advancing 12 does not fail loudly, it
//     fabricates plausible records for everything after the first.
//   * nElements is stored NEGATIVE when the page carries a checksum. It is a
//     flag, not a count; take the absolute value.
//   * The page checksum sits PAST the locator's size, which covers the block
//     only: a page occupies size+8 bytes, with an 8-byte LITTLE-endian xxh3 of
//     the block immediately after it.
//   * A page's uncompressed size is ceil(nElements * bitsOnStorage / 8). Bit
//     packed columns are real — NanoAOD stores booleans at one bit — and floor
//     is off by one byte on exactly those.
//   * Frames are self-describing: a positive int64 is a record frame's size, a
//     negative one is a list frame followed by a uint32 item count.
//
// Thread-safety: pure functions over caller-owned buffers; no shared state.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ucache::transpose {

// Envelope type ids, from the format specification.
inline constexpr uint16_t kEnvelopeHeader = 0x01;
inline constexpr uint16_t kEnvelopeFooter = 0x02;
inline constexpr uint16_t kEnvelopePageList = 0x03;

// The `ROOT::RNTuple` anchor: what the key payload holds, in order.
struct RNTupleAnchor {
  uint16_t versionEpoch = 0, versionMajor = 0, versionMinor = 0, versionPatch = 0;
  uint64_t seekHeader = 0, nbytesHeader = 0, lenHeader = 0;
  uint64_t seekFooter = 0, nbytesFooter = 0, lenFooter = 0;
  uint64_t maxKeySize = 0;
  // Where the payload sits in the file, so a writer can patch it in place.
  uint64_t payloadOffset = 0;
  uint32_t payloadLength = 0;
};

// One physical column, from the header envelope's column descriptors.
struct ColumnInfo {
  uint16_t type = 0;
  uint16_t bitsOnStorage = 0;
  uint32_t fieldId = 0;
  std::string fieldName; // joined from the field descriptors
};

// One page, from a page-list envelope. `offset`/`nbytes` locate the BLOCK;
// add 8 bytes for the checksum when `hasChecksum`.
struct PageInfo {
  uint64_t offset = 0;
  uint32_t nbytes = 0;      // block only, checksum NOT included
  uint32_t nElements = 0;   // absolute value; sign carried in hasChecksum
  bool hasChecksum = false;
  uint64_t uncompressedBytes = 0; // ceil(nElements * bits / 8)
  // Where this record sits inside the decompressed page-list envelope, so a
  // writer can patch the locator without re-deriving the walk.
  uint32_t recordOffset = 0;
};

// The pages of one column within one cluster, plus the compression setting
// that applies to all of them (ROOT's usual algorithm*100 + level).
struct ColumnRange {
  uint32_t clusterId = 0;
  uint32_t columnId = 0;
  int32_t compressionSettings = 0;
  uint32_t compressionOffset = 0; // patch site inside the page-list envelope
  std::vector<PageInfo> pages;
};

struct RNTupleMeta {
  std::string error;    // non-empty => nothing below is meaningful
  uint64_t fileSize = 0;
  int64_t fend = 0;
  // fEND's width in the file header. A file that outgrew 2 GB after its keys
  // list was written keeps narrow records elsewhere, so this width is valid
  // only at fEND's own site — and a rewrite that pushes fEND past 2 GB under a
  // narrow header cannot be patched in place at all.
  int headerSeekWidth = 8;
  std::string ntupleName;
  std::string writer;   // e.g. "ROOT v6.36.02"; self-identifying, do not assume
  RNTupleAnchor anchor;
  std::vector<ColumnInfo> columns;
  std::vector<ColumnRange> ranges;
  uint64_t nEntries = 0;
  uint32_t nClusters = 0;
  // Decompressed envelopes, retained because a writer must patch and re-seal
  // them rather than rebuild them from scratch.
  std::vector<uint8_t> header, footer, pageList;
  // Where the footer records the page-list locator (length, nbytes, offset).
  uint32_t pageListLocatorOffset = 0;
  // ...and where that locator currently points, so a rewrite can mark the
  // superseded copy as reclaimable.
  uint64_t pageListOffset = 0;
  uint32_t pageListNbytes = 0;
};

// Parse the RNTuple named `ntuple` out of `path`. On any malformed or
// unsupported input the result carries `error` and the caller fails open —
// never guesses, same contract as TreeMeta::parse.
RNTupleMeta parseRNTuple(const std::string& path, const std::string& ntuple);

// Verify and strip an envelope: checks the type, that the length field matches
// the buffer, and the trailing xxh3-64. Returns false with `why` set on any
// mismatch.
bool verifyEnvelope(const uint8_t* p, size_t n, uint16_t expectType, std::string& why);

// Recompute an envelope's trailing xxh3-64 in place after its contents change.
void sealEnvelope(uint8_t* p, size_t n);

// The hash itself is `ucache::xxh3_64` from vendor/xxh3.h — pages need it too.

} // namespace ucache::transpose
