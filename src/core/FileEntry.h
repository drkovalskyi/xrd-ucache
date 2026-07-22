// One cached object: sparse .data file + .meta sidecar.
//
// The contract the plugin builds on:
//   hasRange()   — atomic-chunk classification (all pages present?)
//   readCached() — HIT path; per-page CRC verified; false => fall back to
//                  origin (pages marked absent, crc_failures counted)
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
// Thread-safety: fully thread-safe. A single mutex guards bitmap/CRC/meta
// state AND the two stage maps; data-file pread/pwrite run outside the
// lock (page writes are idempotent-same-bytes; presence bits only ever
// transition absent→present under the lock, and present→absent only on
// CRC failure / clearAll). At most one buffer flush runs at a time
// (flushInProgress_ + condvar); staged pages stay readable during their
// flush via the flushing_ map.
#pragma once

#include "Config.h"
#include "IOBackend.h"
#include "MetaFile.h"
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
  bool readCached(uint64_t off, uint64_t len, void* buf);
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
  };
  Obs& obs() { return obs_; }

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

 private:
  FileEntry(IOBackend& io, const Config& cfg, Stats& stats, UrlKey key)
      : io_(io), cfg_(cfg), stats_(stats), key_(std::move(key)) {}

  // Reads page i fully and verifies its CRC; on mismatch marks it absent.
  // scratch must hold pageSize bytes. Returns false on mismatch/IO error.
  bool readVerifyPage(uint64_t i, uint32_t expectCrc, uint8_t* scratch);
  void touchAtime();
  // Legacy immediate write path (fill_buffer_mb = 0), byte-identical to the
  // pre-buffering behavior; also the fallback if staging is disabled at runtime.
  void writePagesDirect(uint64_t off, uint64_t len, const void* buf);

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

  // Fill buffer. Page payloads are immutable once staged; both maps are
  // ordered by page index so a flush walk coalesces naturally.
  struct BufPage {
    std::unique_ptr<uint8_t[]> data; // pageBytes(i) bytes (tail page short)
    uint32_t crc = 0;
  };
  std::map<uint64_t, BufPage> buf_;      // staged, awaiting flush
  std::map<uint64_t, BufPage> flushing_; // snapshot being written (readable)
  uint64_t bufBytes_ = 0;                // staged bytes in buf_ (not flushing_)
  uint64_t lastBufFlushS_ = 0;
  bool flushInProgress_ = false;
  std::condition_variable flushCv_;
  // Process-wide staged total across entries (fill_buffer_total_mb ceiling).
  static std::atomic<uint64_t> g_bufTotal_;
  // In-flight fetch table: (off,len) -> parked re-dispatch callbacks.
  std::map<std::pair<uint64_t, uint64_t>, std::vector<std::function<void()>>> inflight_;

  // Observability. servedOnce_ (under mu_): pages served at least
  // once, for first_touch accounting — lazily sized, RAM-only, never stored.
  // lastDiskEnd_: end offset of the previous hit-path pread, for the
  // sequential-share counter (as the DISK sees the stream — thread
  // interleaving deliberately counts as non-sequential).
  Obs obs_;
  std::shared_ptr<ObsSink> obsSink_;
  std::atomic<bool> obsEmitted_{false};
  PageBitmap servedOnce_;
  std::atomic<uint64_t> lastDiskEnd_{~0ull};
};

} // namespace ucache
