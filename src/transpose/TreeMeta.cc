#include "TreeMeta.h"

#include <cstring>
#include <fcntl.h>
#include <type_traits>
#include <unistd.h>

#include <lzma.h>
#include <zlib.h>
#include <zstd.h>
#ifdef UCACHE_HAVE_LZ4
#include <lz4.h>
#endif

// Layout sources of record: uproot 5.6 models (TTree v20 _ttree20_format1,
// TBranch v13 _tbranch13_format{1,2}, TLeaf v2 _tleaf2_format0, TIOFeatures,
// TObjArray) — the same independent implementation that verified the spike.
namespace ucache::transpose {
namespace {

constexpr uint32_t kByteCountMask = 0x40000000;
constexpr uint32_t kNewClassTag = 0xFFFFFFFF;
constexpr uint32_t kClassMask = 0x80000000;
constexpr uint32_t kIsReferenced = 1u << 4;

template <typename T> T beGet(const uint8_t* p) {
  using U = std::make_unsigned_t<T>;
  U v{};
  for (size_t i = 0; i < sizeof(T); ++i)
    v = static_cast<U>((v << 8) | p[i]);
  T out;
  std::memcpy(&out, &v, sizeof(T));
  return out;
}

// Big-endian cursor over the decompressed metadata blob. All read methods
// set fail_ instead of throwing (no exceptions cross the plugin ABI).
struct Cur {
  const uint8_t* p;
  size_t n;
  size_t at = 0;
  bool fail_ = false;
  std::string why;
  // Back-reference bias is EXACT, not learned: ROOT's TBufferFile maps
  // streamed objects at (position within the full key record + kMapOffset=2),
  // and this blob excludes the key header — so an object first streamed at
  // blob offset P is referenced as P + keylen + 2, and a class tag written at
  // blob offset T is referenced as T + keylen + 2 (we record the class NAME
  // position, tag field + 4, hence the -4 below).
  uint16_t keylen = 0;

  bool need(size_t k) {
    if (fail_ || at + k > n) {
      if (!fail_) {
        fail_ = true;
        why = "truncated at " + std::to_string(at);
      }
      return false;
    }
    return true;
  }
  template <typename T> T get() {
    if (!need(sizeof(T)))
      return T{};
    T v = beGet<T>(p + at);
    at += sizeof(T);
    return v;
  }
  void skip(size_t k) { need(k) ? void(at += k) : void(); }
  std::string tstring() {
    uint32_t len = get<uint8_t>();
    if (len == 255)
      len = get<uint32_t>();
    if (!need(len))
      return {};
    std::string s(reinterpret_cast<const char*>(p + at), len);
    at += len;
    return s;
  }
  // [bytecount|kByteCountMask][version]; returns end-of-object position.
  size_t objHeader(uint16_t& ver, const char* what) {
    uint32_t bc = get<uint32_t>();
    if (!(bc & kByteCountMask)) {
      fail_ = true;
      why = std::string(what) + ": missing byte count";
      return 0;
    }
    size_t end = at + (bc & ~kByteCountMask);
    ver = get<uint16_t>();
    return end;
  }
  void skipObj(const char* what) {
    uint16_t v;
    size_t end = objHeader(v, what);
    if (!fail_)
      need(end - at) ? void(at = end) : void();
  }
  void tobject() {
    get<uint16_t>(); // version
    get<uint32_t>(); // fUniqueID
    uint32_t bits = get<uint32_t>();
    if (bits & kIsReferenced)
      get<uint16_t>();
  }
};

// One entry of a TObjArray: either null, a back-reference (`ref` set), or an
// inline object (`cls` + body up to `end`; cursor left at the body start).
struct AnyObj {
  bool null = false;
  uint32_t ref = 0;   // nonzero => back-reference to an earlier object
  std::string cls;    // resolved class name for inline objects
  size_t end = 0;     // one past the object (skip target)
  size_t tagPos = 0;  // blob offset of the first u32 (registration anchor)
};

// read_object_any per TBuffer conventions; classRefs maps the blob offset of
// a kNewClassTag (+ kMapOffset bias handled by the caller's registry) to its
// class name for tag back-references.
AnyObj readAny(Cur& c, std::vector<std::pair<uint32_t, std::string>>& classRefs) {
  AnyObj o;
  o.tagPos = c.at;
  uint32_t first = c.get<uint32_t>();
  if (first == 0) {
    o.null = true;
    return o;
  }
  if (!(first & kByteCountMask)) {
    o.ref = first; // back-reference to an already-streamed object
    return o;
  }
  o.end = c.at + (first & ~kByteCountMask);
  uint32_t tag = c.get<uint32_t>();
  if (tag == kNewClassTag) {
    size_t namePos = c.at;
    std::string cls;
    while (!c.fail_ && c.at < c.n && c.p[c.at] != 0)
      cls.push_back(static_cast<char>(c.p[c.at++]));
    c.skip(1); // NUL
    o.cls = cls;
    // ROOT registers the class at (tag position + kMapOffset); empirically
    // the reference value equals namePos - 4 + 2... calibrated below via the
    // registry the caller maintains keyed on tagPos-relative offsets.
    classRefs.emplace_back(static_cast<uint32_t>(namePos), cls);
  } else if (tag & kClassMask) {
    const int64_t off = static_cast<int64_t>(tag & ~kClassMask);
    for (auto& [pos, cls] : classRefs)
      if (static_cast<int64_t>(pos) - 4 + c.keylen + 2 == off) {
        o.cls = cls;
        break;
      }
    if (o.cls.empty())
      o.cls = "?ref";
  } else {
    c.fail_ = true;
    c.why = "object tag without class at " + std::to_string(o.tagPos);
  }
  return o;
}

} // namespace

std::optional<KeyInfo> parseKey(const uint8_t* p, size_t n, uint64_t off) {
  if (off + 34 > n)
    return std::nullopt;
  const uint8_t* k = p + off;
  KeyInfo o;
  o.nbytes = beGet<int32_t>(k);
  o.ver = beGet<uint16_t>(k + 4);
  o.objlen = beGet<int32_t>(k + 6);
  o.keylen = beGet<uint16_t>(k + 14);
  o.cycle = beGet<uint16_t>(k + 16);
  size_t q;
  if (o.ver > 1000) {
    o.seekkey = beGet<int64_t>(k + 18);
    q = 34;
  } else {
    o.seekkey = beGet<int32_t>(k + 18);
    q = 26;
  }
  auto str = [&](std::string& s) {
    if (off + q >= n)
      return false;
    uint8_t len = k[q++];
    if (off + q + len > n)
      return false;
    s.assign(reinterpret_cast<const char*>(k + q), len);
    q += len;
    return true;
  };
  if (!str(o.cls) || !str(o.name) || !str(o.title))
    return std::nullopt;
  return o;
}

std::vector<uint8_t> decompressFrames(const uint8_t* payload, size_t n, uint64_t objlen) {
  std::vector<uint8_t> out;
  // objlen is attacker-influenceable: cap the upfront reservation (growth past
  // it amortizes per frame) so a hostile value cannot force a giant allocation.
  out.reserve(std::min<uint64_t>(objlen, 64ull << 20));
  size_t p = 0;
  while (out.size() < objlen) {
    if (p + 9 > n)
      return {};
    const char m0 = static_cast<char>(payload[p]), m1 = static_cast<char>(payload[p + 1]);
    uint64_t csize = payload[p + 3] | payload[p + 4] << 8 | payload[p + 5] << 16;
    uint64_t usize = payload[p + 6] | payload[p + 7] << 8 | payload[p + 8] << 16;
    if (p + 9 + csize > n)
      return {};
    const uint8_t* blob = payload + p + 9;
    size_t pre = out.size();
    out.resize(pre + usize);
    if (m0 == 'X' && m1 == 'Z') {
      uint64_t memlim = UINT64_MAX;
      size_t inPos = 0, outPos = 0;
      if (lzma_stream_buffer_decode(&memlim, 0, nullptr, blob, &inPos, csize,
                                    out.data() + pre, &outPos, usize) != LZMA_OK ||
          outPos != usize)
        return {};
    } else if (m0 == 'Z' && m1 == 'L') {
      uLongf dl = usize;
      if (::uncompress(out.data() + pre, &dl, blob, csize) != Z_OK || dl != usize)
        return {};
    } else if (m0 == 'Z' && m1 == 'S') {
      if (ZSTD_decompress(out.data() + pre, usize, blob, csize) != usize)
        return {};
    } else if (m0 == 'L' && m1 == '4') {
#ifdef UCACHE_HAVE_LZ4
      if (csize < 8 ||
          LZ4_decompress_safe(reinterpret_cast<const char*>(blob + 8),
                              reinterpret_cast<char*>(out.data() + pre),
                              static_cast<int>(csize - 8),
                              static_cast<int>(usize)) != static_cast<int>(usize))
        return {};
#else
      return {}; // built without lz4: refuse (fail-open, no transpose)
#endif
    } else {
      return {};
    }
    p += 9 + csize;
  }
  return out.size() == objlen ? out : std::vector<uint8_t>{};
}

namespace {

// Streamed-TObjArray walk calling `element` for each non-null inline object;
// the callback must leave the cursor at o.end.
template <typename F>
bool objArray(Cur& c, std::vector<std::pair<uint32_t, std::string>>& classRefs,
              const char* what, F element) {
  uint16_t ver;
  size_t end = c.objHeader(ver, what);
  if (c.fail_)
    return false;
  if (ver != 3) {
    c.fail_ = true;
    c.why = std::string(what) + ": TObjArray v" + std::to_string(ver) + " unsupported";
    return false;
  }
  c.tobject();
  c.tstring(); // fName
  int32_t size = c.get<int32_t>();
  c.get<int32_t>(); // fLowerBound
  for (int32_t i = 0; i < size && !c.fail_; ++i) {
    AnyObj o = readAny(c, classRefs);
    if (o.null || o.ref)
      continue;
    if (!element(o))
      return false;
    if (!c.fail_)
      c.at = o.end;
  }
  c.at = end;
  return !c.fail_;
}

// TLeaf v2 base + name/title + counter back-ref; consumed via outer bytecount.
bool parseLeaf(Cur& c, const AnyObj& o, BranchInfo& b,
               std::vector<std::pair<uint32_t, std::string>>& classRefs,
               std::vector<std::pair<uint32_t, std::string>>& leafRegistry) {
  b.leafClass = o.cls;
  uint16_t lver;
  c.objHeader(lver, "TLeafX"); // subclass header (v1)
  uint16_t bver;
  size_t bend = c.objHeader(bver, "TLeaf");
  if (c.fail_)
    return false;
  if (bver != 2) {
    c.fail_ = true;
    c.why = "TLeaf v" + std::to_string(bver) + " unsupported";
    return false;
  }
  { // TNamed: name + title
    uint16_t nv;
    c.objHeader(nv, "TNamed");
    c.tobject();
    std::string name = c.tstring();
    b.leafTitle = c.tstring();
    // Register this leaf for counter back-references: ROOT maps objects at
    // (buffer position of the object's first u32) + keylen + kMapOffset;
    // we store tagPos and resolve with the calibrated bias below.
    leafRegistry.emplace_back(static_cast<uint32_t>(o.tagPos), name);
  }
  c.skip(4 * 3 + 2); // fLen, fLenType, fOffset (i4 each), fIsRange, fIsUnsigned
  AnyObj cnt = readAny(c, classRefs);
  if (cnt.ref) {
    // Exact resolution (header comment): referenced object was first
    // streamed at blob offset `pos`, so its reference is pos + keylen + 2.
    for (auto& [pos, name] : leafRegistry)
      if (static_cast<int64_t>(pos) + c.keylen + 2 == static_cast<int64_t>(cnt.ref)) {
        b.leafCount = name;
        break;
      }
  } else if (!cnt.null && !c.fail_) {
    c.at = cnt.end; // inline counter object (first reference) — not expected
  }
  (void)bend;
  return !c.fail_;
}

// TBranch v13; the caller (objArray) repositions to the outer bytecount end.
bool parseBranch(Cur& c, FileMeta& fm,
                 std::vector<std::pair<uint32_t, std::string>>& classRefs,
                 std::vector<std::pair<uint32_t, std::string>>& leafRegistry) {
  uint16_t ver;
  size_t end = c.objHeader(ver, "TBranch");
  if (c.fail_)
    return false;
  if (ver != 13) {
    c.fail_ = true;
    c.why = "TBranch v" + std::to_string(ver) + " unsupported";
    return false;
  }
  BranchInfo b;
  { // TNamed (need fName)
    uint16_t nv;
    c.objHeader(nv, "TNamed");
    c.tobject();
    b.name = c.tstring();
    c.tstring(); // title
  }
  c.skipObj("TAttFill");
  c.skip(4 * 4 + 8);         // fCompress..fWriteBasket (i4 x4), fEntryNumber (q)
  b.writeBasket = beGet<int32_t>(c.p + c.at - 12);
  c.skipObj("TIOFeatures");
  c.skip(4);                 // fOffset
  b.maxBaskets = c.get<uint32_t>();
  c.skip(4 + 8 * 4);         // fSplitLevel, fEntries..fZipBytes (q x4)
  if (!objArray(c, classRefs, "fBranches(sub)", [&](const AnyObj&) {
        c.fail_ = true;
        c.why = "sub-branches unsupported (flat trees only)";
        return false;
      }))
    return false;
  if (!objArray(c, classRefs, "fLeaves", [&](const AnyObj& lo) {
        return parseLeaf(c, lo, b, classRefs, leafRegistry);
      }))
    return false;
  { // fBaskets: skip whole array via byte count
    c.skipObj("fBaskets");
  }
  if (b.maxBaskets > (1u << 24) || c.fail_) {
    c.fail_ = true;
    if (c.why.empty())
      c.why = "implausible fMaxBaskets";
    return false;
  }
  c.skip(1);
  b.bytesArrayOff = c.at;
  b.basketBytes.resize(b.writeBasket);
  for (int32_t i = 0; i < b.writeBasket; ++i)
    b.basketBytes[i] = beGet<int32_t>(c.p + c.at + 4 * i);
  c.skip(4ull * b.maxBaskets);
  c.skip(1);
  c.skip(8ull * b.maxBaskets); // fBasketEntry
  c.skip(1);
  b.seekArrayOff = c.at;
  b.basketSeek.resize(b.writeBasket);
  if (!c.need(8ull * b.maxBaskets))
    return false;
  for (int32_t i = 0; i < b.writeBasket; ++i)
    b.basketSeek[i] = beGet<int64_t>(c.p + c.at + 8 * i);
  c.at = end; // fFileName + any tail via the outer byte count
  fm.branches.push_back(std::move(b));
  return true;
}

} // namespace

ContainerMeta parseContainer(int fd) {
  ContainerMeta fm;
  auto fail = [&](std::string why) {
    fm.error = std::move(why);
    fm.keys.clear();
    return fm;
  };

  off_t fsz = ::lseek(fd, 0, SEEK_END);
  fm.fileSize = fsz;
  std::vector<uint8_t> head(512);
  if (::pread(fd, head.data(), head.size(), 0) < 4 ||
      std::memcmp(head.data(), "root", 4) != 0)
    return fail("not a ROOT file");
  fm.large = beGet<int32_t>(head.data() + 4) > 1000000;
  fm.headerSeekWidth = fm.large ? 8 : 4;
  fm.fend = fm.large ? beGet<int64_t>(head.data() + 12) : beGet<int32_t>(head.data() + 12);

  // Root directory record at fBEGIN=100: key, TNamed payload, TDirectory
  // (fVersion, 2x datime, fNbytesKeys, fNbytesName, 3 seeks by width).
  auto dirKey = parseKey(head.data(), head.size(), 100);
  if (!dirKey)
    return fail("no root directory key");
  {
    // The whole walk is bounds-checked against the header buffer: keylen and
    // the TString lengths are hostile bytes (a bad keylen once walked the
    // cursor 64 KiB past `head` — found by transpose-fuzz).
    size_t q = 100 + dirKey->keylen;
    bool oob = false;
    // TNamed inline (no byte count here): two TStrings.
    auto pstr = [&](size_t& at) {
      if (at >= head.size()) {
        oob = true;
        return;
      }
      uint32_t len = head[at++];
      if (len == 255) {
        if (at + 4 > head.size()) {
          oob = true;
          return;
        }
        len = beGet<uint32_t>(head.data() + at);
        at += 4;
      }
      if (len > head.size() - at) {
        oob = true;
        return;
      }
      at += len;
    };
    pstr(q);
    pstr(q);
    if (oob || q + 2 > head.size())
      return fail("directory record out of bounds");
    uint16_t dver = beGet<uint16_t>(head.data() + q);
    q += 2 + 4 + 4 + 4 + 4; // version, datimeC, datimeM, fNbytesKeys, fNbytesName
    bool wide = dver > 1000;
    q += (wide ? 8 : 4) * 2; // fSeekDir, fSeekParent
    if (q + (wide ? 8u : 4u) > head.size())
      return fail("directory record out of bounds");
    fm.keyslistSeek = wide ? beGet<int64_t>(head.data() + q) : beGet<int32_t>(head.data() + q);
    fm.dirSeekKeysOff = q;
    // A narrow directory record under a 64-bit header is a valid layout, not a
    // corrupt file: each width is used only where it applies (see FileMeta).
    fm.dirSeekWidth = wide ? 8 : 4;
  }

  // Keys list.
  {
    auto kl = [&]() -> std::optional<KeyInfo> {
      std::vector<uint8_t> buf(4096);
      ssize_t r = ::pread(fd, buf.data(), buf.size(), fm.keyslistSeek);
      if (r < 34)
        return std::nullopt;
      return parseKey(buf.data(), static_cast<size_t>(r), 0);
    }();
    if (!kl)
      return fail("cannot read keys list");
    // Geometry must hold before it sizes an allocation or indexes the record
    // (hostile nbytes/keylen: negative/oversized alloc, OOB nkeys read).
    if (kl->nbytes <= 0 || kl->keylen + 4u > static_cast<uint32_t>(kl->nbytes) ||
        fm.keyslistSeek < 0 || kl->nbytes > fsz - fm.keyslistSeek)
      return fail("keys-list key geometry implausible");
    std::vector<uint8_t> buf(kl->nbytes);
    if (::pread(fd, buf.data(), buf.size(), fm.keyslistSeek) !=
        static_cast<ssize_t>(buf.size()))
      return fail("short keys list read");
    size_t at = kl->keylen;
    int32_t nkeys = beGet<int32_t>(buf.data() + at);
    at += 4;
    for (int32_t i = 0; i < nkeys; ++i) {
      auto e = parseKey(buf.data(), buf.size(), at);
      if (!e)
        break;
      fm.keys.push_back(*e);
      if (e->keylen < 29) // smaller than a minimal key record: cannot advance
        break;
      at += e->keylen;
    }
  }
  return fm;
}

std::vector<uint8_t> readKeyPayload(int fd, const KeyInfo& k) {
  if (k.nbytes <= 0 || k.keylen > k.nbytes || k.objlen < 0 || k.seekkey < 0)
    return {};
  std::vector<uint8_t> rec(k.nbytes);
  if (::pread(fd, rec.data(), rec.size(), k.seekkey) != static_cast<ssize_t>(rec.size()))
    return {};
  std::vector<uint8_t> out;
  if (k.objlen == k.nbytes - k.keylen)
    out.assign(rec.begin() + k.keylen, rec.end());
  else
    out = decompressFrames(rec.data() + k.keylen, rec.size() - k.keylen, k.objlen);
  if (out.size() != static_cast<size_t>(k.objlen))
    return {};
  return out;
}

FileMeta parseFile(const std::string& path, const std::string& tree) {
  FileMeta fm;
  auto fail = [&](std::string why) {
    fm.error = std::move(why);
    fm.branches.clear();
    return fm;
  };

  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return fail("cannot open " + path);
  ContainerMeta cm = parseContainer(fd);
  // Geometry is carried over even when the walk failed later on: the detected
  // seek widths stay observable on the error path, which is what makes a
  // width-detection bug diagnosable from a file that does not fully parse.
  const off_t fsz = cm.fileSize;
  fm.large = cm.large;
  fm.headerSeekWidth = cm.headerSeekWidth;
  fm.fend = cm.fend;
  fm.keyslistSeek = cm.keyslistSeek;
  fm.dirSeekKeysOff = cm.dirSeekKeysOff;
  fm.dirSeekWidth = cm.dirSeekWidth;
  if (!cm.error.empty()) {
    ::close(fd);
    return fail(cm.error);
  }

  // The live (highest-cycle) key named `tree`.
  for (const auto& e : cm.keys)
    if (e.cls == "TTree" && e.name == tree && e.cycle >= fm.treeKey.cycle)
      fm.treeKey = e;
  if (fm.treeKey.nbytes == 0) {
    ::close(fd);
    return fail("tree '" + tree + "' not found in keys list");
  }
  if (fm.treeKey.nbytes < 0 || fm.treeKey.keylen > fm.treeKey.nbytes ||
      fm.treeKey.objlen < 0 || fm.treeKey.seekkey < 0 ||
      fm.treeKey.nbytes > fsz - fm.treeKey.seekkey) {
    ::close(fd);
    return fail("tree key geometry implausible");
  }
  fm.treeBlob = readKeyPayload(fd, fm.treeKey);
  ::close(fd);
  if (fm.treeBlob.size() != static_cast<size_t>(fm.treeKey.objlen))
    return fail("tree metadata decompression failed");

  // Streamed TTree v20 walk.
  Cur c;
  c.p = fm.treeBlob.data();
  c.n = fm.treeBlob.size();
  c.keylen = fm.treeKey.keylen;
  std::vector<std::pair<uint32_t, std::string>> classRefs;
  std::vector<std::pair<uint32_t, std::string>> leafRegistry;
  uint16_t tver;
  c.objHeader(tver, "TTree");
  if (c.fail_ || tver != 20)
    return fail(c.fail_ ? c.why : "TTree v" + std::to_string(tver) + " unsupported");
  c.skipObj("TNamed");
  c.skipObj("TAttLine");
  c.skipObj("TAttFill");
  c.skipObj("TAttMarker");
  c.skip(5 * 8 + 8 + 4 * 4); // fEntries..fFlushedBytes (q x5), fWeight (d), 4x i4
  uint32_t nClusterRange = c.get<uint32_t>();
  c.skip(6 * 8);             // fMaxEntries..fEstimate (q x6)
  if (nClusterRange > (1u << 20))
    return fail("implausible fNClusterRange");
  c.skip(1 + 8ull * nClusterRange); // fClusterRangeEnd
  c.skip(1 + 8ull * nClusterRange); // fClusterSize
  c.skipObj("TIOFeatures");
  if (!objArray(c, classRefs, "fBranches", [&](const AnyObj& o) {
        return o.cls == "TBranch" ? parseBranch(c, fm, classRefs, leafRegistry)
                                  : (c.fail_ = true,
                                     c.why = "branch class " + o.cls + " unsupported", false);
      }))
    return fail(c.why.empty() ? "branch walk failed" : c.why);
  if (c.fail_)
    return fail(c.why);
  return fm;
}

} // namespace ucache::transpose
