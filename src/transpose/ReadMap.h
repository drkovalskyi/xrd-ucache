// Which share of a file's data a reader asks for.
//
// A reader that fetches most of every file gains nothing from a cache it would
// fill with the whole dataset; the plugin reads such files straight from the
// origin (`max_read_fraction`). The question is answered from the file's own
// structure, from the branches (RNTuple: columns) a reader's requests touch:
// the share is the bytes those branches hold in the file over the bytes every
// branch holds -- the part of each entry's data the reader asks for.
//
// It is weighed by branch, not by the entries a request happens to cover: a
// branch with little data per entry may keep a whole file's entries in one
// basket, and one such basket in a request would otherwise make the request
// look like a sliver of a file-wide area.
//
// No single request is trusted to show the whole reader. ROOT's read cache
// learns a reader's branches one basket at a time; a cache too small for a
// cluster fills with the baskets that fit (the small ones); a fill of more
// than 1024 baskets goes out as several vector reads, in any order. So the
// branches accumulate over the reader's first requests (step()), and the
// decision is taken once: read directly as soon as the branches read so far
// hold more than the limit; cached as soon as a request of two or more
// branches adds none -- the reader has come back to branches it read before.
//
// A branch counts as read when a request covers at least half of one of its
// units (baskets, pages): readers fetch whole units, and ROOT's first read of
// a file (its header, 300 bytes) runs into the first basket when one sits
// right after it. A request that covers no unit reads the file's own records
// (header, keys list, tree or anchor metadata) and names no branch.
//
// Thread-safety: a ReadMap is immutable after construction; every method may
// be called concurrently.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ucache::transpose {

struct FileMeta;
struct RNTupleMeta;
struct Source;

class ReadMap {
 public:
  // The share a set of branches (columns) holds.
  struct Share {
    uint64_t asked = 0;  // bytes the branches hold in the file
    uint64_t total = 0;  // bytes every branch holds
    uint32_t groups = 0; // how many branches
  };

  // A branch whose baskets live in another file is left out; so are baskets
  // with no seek or no length.
  static ReadMap fromTree(const FileMeta& fm);
  // Pages are counted once per offset, checksum included; the first 128 KiB
  // never counts (ROOT reads the file head as one block at open, and on a
  // small file that block runs into the first pages).
  static ReadMap fromRNTuple(const RNTupleMeta& rm);

  bool empty() const { return off_.empty(); }
  bool isRNTuple() const { return rnt_; }

  // The branches (columns) with a unit at least half covered by the
  // [offset, offset + length) ranges of the ORIGINAL file: distinct,
  // ascending. Empty when they cover none.
  std::vector<uint32_t> groupsOf(const std::vector<std::pair<uint64_t, uint64_t>>& ranges) const;
  // The share `groups` (distinct, ascending) hold.
  Share weigh(const std::vector<uint32_t>& groups) const;
  // Does a share exceed the limit (asked * 100 > limitPercent * total)?
  static bool exceeds(const Share& s, int limitPercent);

  // One request that needs the origin, in the order they come; `seen` carries
  // the branches earlier ones read (distinct, ascending) and `out` the share
  // they hold now. 1 = read directly, 0 = cached, -1 = not yet (a request of
  // the file's own records, a single basket, or one that found new branches).
  int step(std::vector<uint32_t>& seen, const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
           int limitPercent, Share& out) const;

  // Does [off, off + len) overlap any unit?
  bool touches(uint64_t off, uint64_t len) const;

  // The units' extents, merged where they touch: [begin, end), ascending.
  // A compact stand-in for touches() once the full map is no longer needed.
  std::vector<std::pair<uint64_t, uint64_t>> dataRuns() const;

  uint64_t memoryBytes() const;

 private:
  bool rnt_ = false;
  uint64_t headSkip_ = 0;
  // Units sorted by offset.
  std::vector<uint64_t> off_;
  std::vector<uint32_t> len_;
  std::vector<uint32_t> group_;
  std::vector<uint64_t> groupBytes_; // per branch (column): its units' bytes
  uint64_t total_ = 0;
};

// The tree a reader of this file would be reading: NanoAOD's by name, else the
// first TTree the keys list names.
FileMeta parseReaderTree(Source& src, int64_t size);

} // namespace ucache::transpose
