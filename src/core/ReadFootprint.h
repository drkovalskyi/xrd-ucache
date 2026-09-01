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
  // three different answers; from 256 KiB up they agreed on the sets measured
  // then, and 1 MiB is taken for margin. Discrimination survives it: two
  // analyses reading different columns of the same files share 17% of their
  // buckets at this size, so a different analysis still reads as different
  // work.
  //
  // IT IS MARGIN, NOT A GUARANTEE, and the difference matters to anything
  // that compares two runs. A replica is a rewritten container: the reader
  // asks it for different spans than it asks the original, so the two routes
  // do not merely coalesce differently, they request differently. On a file
  // set where only part of each file was relocated, the reader's spans over
  // the ORIGINAL layout crossed large unrelocated gaps that the replica never
  // asks for, and a whole bucket fell inside such a gap on 6% of the files --
  // the byte route marked it, the replica route did not. Both are honest
  // about what was read; they are answers to slightly different questions.
  // Exact equality of two signatures is therefore evidence of the same work,
  // while inequality is not proof of different work.
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

  // Both, under ONE lock. Taking sig() and count() separately lets a poison()
  // from another handle on the same URL land between them, so the record gets a
  // confident signature next to a zero bucket count -- a self-contradicting
  // pair, which is worse than emitting neither. Holding one reference to the
  // footprint does not close that: the two calls still lock independently.
  // Returns false when the footprint has nothing to say (poisoned, or empty),
  // in which case neither out-parameter is written.
  bool sigAndCount(uint64_t limitBytes, std::string& sigOut, uint64_t& countOut) const;

  // Declare this file's footprint permanently UNKNOWN. Sticky, and it wins
  // over anything recorded before or after.
  //
  // Skipping a read that cannot be expressed in the original file's
  // coordinates is not enough, because a footprint outlives the handle that
  // filled it: one process can read a file through a replica with no map
  // (contributing nothing) and then through the byte tier (contributing its
  // ranges), and what is emitted is a CONFIDENT signature covering part of the
  // work. Partial and confident is the one answer worse than none -- an empty
  // signature is treated as no evidence, while a partial one is compared and
  // silently disagrees. Once any read on a file could not be translated, the
  // file has nothing trustworthy to say for the rest of the process.
  void poison();
  bool poisoned() const;
  // Touched bucket indices, ascending. For diagnosis only: comparing two
  // signatures tells you THAT they differ, this tells you where.
  std::vector<uint64_t> buckets(uint64_t limitBytes = 0) const;

 private:
  // Both assume mu_ is held. The public accessors take the lock and check the
  // poison flag; sigAndCount() does so once for the pair.
  std::string sigLocked(uint64_t limitBytes) const;
  uint64_t countLocked(uint64_t limitBytes) const;

  mutable std::mutex mu_;
  std::vector<uint64_t> bits_;
  bool poisoned_ = false;
};

} // namespace ucache
