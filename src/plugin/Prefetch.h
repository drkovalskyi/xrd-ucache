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
//     share that branch drew in one fill; a process-wide cap on staged
//     speculative bytes shrinks the window under pressure.
//   * SELF-DISABLING. A speculative page the reader's frontier has passed
//     without demanding is dropped and counted never-used; when never-used
//     exceeds a quarter of what was issued (after 64 MB) the process stops.
// A speculative page never reaches the cache: FileEntry writes a page only
// once the reader has demanded it (FileEntry::stageSpeculative).
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

  // Process state, for tests and diagnostics.
  bool confirmed() const;
  bool disabled() const;

 private:
  Prefetcher();
  struct Impl;
  Impl* impl_;
};

} // namespace ucache
