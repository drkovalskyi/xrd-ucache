// XrdCl FilePlugIn gluing ucache-core to the client.
//
// Design invariants:
//  - The inner XrdCl::File is constructed with plugins DISABLED — no
//    recursion (asserted in tests).
//  - Classification (bitmap checks) happens on the caller's thread; disk
//    reads/writes and buffer assembly happen on the Executor; wire misses
//    are issued as ONE VectorRead of page-rounded, order-preserving,
//    coalesced ranges (§5.2 step 3); the user's completion is never delayed
//    by disk persistence (§5.2 step 4).
//  - Exactly one completion per request, statuses propagated faithfully;
//    any cache-side failure degrades to pure pass-through (§5.2 step 5);
//    after UCACHE_MAX_ERRORS consecutive cache errors the handle trips to
//    permanent pass-through (disabled_handles).
//  - No exceptions cross this ABI (§5.3).
//
// Lifetime: executor tasks and wire handlers capture only the shared
// HandleState (never `this`). Tasks that must touch the inner file go
// through HandleState::acquireInner/releaseInner; the plugin destructor
// invalidates and drains. PgRead is pass-through and never cached (§4.7:
// ROOT's path is Read/VectorRead; xrdcp's pgread traffic stays uncached) —
// EXCEPT on a transposed handle, where pass-through would
// return stale/missing bytes: there it is served from the stitched view
// with locally computed crc32c page checksums.
//
// Thread-safety: fully thread-safe; see HandleState.
#pragma once

#include "CacheStore.h"
#include "Config.h"
#include "ReplicaStore.h"
#include "XrdClTimeout.h"

#include <XrdCl/XrdClFile.hh>
#include <XrdCl/XrdClPlugInInterface.hh>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace ucache {

// State shared between the plugin object, executor tasks, and wire handlers.
struct HandleState {
  // CPU-span evidence for the recompression estimator: rusage
  // user+sys µs snapshotted when the handle opens; the delta at Close is the
  // CPU attributable to this file for sequential access patterns. `blended`
  // is set when other ucache handles were open concurrently (attribution
  // uncertain — the estimator treats it as an upper bound).
  uint64_t cpu0Us = 0;
  bool cpuBlended = false;
  std::shared_ptr<CacheStore> store; // process-wide; null => never cache
  std::string url;
  // Pass-through bytes this handle relayed, and the once-latch for its
  // per-file record. A handle with no entry (UCACHE_DISABLE, write-opened)
  // leaves no lifetime record from FileEntry, so the first of Close/dtor
  // writes one from these — that is what makes a cache-disabled BASELINE run
  // matchable, file by file, against the cached runs that follow it.
  std::atomic<uint64_t> relayedBytes{0};
  std::atomic<bool> relayObsDone{false};
  // Wall span of a handle the cache never served, in µs of a steady clock —
  // the same quantity FileEntry tracks for cached files, so a relayed file and
  // a cached file are directly comparable.
  std::atomic<uint64_t> relayFirstUs{0};
  std::atomic<uint64_t> relayLastUs{0};

  std::mutex mu;
  std::shared_ptr<FileEntry> entry;            // null until setup completes
  // Transposed-replica view: adopted once at entry setup after
  // its open-time full verify, HANDLE-STABLE thereafter —
  // overlay ranges do not exist at the origin, so a handle must keep the
  // view it opened with. A mid-handle overlay fault errors that read
  // faithfully (the "behaves like a local file" contract) and drops the
  // replica for FUTURE opens; this handle keeps serving what still verifies.
  std::shared_ptr<ReplicaView> view;
  std::atomic<bool> replicaDropped{false};     // one-time drop-on-fault latch
  std::unique_ptr<XrdCl::StatInfo> statInfo;   // clone source for Stat(false)
  std::mutex setupMu;                          // serializes lazy entry setup
  bool setupDone = false;                      // entry setup attempted (ok or not)
  bool closed = false;
  int errors = 0; // consecutive cache-side errors (UCACHE_MAX_ERRORS trip)
  bool tripped = false;

  // The inner XrdCl::File lives HERE (owned by HandleState, not the plugin
  // object) so a posted retry task can swap it while capturing only `st_`
  // `inner` is the guarded raw pointer
  // into `innerOwned`, repointed by resetInner().
  std::unique_ptr<XrdCl::File> innerOwned;
  XrdCl::File* inner = nullptr;
  int innerOps = 0;
  bool innerValid = false;
  std::condition_variable innerCv;

  // Cache-freshness (UCACHE_REVALIDATE_S): when a recent local entry is
  // trusted, Open skips the remote open+stat and sets cacheOnly — the inner
  // file is then opened LAZILY only if a genuine miss needs the origin
  // (fail-open). Opt-in; when the knob is 0 these stay false and the path is
  // unchanged. innerOpenMu serializes the one-shot lazy open off the fast path.
  bool cacheOnly = false;
  bool innerOpened = false;
  bool innerOpenOk = false;
  std::mutex innerOpenMu;
  // Failed lazy opens do NOT latch (a user's cmsRun died to one transient
  // "no route to host" reselection): the failure and its time are kept so
  // the next miss after a cool-down re-attempts, and reads surface the
  // ORIGIN's error, never a generic errInvalidOp. Guarded by innerOpenMu.
  XrdCl::XRootDStatus lazyOpenError;
  uint64_t lazyOpenFailUs = 0;
  int lazyOpenAttempt = 0; // cumulative across cool-down re-attempts (fault hook numbering)
  bool ensureInnerOpen(); // lazy synchronous origin open on first miss
  // The status a cache miss should fail with when the inner file is
  // unavailable: the recorded lazy-open failure if there is one, else
  // errInvalidOp (genuinely invalid handle states).
  XrdCl::XRootDStatus missError();

  XrdCl::File* acquireInner();
  void releaseInner();
  void shutdownInner(); // plugin dtor: invalidate + drain + destroy the file
  // Destroy the terminally-failed inner file and install a fresh one; returns
  // the new raw pointer. Retry only; precondition innerOps==0.
  XrdCl::File* resetInner();

  // Async page-persist accounting. Persists are posted off the read path
  // (§5.2 step 4), but Close/destruction must wait for them so short-lived
  // processes still leave a fully populated cache. begin/end bracket each
  // posted task; drainPersists blocks (Close/dtor only, never a read).
  std::mutex persistMu;
  std::condition_variable persistCv;
  int pendingPersists = 0;
  void beginPersist();
  void endPersist();
  void drainPersists();

  void noteCacheError(const Config& cfg);
  void noteCacheOk();
};

class UCacheFile : public XrdCl::FilePlugIn {
 public:
  UCacheFile();
  ~UCacheFile() override;

  XrdCl::XRootDStatus Open(const std::string& url, XrdCl::OpenFlags::Flags flags,
                           XrdCl::Access::Mode mode, XrdCl::ResponseHandler* handler,
                           ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Close(XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Stat(bool force, XrdCl::ResponseHandler* handler,
                           ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Read(uint64_t offset, uint32_t size, void* buffer,
                           XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus PgRead(uint64_t offset, uint32_t size, void* buffer,
                             XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus VectorRead(const XrdCl::ChunkList& chunks, void* buffer,
                                 XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Write(uint64_t offset, uint32_t size, const void* buffer,
                            XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Write(uint64_t offset, XrdCl::Buffer&& buffer,
                            XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus VectorWrite(const XrdCl::ChunkList& chunks,
                                  XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus WriteV(uint64_t offset, const struct iovec* iov, int iovcnt,
                             XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Sync(XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Truncate(uint64_t size, XrdCl::ResponseHandler* handler,
                               ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Fcntl(const XrdCl::Buffer& arg, XrdCl::ResponseHandler* handler,
                            ucache::XrdTimeout timeout) override;
  XrdCl::XRootDStatus Visa(XrdCl::ResponseHandler* handler, ucache::XrdTimeout timeout) override;
  bool IsOpen() const override;
  bool SetProperty(const std::string& name, const std::string& value) override;
  bool GetProperty(const std::string& name, std::string& value) const override;

 private:
  void invalidateOnWrite();
  // Lazily create the cache entry on the caller's thread (first read), doing
  // a synchronous inner Stat. Returns the entry, or nullptr for
  // pass-through (write-open, disabled, denied host, Stat failure, closed).
  // Also adopts the transposed-replica view when one validates.
  std::shared_ptr<FileEntry> ensureEntry();
  std::shared_ptr<ReplicaView> currentView() const;

  std::shared_ptr<HandleState> st_;
  bool passthroughOnly_ = false; // write-open / UCACHE_DISABLE / denied host
  bool cacheOpen_ = false;       // trusted-cache Open succeeded without the remote
};

// Process-wide config/store accessors (UCacheFactory.cc).
const Config& globalConfig();
std::shared_ptr<CacheStore> globalStore();

} // namespace ucache
