// One cached object: sparse .data file + .meta sidecar.
//
// The contract the plugin builds on:
//   hasRange()   — atomic-chunk classification (all pages present?)
//   readCached() — HIT path; per-page CRC verified; false => fall back to
//                  origin (bad pages marked absent, crc_failures counted).
//                  On false the caller's buffer holds UNDEFINED bytes: a
//                  contiguous run of pages is read with ONE pread and
//                  verified afterwards, so unverified bytes may already have
//                  landed there. The caller must treat the whole request as
//                  unserved and overwrite the entire span.
//   writePages() — MISS path; persists every *full* page inside the span
//                  (tail page special-cased), data-before-bit ordering,
//                  UCACHE_FSYNC honored; backend errors fail open (page
//                  skipped, failopen_events counted) and never throw
//
// Fill buffer: with fill_buffer_mb > 0 (default),
// writePages STAGES pages in RAM instead of writing them; staged pages are
// part of the presence contract (hasRange/readCached serve them), so a
// cold fill performs NO random IO against the cache disk. The stage drains
// as offset-sorted coalesced writes (flushBuffer) on: per-entry cap,
// process-wide cap, meta_flush_seconds interval, entry close, and the
// eviction/cleanup flush. Durability ordering is unchanged — bytes (+
// fdatasync per UCACHE_FSYNC) land before bits are published; a crash
// loses at most the staged pages, never consistency. Staged-but-unflushed
// bytes are invisible to cachedBytes()/usage until flushed (eviction
// flushes first). fill_buffer_mb = 0 restores the legacy immediate path.
//
// Cross-process: several processes may hold the same entry open and fill
// different ranges of it (one job per chunk; several jobs on one node). Page
// data is idempotent, so the .data writes never conflict. The sidecar is
// shared state and is COMMITTED, never replaced: every store re-reads the
// image on disk under the entry lock, applies only the pages this handle set
// or cleared since its last store (plus a pin it changed), stores the result
// and adopts it, so a sibling's pages are served here as well. A fresh
// entry's empty sidecar is stored at open, under the same lock, so a second
// opener joins the fill instead of truncating it.
//
// Thread-safety: fully thread-safe. A single mutex guards bitmap/CRC/meta
// state AND the two stage maps; data-file pread/pwrite run outside the
// lock (page writes are idempotent-same-bytes; presence bits only ever
// transition absent→present under the lock, and present→absent only on
// CRC failure, a punch, or a commit adopting a sibling's clear). At most one buffer flush runs at a time
// (flushInProgress_ + condvar); staged pages stay readable during their
// flush via the flushing_ map.
#pragma once

#include "Config.h"
#include "IOBackend.h"
#include "MetaFile.h"
#include "ReadFootprint.h"
#include "Stats.h"
#include "UrlKey.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ucache {

class FileEntry {
 public:
  // Opens or creates the entry and validates it against the origin
  // size always; mtime/cksum per cfg.validate. A loadable but
  // stale sidecar is discarded (validations_failed); a corrupt one likewise
  // (meta_corrupt). Returns nullptr only when even a fresh entry cannot be
  // set up — the caller fails open to pure pass-through.
  // onPersist (optional) is invoked after each successful page-write batch with
  // the number of bytes newly persisted — the store uses it to maintain a
  // running usage total and trigger a rate-limited eviction check on the write
  // path, so a single long-open cold read cannot overshoot the budget
  // between opens. The target must outlive this entry (the store owns entries
  // and already lends `cfg` by const ref, so this adds no new lifetime rule).
  // onPersist(bytes, allowEvict): allowEvict=false for the destructor's final
  // flush — an entry mid-close has no registry protection, and an eviction
  // check there could evict the entry being closed (test-caught).
  static std::shared_ptr<FileEntry> open(IOBackend& io, const Config& cfg, Stats& stats,
                                         const UrlKey& key, uint64_t originSize,
                                         uint64_t originMtime, uint8_t cksumKind,
                                         uint32_t originCksum,
                                         std::function<void(uint64_t, bool)> onPersist = {});
  ~FileEntry();

  FileEntry(const FileEntry&) = delete;
  FileEntry& operator=(const FileEntry&) = delete;

  uint64_t fileSize() const { return meta_.fileSize; }
  uint32_t pageSize() const { return meta_.pageSize; }
  const UrlKey& key() const { return key_; }

  bool hasRange(uint64_t off, uint64_t len);
  // account=false reads for the cache's own purposes (the prefetcher parsing
  // the metadata the reader fetched): no hit/served/first-touch accounting,
  // no activity note, so the run's records describe the reader alone.
  bool readCached(uint64_t off, uint64_t len, void* buf, bool account = true);
  void writePages(uint64_t off, uint64_t len, const void* buf);

  // Persist the sidecar if dirty and (force or flush interval elapsed).
  void flushMeta(bool force);
  // Drain the fill buffer to disk. force = unconditional (close,
  // eviction, tests); otherwise honors the cap/interval policy. Waits for a
  // concurrent flush when force so "returned" means "durable".
  void flushBuffer(bool force);
  // flushBuffer(true) + flushMeta(true): everything durable. Used by close,
  // eviction, cleanup — anything that needs the on-disk state authoritative.
  void flushAll();
  // The interval policy, driven by time instead of by writes: drain the fill
  // buffer if meta_flush_seconds have passed since the last drain, and commit
  // the sidecar on its own interval. The policy otherwise fires only from
  // inside writePages and at close, so a process that stopped writing and then
  // died without running destructors (a multiprocessing worker _exit()s) took
  // every page it had staged with it. Called by CacheStore::checkpoint.
  void checkpoint();

  // ---- speculative (prefetched) pages -----------------------------------
  // A speculative page is staged in RAM exactly like a fill page and serves
  // reads the same way, but while it carries its mark it is NEVER written:
  // flushBuffer, checkpoint and flushAll leave it in the stage. The mark
  // clears when the page is served (readCached) or when a real fill covers it
  // (writePages); a page still marked when dropped -- by dropSpeculative when
  // the reader's frontier has passed it, or at close -- leaves no trace in the
  // sidecar, the bitmap or the data file. Only full pages inside [off,off+len)
  // that are neither present nor staged are taken; returns the bytes staged.
  uint64_t stageSpeculative(uint64_t off, uint64_t len, const void* buf);
  // Drop the pages of [off, off+len) still marked speculative (never served,
  // never covered); returns the bytes dropped and counts them never-used.
  uint64_t dropSpeculative(uint64_t off, uint64_t len);
  uint64_t dropAllSpeculative();
  uint64_t speculativeBytes();
  // Page runs inside [off, off+len) that are neither present nor staged --
  // what a prefetch would have to fetch. Page-aligned, the tail page clamped
  // to the file size; empty when everything is already there.
  std::vector<std::pair<uint64_t, uint64_t>> absentRuns(uint64_t off, uint64_t len);
  // Process-wide bytes currently staged speculatively, across entries.
  static uint64_t speculativeTotal();

  bool pinned();
  void setPinned(bool p);
  uint64_t cachedBytes();

  // CRC scrub over all present pages (CLI `verify`, crash suite).
  struct ScrubResult {
    uint64_t checked = 0;
    uint64_t bad = 0;
  };
  ScrubResult verifyAll();

  // Fetch gate: dedups EXACT rounded miss ranges across handles of the
  // same entry. beginFetch returns true when the caller OWNS the fetch; false
  // parks the caller — onOwnerDone fires (from the owner's endFetch, on the
  // owner's thread) and the caller must then RE-CLASSIFY the read (usually a
  // RAM hit against the staged pages). endFetch must run on every owner exit
  // path, success or failure; parked callers keep their own safety timer.
  bool beginFetch(uint64_t off, uint64_t len, std::function<void()> onOwnerDone);
  void endFetch(uint64_t off, uint64_t len);

  // Replica punch-and-clear: for each byte range, mark every
  // FULLY-covered page absent (bit cleared, CRC zeroed; partial edge pages
  // stay), force-flush the sidecar, then hole-punch the cleared spans in
  // .data. Clear-then-punch ordering bounds a crash to lost cached pages —
  // bits are never left set over holes. Returns the formerly-RESIDENT cached
  // bytes whose disk backing was punched — not the span length, so sparse
  // holes inside the range don't inflate it (reclaim reports depend on this);
  // 0 when the filesystem lacks punch support (bits are cleared regardless:
  // reclaim is best-effort, correctness is not).
  uint64_t releaseRanges(const std::vector<std::pair<uint64_t, uint64_t>>& ranges);

  // Per-file record: this process's lifetime observation of the entry.
  // Accumulated on the serve/fill paths; written as ONE JSON record to
  // <stats stem>.files.jsonl when the last handle releases the entry.
  struct Obs {
    std::atomic<uint64_t> opens{0};           // CacheStore::open calls
    std::atomic<uint64_t> servedBytes{0};     // byte-tier bytes served
    std::atomic<uint64_t> ramBytes{0};        //   … of which from staged RAM
    std::atomic<uint64_t> replicaBytes{0};    // stitched bytes (plugin-attributed)
    std::atomic<uint64_t> diskReads{0};       // physical .data preads (hit path)
    std::atomic<uint64_t> diskSeq{0};         //   … starting at the previous end
    std::atomic<uint64_t> diskBytes{0};
    std::atomic<uint64_t> firstTouchBytes{0}; // bytes served for the first time
    std::atomic<uint64_t> wireBytes{0};       // bytes staged/persisted (fills)
    // Prefetch, per file: bytes the prefetcher asked the origin for, bytes of
    // speculative pages the reader then demanded, bytes dropped never used.
    std::atomic<uint64_t> prefetchIssued{0};
    std::atomic<uint64_t> prefetchServed{0};
    std::atomic<uint64_t> prefetchDropped{0};
    // Wall span this entry was live and doing work for, in µs of a steady
    // clock: first activity to last. In a slot-based analysis (one worker per
    // file at a time) this is that thread's FULL cost for the file — waits
    // AND the route's own CPU — which is what makes spans composable where
    // service times are not, and it is the quantity the gain methods divide.
    std::atomic<uint64_t> firstUs{0};
    std::atomic<uint64_t> lastUs{0};
  };
  Obs& obs() { return obs_; }
  // Zero when the entry never recorded activity, or when all of it landed
  // inside one clock tick — callers treat 0 as "no usable span".
  uint64_t spanUs() const {
    const uint64_t a = obs_.firstUs.load(std::memory_order_relaxed);
    const uint64_t b = obs_.lastUs.load(std::memory_order_relaxed);
    return b > a ? b - a : 0;
  }

  // Append sink for the close-time record. shared_ptr: entries released
  // during/after store teardown must not reach into a dead store.
  struct ObsSink {
    ObsSink(IOBackend& io, std::string path) : io_(io), path_(std::move(path)) {}
    ~ObsSink() {
      if (fd_ >= 0)
        io_.close(fd_);
    }
    void append(const std::string& line);

   private:
    IOBackend& io_;
    const std::string path_;
    std::mutex mu_;
    int fd_ = -1;
    uint64_t off_ = 0;
    friend class FileEntry;
  };
  void setObsSink(std::shared_ptr<ObsSink> s) { obsSink_ = std::move(s); }
  // Writes the Layer-2 record exactly once (atomic guard): called by the
  // destructor AND by the store's FINAL stats dump — an entry whose last
  // reference is dropped only at process teardown (leaked plugin globals,
  // executor task captures) would otherwise never record.
  void emitObsRecord();
  // Stamp activity onto the span. Cheap (two relaxed atomics, one CAS only on
  // the first call); called from the serve and fill paths.
  void noteActivity();
  // Record that [off, off+len) of the ORIGINAL file was read. Callers on the
  // replica path must translate first (ReplicaView::originRanges); every other
  // route already addresses the origin's layout.
  void noteRead(uint64_t off, uint64_t len) { footprint().note(off, len); }
  // The store hands over a footprint that outlives this entry, so a file
  // opened twice accumulates instead of recording two halves.
  //
  // A NULL pointer means the store's table is full and it is declining to
  // track this file. The entry's own footprint then stands in, but it is
  // POISONED first, so the file records no signature at all -- which is what
  // the store's contract promises for that case. Letting the private one fill
  // up instead is what made the two contracts disagree: an application that
  // opens each file twice (ROOT does) would emit two half-footprints as two
  // confident signatures, the exact wrong answer the cap exists to avoid.
  //
  // Tests that build an entry directly never call this and keep a clean
  // private footprint.
  void useSharedFootprint(ReadFootprint* fp) {
    shared_ = fp;
    if (!fp)
      ownFootprint_.poison();
  }
  ReadFootprint& footprint() { return shared_ ? *shared_ : ownFootprint_; }
  const ReadFootprint& footprint() const { return shared_ ? *shared_ : ownFootprint_; }

 private:
  FileEntry(IOBackend& io, const Config& cfg, Stats& stats, UrlKey key)
      : io_(io), cfg_(cfg), stats_(stats), key_(std::move(key)) {}

  // Reads pages [firstPage, lastPage] with ONE pread into dst (which must
  // hold the run's byte span) and verifies each page's CRC out of that
  // buffer; expect[] holds the run's per-page CRCs. Pages that could not be
  // verified are marked absent. Returns false on mismatch/short read/IO error.
  bool readVerifyRun(uint64_t firstPage, uint64_t lastPage, const uint32_t* expect,
                     uint8_t* dst);
  // Marks [firstPage, lastPage] absent and the entry incomplete: their
  // content could not be trusted. Counts ONE crc_failure for the run.
  void demoteRun(uint64_t firstPage, uint64_t lastPage, const char* why);
  // Re-reads [firstPage, lastPage] one page at a time and demotes only the
  // pages that are genuinely bad (one crc_failure each). Always returns false:
  // the request that hit the failure is unserved either way.
  bool demoteBadPages(uint64_t firstPage, uint64_t lastPage, const uint32_t* expect);
  // Byte span of pages [firstPage, lastPage] (only the file's final page is
  // ever short, so this is exact for any run).
  uint64_t runBytes(uint64_t firstPage, uint64_t lastPage) const {
    return lastPage * uint64_t(meta_.pageSize) + meta_.pageBytes(lastPage) -
           firstPage * uint64_t(meta_.pageSize);
  }
  void touchAtime();
  // Legacy immediate write path (fill_buffer_mb = 0), byte-identical to the
  // pre-buffering behavior; also the fallback if staging is disabled at runtime.
  void writePagesDirect(uint64_t off, uint64_t len, const void* buf);
  // Would open() adopt this sidecar for this origin? Shared by open (both of
  // its reads) and by the store-time merge, so "same entry" has one meaning.
  static bool adoptable(const MetaData& m, const Config& cfg, const UrlKey& key,
                        uint64_t originSize, uint64_t originMtime, uint8_t cksumKind,
                        uint32_t originCksum);
  // Presence transitions, under mu_: the bit, its CRC, and the since-last-store
  // delta the next sidecar commit carries.
  void publishPage(uint64_t pg, uint32_t crc);
  void retractPage(uint64_t pg);

  IOBackend& io_;
  const Config& cfg_;
  Stats& stats_;
  UrlKey key_;
  std::string dataPath_;
  std::string metaPath_;
  int dataFd_ = -1;
  std::function<void(uint64_t, bool)> onPersist_; // usage/eviction hook
  bool closing_ = false; // set by the dtor: final flush counts, never evicts

  std::mutex mu_; // guards meta_, dirty_, lastFlush_, buf_/flushing_ state
  MetaData meta_;
  bool dirty_ = false;
  uint64_t lastFlushS_ = 0; // monotonic-ish seconds of last sidecar persist
  // What THIS handle changed since its last successful sidecar store — the
  // only part of the image it may claim when it commits (see flushMeta).
  PageBitmap setSince_;
  PageBitmap clearedSince_;
  bool pinTouched_ = false;

  // Fill buffer. Page payloads are immutable once staged; both maps are
  // ordered by page index so a flush walk coalesces naturally.
  struct BufPage {
    std::unique_ptr<uint8_t[]> data; // pageBytes(i) bytes (tail page short)
    uint32_t crc = 0;
    bool spec = false; // speculative: serve it, never write it while marked
  };
  std::map<uint64_t, BufPage> buf_;      // staged, awaiting flush (incl. speculative)
  std::map<uint64_t, BufPage> flushing_; // snapshot being written (readable; never speculative)
  uint64_t bufBytes_ = 0;                // REAL staged bytes in buf_ (not flushing_, not speculative)
  uint64_t specBytes_ = 0;               // speculative bytes in buf_
  // Clears a speculative mark under mu_: the page becomes a real staged page
  // (accounting moves from the speculative pools to the fill pools).
  void unmarkSpeculative(BufPage& p, uint32_t nbytes);
  uint64_t lastBufFlushS_ = 0;
  bool flushInProgress_ = false;
  std::condition_variable flushCv_;
  // Process-wide staged total across entries (fill_buffer_total_mb ceiling).
  static std::atomic<uint64_t> g_bufTotal_;
  static std::atomic<uint64_t> g_specTotal_; // process-wide speculative bytes
  // In-flight fetch table: (off,len) -> parked re-dispatch callbacks.
  std::map<std::pair<uint64_t, uint64_t>, std::vector<std::function<void()>>> inflight_;

  // Observability. servedOnce_ (under mu_): pages served at least
  // once, for first_touch accounting — lazily sized, RAM-only, never stored.
  // lastDiskEnd_: end offset of the previous hit-path pread, for the
  // sequential-share counter (as the DISK sees the stream — thread
  // interleaving deliberately counts as non-sequential).
  Obs obs_;
  ReadFootprint ownFootprint_;
  ReadFootprint* shared_ = nullptr;
  std::shared_ptr<ObsSink> obsSink_;
  std::atomic<bool> obsEmitted_{false};
  PageBitmap servedOnce_;
  std::atomic<uint64_t> lastDiskEnd_{~0ull};
};

} // namespace ucache
