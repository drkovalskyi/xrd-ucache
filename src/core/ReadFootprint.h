// Which parts of a file a run actually read, in ORIGINAL-file coordinates.
//
// This is the identity of the WORK, as opposed to the identity of the inputs.
// Two runs over the same files can still be different analyses -- different
// columns, different selection -- and comparing their walls would be
// meaningless. The input signature cannot tell them apart; this can.
//
// Original coordinates are the point. The byte tier, a relay and a
// cache-disabled run all address the origin's own layout, so their requests
// need no translation. A replica does not: it is a rewritten container whose
// baskets live at different offsets and different lengths, so a read of it is
// mapped back through the sidecar's origMap before being recorded here. That
// is what makes a replica run comparable with the baseline that measured it.
//
// Granularity is a fixed bucket rather than the exact byte range, because the
// two routes coalesce differently -- a replica serves ~465 KiB per request
// where the byte tier serves ~42 KiB for the same physics. Whole buckets are
// stable under that; byte ranges are not.
//
// Thread-safety: note() is safe from any thread. The bitmap grows under a
// lock; the common case touches an already-sized vector.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ucache {

class ReadFootprint {
 public:
  // 64 KiB: coarse enough that the two routes' coalescing does not change the
  // answer, fine enough to separate one column from another.
  static constexpr uint64_t kBucket = 64 * 1024;

  void note(uint64_t off, uint64_t len);
  // 12 hex characters over the set of touched buckets, or empty when nothing
  // was read. Order-independent by construction: it hashes bucket INDICES in
  // ascending order, so the sequence reads arrived in cannot change it.
  std::string sig() const;
  bool empty() const;

 private:
  mutable std::mutex mu_;
  std::vector<uint64_t> bits_;
};

} // namespace ucache
