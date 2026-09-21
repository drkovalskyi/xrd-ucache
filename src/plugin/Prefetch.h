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
// Two things follow from what an origin actually charges for. A request costs
// far more for its ELEMENT COUNT than for its bytes, and a request on one open
// file is answered strictly after the one before it, so the reader's stream is
// paced by how long a fill-sized read takes -- which is about how long the
// decompression it hides takes. Hence `prefetch_depth`, which keeps more than
// one fill on the wire so a slow one has somewhere to be absorbed, and
// `prefetch_bridge_kb`, which joins predicted ranges across small gaps to buy
// element count with bandwidth (the bridged bytes are never staged). And hence
// PRIMING: the first fill of a file is the one nothing can predict from a
// previous fill, so the map is read at OPEN instead, from the origin, using
// the branch set an earlier file taught. That also serves the reader's own
// metadata reads out of RAM.
// A speculative page never reaches the cache: FileEntry writes a page only
// once the READER has demanded it (FileEntry::stageSpeculative). The cache
// reading its own stage -- the basket-map parse, through readCached with
// accounting off -- does not count as demand and does not promote a page.
//
// Thread-safety: onFill/onClose copy what they need and post to the
// prefetcher's own thread, which owns every table and every handle's state;
// wire completions run on XrdCl threads and touch only the entry (itself
// thread-safe) and atomics. Nothing here runs on the hit-serving executor.
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
  // A handle finished setting its entry up. Read the file's basket map now,
  // ahead of the reader's own metadata reads, and predict its first fill.
  // Does nothing until read-ahead has confirmed on an earlier file.
  void onOpen(const std::shared_ptr<HandleState>& st, const std::shared_ptr<FileEntry>& entry);

  // True once something has created the prefetcher (a handle missed and read
  // ahead). Callers on the open path test this so a process that never reads
  // ahead never starts its threads.
  static bool active();

  // Process state, for tests and diagnostics.
  bool confirmed() const;
  bool disabled() const;

 private:
  Prefetcher();
  struct Impl;
  Impl* impl_;
};

} // namespace ucache
