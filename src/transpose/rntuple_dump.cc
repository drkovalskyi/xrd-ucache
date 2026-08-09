// Dump what the ROOT-free RNTuple parser sees, as JSON, so it can be diffed
// against ROOT's own descriptor. Every number here has an equivalent in
// RNTupleReader's descriptor, which is the point: this parser is checked
// against ROOT rather than against itself.
#include "RNTupleMeta.h"

#include <cstdio>
#include <map>
#include <set>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE.root [NTUPLE]\n", argv[0]);
    return 2;
  }
  const std::string ntuple = argc > 2 ? argv[2] : "";
  auto m = ucache::transpose::parseRNTuple(argv[1], ntuple);
  if (!m.error.empty()) {
    std::printf("{\"error\":\"%s\"}\n", m.error.c_str());
    return 1;
  }

  size_t pages = 0, checksummed = 0, zeroSized = 0;
  std::map<int32_t, size_t> comp;
  std::set<uint64_t> offsets;
  for (const auto& r : m.ranges) {
    pages += r.pages.size();
    comp[r.compressionSettings]++;
    for (const auto& p : r.pages) {
      if (p.hasChecksum) ++checksummed;
      if (p.nbytes == 0) ++zeroSized;
      offsets.insert(p.offset);
    }
  }

  std::printf("{\"ntuple\":\"%s\",\"writer\":\"%s\",", m.ntupleName.c_str(), m.writer.c_str());
  std::printf("\"anchor_version\":\"%u.%u.%u.%u\",", m.anchor.versionEpoch, m.anchor.versionMajor,
              m.anchor.versionMinor, m.anchor.versionPatch);
  std::printf("\"entries\":%llu,\"clusters\":%u,\"columns\":%zu,",
              (unsigned long long)m.nEntries, m.nClusters, m.columns.size());
  std::printf("\"pages\":%zu,\"pages_with_checksum\":%zu,\"ranges\":%zu,", pages, checksummed,
              m.ranges.size());
  // Distinct offsets below the page count means pages share bytes on disk.
  std::printf("\"distinct_page_offsets\":%zu,\"zero_sized_pages\":%zu,", offsets.size(), zeroSized);
  std::printf("\"compression\":{");
  bool first = true;
  for (const auto& kv : comp) {
    std::printf("%s\"%d\":%zu", first ? "" : ",", kv.first, kv.second);
    first = false;
  }
  std::printf("},\"header_bytes\":%zu,\"footer_bytes\":%zu,\"pagelist_bytes\":%zu",
              m.header.size(), m.footer.size(), m.pageList.size());
  // Where the metadata physically lives, so it can be checked for presence in
  // a partially cached image — the envelopes are what a rebuild needs first.
  std::printf(",\"meta_extents\":{\"header\":[%llu,%llu],\"footer\":[%llu,%llu],"
              "\"pagelist\":[%llu,%llu],\"anchor\":[%llu,%u]}",
              (unsigned long long)m.anchor.seekHeader, (unsigned long long)m.anchor.nbytesHeader,
              (unsigned long long)m.anchor.seekFooter, (unsigned long long)m.anchor.nbytesFooter,
              (unsigned long long)m.pageListOffset, (unsigned long long)m.pageListNbytes,
              (unsigned long long)m.anchor.payloadOffset, m.anchor.payloadLength);
  if (!m.columns.empty())
    std::printf(",\"first_column\":{\"field\":\"%s\",\"bits\":%u}", m.columns[0].fieldName.c_str(),
                m.columns[0].bitsOnStorage);
  std::printf("}\n");
  return 0;
}
