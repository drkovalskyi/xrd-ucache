#include "RNTupleMeta.h"

#include "TreeMeta.h"
#include "vendor/xxh3.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace ucache::transpose {
namespace {

// Every multi-byte field below is read through one of these cursors, which
// refuse to run past the buffer. The envelopes are attacker-reachable: a file
// can declare any length it likes, and the walk must fail rather than index
// out of bounds. `bad` is sticky, so a single check at the end of a stage
// covers every read inside it.
struct LE {
  const uint8_t* p = nullptr;
  size_t n = 0;
  size_t at = 0;
  bool bad = false;

  bool need(size_t k) {
    if (bad || k > n || at > n - k) {
      bad = true;
      return false;
    }
    return true;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8) | ((uint32_t)p[at + 2] << 16) |
                 ((uint32_t)p[at + 3] << 24);
    at += 4;
    return v;
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = (uint16_t)(p[at] | ((uint16_t)p[at + 1] << 8));
    at += 2;
    return v;
  }
  uint64_t u64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[at + (size_t)i];
    at += 8;
    return v;
  }
  int32_t i32() { return (int32_t)u32(); }
  int64_t i64() { return (int64_t)u64(); }
  // Strings are a uint32 length followed by the bytes.
  std::string str() {
    uint32_t len = u32();
    if (!need(len)) return {};
    std::string s((const char*)p + at, len);
    at += len;
    return s;
  }
  void skip(uint64_t k) {
    if (k > n || at > n - k) {
      bad = true;
      return;
    }
    at += (size_t)k;
  }
};

uint64_t beU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}
uint16_t beU16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

// A frame is self-describing: a positive int64 is a record frame's size, a
// negative one is a list frame followed by a uint32 item count. Both sizes
// count from the START of the frame, so `o + size` is the next sibling.
struct Frame {
  bool isList = false;
  uint64_t size = 0;
  uint32_t items = 1;
  size_t payload = 0; // first byte after the frame header
};

Frame readFrame(LE& c) {
  Frame f;
  const size_t start = c.at;
  const int64_t sz = c.i64();
  if (sz < 0) {
    f.isList = true;
    f.size = (uint64_t)(-sz);
    f.items = c.u32();
  } else {
    f.size = (uint64_t)sz;
  }
  f.payload = c.at;
  // A frame that does not advance would spin the walk forever.
  if (f.size == 0 || start + f.size > c.n) c.bad = true;
  return f;
}

std::vector<uint8_t> preadRange(int fd, uint64_t off, uint64_t len, uint64_t fileSize) {
  if (len == 0 || off > fileSize || len > fileSize - off) return {};
  std::vector<uint8_t> buf((size_t)len);
  if (::pread(fd, buf.data(), buf.size(), (off_t)off) != (ssize_t)buf.size()) return {};
  return buf;
}

// Read an envelope: `nbytes` on disk at `off`, inflating to `len` if the two
// disagree. The seek points past the enclosing key header, straight at the
// block, exactly as a page locator does.
std::vector<uint8_t> readEnvelope(int fd, uint64_t off, uint64_t nbytes, uint64_t len,
                                  uint64_t fileSize) {
  auto raw = preadRange(fd, off, nbytes, fileSize);
  if (raw.empty()) return {};
  if (nbytes == len) return raw;
  auto out = decompressFrames(raw.data(), raw.size(), len);
  if (out.size() != len) return {};
  return out;
}

} // namespace

bool verifyEnvelope(const uint8_t* p, size_t n, uint16_t expectType, std::string& why) {
  if (n < 16) {
    why = "envelope too short";
    return false;
  }
  LE c{p, n, 0, false};
  const uint64_t word = c.u64();
  const uint16_t type = (uint16_t)(word & 0xFFFF);
  const uint64_t len = word >> 16;
  if (type != expectType) {
    why = "envelope type 0x" + std::to_string(type) + ", expected 0x" + std::to_string(expectType);
    return false;
  }
  // The length field covers the WHOLE envelope, checksum included.
  if (len != n) {
    why = "envelope length " + std::to_string(len) + " != " + std::to_string(n);
    return false;
  }
  uint64_t stored = 0;
  for (int i = 7; i >= 0; --i) stored = (stored << 8) | p[n - 8 + (size_t)i];
  const uint64_t got = xxh3_64(p, n - 8);
  if (got != stored) {
    why = "envelope checksum mismatch";
    return false;
  }
  return true;
}

void sealEnvelope(uint8_t* p, size_t n) {
  if (n < 16) return;
  const uint64_t h = xxh3_64(p, n - 8);
  for (size_t i = 0; i < 8; ++i) p[n - 8 + i] = (uint8_t)((h >> (8 * i)) & 0xFF);
}

RNTupleMeta parseRNTuple(const std::string& path, const std::string& ntuple) {
  RNTupleMeta m;
  auto fail = [&](std::string why) {
    m.error = std::move(why);
    m.ranges.clear();
    m.columns.clear();
    return m;
  };

  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fail("cannot open " + path);
  ContainerMeta cm = parseContainer(fd);
  if (!cm.error.empty()) {
    ::close(fd);
    return fail(cm.error);
  }
  const uint64_t fsz = (uint64_t)cm.fileSize;
  m.fileSize = fsz;
  m.fend = cm.fend;
  m.headerSeekWidth = cm.headerSeekWidth;

  // The anchor hangs off a key whose class is ROOT::RNTuple; that class name
  // is the ONLY thing distinguishing this container from a TTree one.
  KeyInfo anchorKey;
  for (const auto& e : cm.keys)
    if (e.cls == "ROOT::RNTuple" && (ntuple.empty() || e.name == ntuple) &&
        e.cycle >= anchorKey.cycle)
      anchorKey = e;
  if (anchorKey.nbytes == 0) {
    ::close(fd);
    return fail("RNTuple '" + ntuple + "' not found in keys list");
  }

  auto ap = readKeyPayload(fd, anchorKey);
  // 6-byte streamer header, 4 x uint16 version, 7 x uint64 seeks/sizes, and a
  // trailing checksum: 78 bytes. Unlike everything else here the anchor is
  // BIG-endian, the ROOT streamer convention.
  if (ap.size() < 78) {
    ::close(fd);
    return fail("anchor payload too short");
  }
  m.anchor.versionEpoch = beU16(ap.data() + 6);
  m.anchor.versionMajor = beU16(ap.data() + 8);
  m.anchor.versionMinor = beU16(ap.data() + 10);
  m.anchor.versionPatch = beU16(ap.data() + 12);
  m.anchor.seekHeader = beU64(ap.data() + 14);
  m.anchor.nbytesHeader = beU64(ap.data() + 22);
  m.anchor.lenHeader = beU64(ap.data() + 30);
  m.anchor.seekFooter = beU64(ap.data() + 38);
  m.anchor.nbytesFooter = beU64(ap.data() + 46);
  m.anchor.lenFooter = beU64(ap.data() + 54);
  m.anchor.maxKeySize = beU64(ap.data() + 62);
  m.anchor.payloadLength = (uint32_t)ap.size();
  // Only meaningful when the key is stored uncompressed, which is the only
  // case where a writer can patch these fields where they lie.
  if (anchorKey.objlen == anchorKey.nbytes - anchorKey.keylen)
    m.anchor.payloadOffset = (uint64_t)anchorKey.seekkey + anchorKey.keylen;
  {
    // The anchor's own checksum is big-endian and covers payload[6:70] — after
    // the streamer header, through maxKeySize. Patch any seek and it must be
    // recomputed or ROOT refuses the file.
    const uint64_t stored = beU64(ap.data() + 70);
    if (xxh3_64(ap.data() + 6, 64) != stored) {
      ::close(fd);
      return fail("anchor checksum mismatch");
    }
  }

  m.header = readEnvelope(fd, m.anchor.seekHeader, m.anchor.nbytesHeader, m.anchor.lenHeader, fsz);
  m.footer = readEnvelope(fd, m.anchor.seekFooter, m.anchor.nbytesFooter, m.anchor.lenFooter, fsz);
  if (m.header.empty() || m.footer.empty()) {
    ::close(fd);
    return fail("cannot read header/footer envelope");
  }
  std::string why;
  if (!verifyEnvelope(m.header.data(), m.header.size(), kEnvelopeHeader, why)) {
    ::close(fd);
    return fail("header " + why);
  }
  if (!verifyEnvelope(m.footer.data(), m.footer.size(), kEnvelopeFooter, why)) {
    ::close(fd);
    return fail("footer " + why);
  }

  // --- header: name, writer, and the column widths -----------------------
  {
    LE c{m.header.data(), m.header.size(), 16, false}; // envelope word + feature flags
    m.ntupleName = c.str();
    c.str(); // description
    m.writer = c.str();
    std::vector<std::string> fieldNames;
    Frame fl = readFrame(c);
    if (!fl.isList) c.bad = true;
    for (uint32_t i = 0; i < fl.items && !c.bad; ++i) {
      const size_t start = c.at;
      Frame r = readFrame(c);
      c.skip(4 + 4 + 4 + 2 + 2); // versions, parent id, struct role, flags
      fieldNames.push_back(c.str());
      c.at = start + (size_t)r.size;
      if (c.at > c.n) c.bad = true;
    }
    Frame cl = readFrame(c);
    if (!cl.isList) c.bad = true;
    for (uint32_t i = 0; i < cl.items && !c.bad; ++i) {
      const size_t start = c.at;
      Frame r = readFrame(c);
      ColumnInfo ci;
      ci.type = c.u16();
      ci.bitsOnStorage = c.u16();
      ci.fieldId = c.u32();
      if (ci.fieldId < fieldNames.size()) ci.fieldName = fieldNames[ci.fieldId];
      m.columns.push_back(ci);
      c.at = start + (size_t)r.size;
      if (c.at > c.n) c.bad = true;
    }
    if (c.bad) {
      ::close(fd);
      return fail("header schema walk out of bounds");
    }
  }

  // --- footer: cluster groups -> page-list envelope links -----------------
  std::vector<std::pair<uint64_t, std::pair<uint32_t, uint64_t>>> pageLists; // len,(nbytes,off)
  {
    LE c{m.footer.data(), m.footer.size(), 16, false}; // word + header checksum
    c.skip(8);                                         // feature flags
    const size_t extStart = c.at;
    Frame ext = readFrame(c);
    // Skip the whole schema-extension frame. Measured from the frame START,
    // which is where both frame kinds count from — deriving it from the
    // payload offset instead would be wrong by 4 bytes for a list frame.
    c.at = extStart + (size_t)ext.size;
    if (c.at > c.n) c.bad = true;
    Frame gl = readFrame(c);
    if (!gl.isList) c.bad = true;
    for (uint32_t i = 0; i < gl.items && !c.bad; ++i) {
      const size_t start = c.at;
      Frame r = readFrame(c);
      c.skip(8 + 8 + 4); // min entry, entry span, cluster count
      const uint64_t plLen = c.u64();
      const size_t locAt = c.at;
      const int32_t plNbytes = c.i32();
      const uint64_t plOff = c.u64();
      if (plNbytes < 0) {
        // Extended locator: >2 GB or a non-standard payload. Untested against
        // a real file, so refuse rather than guess at it.
        ::close(fd);
        return fail("extended page-list locator unsupported");
      }
      if (i == 0) m.pageListLocatorOffset = (uint32_t)locAt;
      pageLists.push_back({plLen, {(uint32_t)plNbytes, plOff}});
      c.at = start + (size_t)r.size;
      if (c.at > c.n) c.bad = true;
    }
    if (c.bad || pageLists.empty()) {
      ::close(fd);
      return fail("footer cluster-group walk out of bounds");
    }
  }
  if (pageLists.size() != 1) {
    ::close(fd);
    return fail("multiple cluster groups unsupported");
  }

  // --- page list: cluster summaries, then per-column page lists -----------
  m.pageListOffset = pageLists[0].second.second;
  m.pageListNbytes = pageLists[0].second.first;
  m.pageList = readEnvelope(fd, pageLists[0].second.second, pageLists[0].second.first,
                            pageLists[0].first, fsz);
  ::close(fd);
  if (m.pageList.empty()) return fail("cannot read page-list envelope");
  if (!verifyEnvelope(m.pageList.data(), m.pageList.size(), kEnvelopePageList, why))
    return fail("page list " + why);

  {
    LE c{m.pageList.data(), m.pageList.size(), 16, false}; // word + header checksum
    Frame sl = readFrame(c);
    if (!sl.isList) c.bad = true;
    m.nClusters = sl.items;
    for (uint32_t i = 0; i < sl.items && !c.bad; ++i) {
      const size_t start = c.at;
      Frame r = readFrame(c);
      c.skip(8); // first entry
      m.nEntries += c.u64();
      c.at = start + (size_t)r.size;
      if (c.at > c.n) c.bad = true;
    }
    Frame outer = readFrame(c);
    if (!outer.isList) c.bad = true;
    for (uint32_t cluster = 0; cluster < outer.items && !c.bad; ++cluster) {
      Frame cols = readFrame(c);
      if (!cols.isList) c.bad = true;
      for (uint32_t col = 0; col < cols.items && !c.bad; ++col) {
        Frame pages = readFrame(c);
        if (!pages.isList) c.bad = true;
        ColumnRange range;
        range.clusterId = cluster;
        range.columnId = col;
        const uint16_t bits = col < m.columns.size() ? m.columns[col].bitsOnStorage : 0;
        for (uint32_t i = 0; i < pages.items && !c.bad; ++i) {
          PageInfo pi;
          pi.recordOffset = (uint32_t)c.at;
          const int32_t nElem = c.i32();
          // Negative nElements is the has-checksum FLAG, not a count.
          pi.hasChecksum = nElem < 0;
          pi.nElements = (uint32_t)(nElem < 0 ? -(int64_t)nElem : nElem);
          const int32_t size = c.i32();
          if (size < 0) {
            c.bad = true; // extended locator, as above
            break;
          }
          pi.nbytes = (uint32_t)size;
          pi.offset = c.u64();
          // Bit-packed columns are real (booleans are stored at one bit), so
          // this is a ceiling: floor is off by a byte on exactly those.
          pi.uncompressedBytes = bits ? ((uint64_t)pi.nElements * bits + 7) / 8 : 0;
          range.pages.push_back(pi);
        }
        c.skip(8); // element offset
        range.compressionOffset = (uint32_t)c.at;
        range.compressionSettings = c.i32();
        m.ranges.push_back(std::move(range));
      }
    }
    if (c.bad) return fail("page-list walk out of bounds");
  }

  return m;
}

} // namespace ucache::transpose
