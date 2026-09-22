// Read-ahead for TTree readers: predict the reader's next fill from the file's
// own basket map and fetch it while the reader computes.
//
// A ROOT reader fetches a batch of baskets -- the branches it uses, for the
// next stretch of entries -- then decompresses and processes them, then asks
// for the next stretch. The tree's metadata record, which every reader fetches
// before its first event, says where every basket of every branch is and which
// entry it starts at. The next fill can therefore be read off the current one:
// identify the branches from the baskets it asked for, note how far each got,
// and take the next baskets forward in entry order, as much as the reader has
// been drawing per fill. Nothing is guessed except that the reader continues
// where it left off.
//
// Safe by construction, and on by default:
//   * SHADOW before fetch. A handle predicts, fetches nothing, and compares the
//     prediction with the next fill; the process fetches only once a
//     prediction has covered >= 90% of a fill. A reader whose handles issue one
//     fill each never confirms and never costs a byte -- nor a parse, since the
//     map is read only from a handle's second fill while the process is
//     unconfirmed.
//   * BOUNDED by demand. One window ahead per branch, sized by the largest
//     share that branch drew in one fill; a process-wide cap counting both
//     staged bytes and bytes on the wire shrinks the window under pressure.
//   * SELF-DISABLING. A speculative page the reader's frontier has passed
//     without demanding is dropped and counted never-used; when never-used
//     exceeds a quarter of what was issued (after 64 MB) the process stops.
//     Never-used is read from the entries themselves, so every route counts:
//     the frontier, the close, the sweep, a punch, the entry's own death.
//   * NEVER ITS OWN CONNECTION. Read-ahead uses the origin only if the handle
//     already has it open; it will not run the lazy open of a trusted handle,
//     because the application did not ask for this read and must not wait for
//     it, inherit its failure, or spend its one open attempt on it.
//
// WHAT AN ORIGIN CHARGES FOR, measured, because three plausible ideas died on
// it: a request on one open file is answered strictly after the one before it,
// so queueing a second does not make the first arrive sooner; and on a link
// several readers share, a byte costs far more than a request element saves.
// Together: READ-AHEAD MAY ONLY FETCH THE SAME BYTES EARLIER. `prefetch_depth`
// (fills on the wire) and `prefetch_bridge_kb` (join ranges across gaps to buy
// elements with bandwidth) both exist to be turned UP on an origin that
// behaves differently; on the ones measured they cost more than they return,
// and their defaults say so.
// A speculative page never reaches the cache: FileEntry writes a page only
// once the READER has demanded it (FileEntry::stageSpeculative). The cache
// reading its own stage -- the basket-map parse, through readCached with
// accounting off -- does not count as demand and does not promote a page.
//
// Thread-safety: onFill/onClose copy what they need and post to one of
// `prefetch_threads` prediction threads, chosen by handle, so a handle's
// state belongs to exactly one of them for its life and needs no lock. What
// they share -- the switches, the byte counters and the cache of parsed
// basket maps -- is either atomic or under one mutex. Wire completions run on
// XrdCl threads and touch only the entry (itself thread-safe) and atomics.
// Nothing here runs on the hit-serving executor.
#pragma once

#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdint>
#include <memory>

namespace ucache {

struct HandleState;
class FileEntry;
struct PrefetchHandle; // per-handle prediction state; lives on HandleState

class Prefetcher {
 public:
  // Process-wide instance, intentionally leaked (its thread ends with the
  // process, like the executor's).
  static Prefetcher& instance();

  // A vector read arrived on a plain (non-replica) entry. `anyMiss` = the
  // request had at least one chunk not present: a handle that never misses is
  // a warm handle and is never looked at, so warm passes pay nothing.
  void onFill(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry,
              const XrdCl::ChunkList& chunks, bool anyMiss);
  // The handle closed: whatever it had in flight or staged speculatively is
  // dropped and counted; completions that land afterwards stage nothing.
  void onClose(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry);

  // Process state, for tests and diagnostics.
  bool confirmed() const;
  bool disabled() const;

 private:
  Prefetcher();
  struct Impl;
  Impl* impl_;
};

} // namespace ucache
