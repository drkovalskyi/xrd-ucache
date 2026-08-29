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
  // 1 MiB, chosen by measurement rather than taste. The routes coalesce
  // differently: a byte-tier request merges adjacent pages and transfers the
  // gap between them, while a replica read maps back to the pages alone and
  // never names the gap. At 64 KiB those gaps landed in buckets one route
  // marked and the other did not, and four routes over one analysis produced
  // three different answers; from 256 KiB up they agree, and 1 MiB is taken
  // for margin against a workload that coalesces harder than the ones
  // measured. Discrimination survives it: two analyses reading different
  // columns of the same files share 17% of their buckets at this size, so a
  // different analysis still reads as different work.
  static constexpr uint64_t kBucket = 1024 * 1024;

  void note(uint64_t off, uint64_t len);

  // `limitBytes` is the file's size at the ORIGIN; buckets beyond it are
  // dropped. Readers routinely ask past the end -- ROOT fetches a file's tail
  // in fixed blocks -- and a route that serves the request verbatim would
  // record those bytes while a route that maps them through a replica's layout
  // would not. Past-EOF bytes are not file content, so neither should sign
  // them. 0 means "no limit known", which only a caller with no size may pass.
  std::string sig(uint64_t limitBytes = 0) const;
  bool empty() const;
  uint64_t count(uint64_t limitBytes = 0) const;
  // Touched bucket indices, ascending. For diagnosis only: comparing two
  // signatures tells you THAT they differ, this tells you where.
  std::vector<uint64_t> buckets(uint64_t limitBytes = 0) const;

 private:
  mutable std::mutex mu_;
  std::vector<uint64_t> bits_;
};

} // namespace ucache
