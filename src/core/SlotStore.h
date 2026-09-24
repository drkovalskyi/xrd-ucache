// The slot store: where a file's converted records live once the file has been
// shown in its slot layout (transpose/FillLayout.h). The layout is the file's
// address space from its first open on, for every process and every open, so a
// reader that holds its offsets can close and reopen at any time.
//
// One file per entry, beside the byte cache's .data/.meta: <hash>.slots
//   [0, 4 KiB)           header, written once: what the layout was made for
//                        (validators, slot factor, codecs), the store's random
//                        id, and the layout blob's length and CRC
//   [4 KiB, ...)         the layout blob, written once: the layout itself,
//                        computed by whoever created the store and served
//                        as-is by everyone after -- never recomputed
//   then commit blocks   each 4 KiB-aligned: a block header, the entries it
//                        commits, then their records
//
// A record's CRC32C is seeded with the store id, the slot and the length, so a
// record only ever validates as the record it was written as, in the store it
// was written to. The last valid entry for a slot wins. A block that a crash
// cut short is stepped over: its header or its records fail their checks.
//
// A DECLINED store has a header and no layout: the file is not served in a
// slot layout, and nobody need parse it again to find that out.
//
// Concurrency: commits from every process are serialized by an exclusive
// flock on the file, taken only by a thread that may wait; reading new blocks
// takes the shared lock without waiting, and is simply skipped when a commit
// holds it. A commit refuses (-ESTALE) once the path no longer names the file
// it has open -- the store was dropped or replaced. Requires a local
// filesystem, as the byte cache's own locking does.
//
// Thread-safety: every method is thread-safe. One SlotStore object may be used
// by all threads of a process; several processes may each have their own.
#pragma once

#include "IOBackend.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ucache {

struct SlotStoreHeader {
  static constexpr uint32_t kFormatVersion = 1;
  uint32_t layoutVersion = 0; // bumped when the layout algorithm changes
  uint8_t container = 0;      // 0 = TTree, 1 = RNTuple
  uint8_t slotFactor = 0;
  bool declined = false;      // not served in a slot layout: nothing else below matters
  std::string codecs;         // which baskets are converted (the rest are kept as stored)
  uint64_t originSize = 0;
  uint64_t originMtime = 0;
  uint8_t cksumKind = 0;
  uint32_t originCksum = 0;
  uint64_t virtualSize = 0;
  uint32_t nSlots = 0;
  uint64_t layoutHash = 0;
  uint64_t storeId = 0;       // set when the store is created
  uint64_t blobLen = 0;
  uint32_t blobCrc = 0;
};

struct SlotEntry {
  enum Kind : uint8_t { kZstd = 1, kRaw = 2, kKept = 3 };
  uint32_t slot = 0;
  uint8_t kind = 0;
  uint64_t off = 0; // record offset in the file (0 for kKept: the original is in the byte cache)
  uint32_t len = 0; // record length (0 for kKept)
  uint32_t crc = 0; // seeded CRC32C of the record bytes
};

struct SlotRecord {
  uint32_t slot = 0;
  uint8_t kind = 0;
  std::vector<uint8_t> bytes; // empty for kKept
};

class SlotStore {
 public:
  static constexpr uint64_t kHeaderBytes = 4096;
  static constexpr uint64_t kAlign = 4096;

  static std::string path(const std::string& objectDir, const std::string& hashHex);

  // The entry's store, or null if there is none, or it is not usable: a header
  // being written right now, a layout blob failing its CRC, a format this
  // build does not know. Nothing is compared against the file here.
  static std::shared_ptr<SlotStore> open(IOBackend& io, const std::string& objectDir,
                                         const std::string& hashHex);

  // The store, created from `want` and `blob` if there is none. When one
  // exists (made by someone else, perhaps a moment ago) it is returned as it
  // is and `created` is false: its header and layout are the ones to serve.
  // Null on I/O failure (err set). `want.storeId` is ignored and generated.
  static std::shared_ptr<SlotStore> openOrCreate(IOBackend& io, const std::string& objectDir,
                                                 const std::string& hashHex, SlotStoreHeader want,
                                                 const std::vector<uint8_t>& blob, bool& created,
                                                 std::string& err);

  // Is there a store here that files are served from (a valid header, not
  // DECLINED)? Reads only the header.
  static bool serving(IOBackend& io, const std::string& objectDir, const std::string& hashHex);

  // Remove the store. Processes that still have it open keep reading it;
  // their commits are refused.
  static void drop(IOBackend& io, const std::string& objectDir, const std::string& hashHex);

  // Remove THIS store: only while the path still names it, and only under its
  // exclusive lock, taken without waiting -- so a store someone replaced it
  // with is never removed, and neither is one a commit holds. False when it
  // did not remove it.
  bool dropIfCurrent();

  ~SlotStore();
  const SlotStoreHeader& header() const { return hdr_; }
  const std::vector<uint8_t>& layoutBlob() const { return blob_; }

  // Entries committed (by anyone) since the last refresh or commit, in commit
  // order. `wait` = wait for a commit in progress (in this process or
  // another); otherwise a busy store returns nothing now and the same entries
  // next time -- it never waits. Cheap when nothing is new: one fstat.
  std::vector<SlotEntry> refresh(bool wait = false);

  // Commit records, under the exclusive lock (this call may wait for it):
  //   1. entries others committed meanwhile go to `onOthers` first;
  //   2. a record whose slot `taken` then reports as committed is skipped;
  //   3. the rest are written as one block at the end.
  // `committed` gets the new entries. `fsync` = fdatasync before unlocking.
  // Returns the record bytes written, 0 when there was nothing to write, or
  // -errno: -ESTALE when the store was dropped or replaced (then nothing of
  // this call is committed; `onOthers` may still have been called).
  int64_t commit(std::vector<SlotRecord>& recs,
                 const std::function<void(const std::vector<SlotEntry>&)>& onOthers,
                 const std::function<bool(uint32_t)>& taken, bool fsync,
                 std::vector<SlotEntry>& committed);

  // Bytes of a committed record, CRC-verified. False on I/O error, a short
  // read, or a CRC mismatch (the caller treats the slot as absent).
  bool readRecord(const SlotEntry& e, std::vector<uint8_t>& out);

  // The store's size on disk.
  uint64_t fileBytes();

  // The seeded CRC a record of `slot` in this store must carry.
  static uint32_t recordCrc(uint64_t storeId, uint32_t slot, const uint8_t* p, size_t n);

 private:
  SlotStore(IOBackend& io, int fd, std::string path, SlotStoreHeader hdr, std::vector<uint8_t> blob);
  std::vector<SlotEntry> readBlocks(uint64_t end); // caller holds a lock on fd_

  IOBackend& io_;
  int fd_ = -1;
  std::string path_;
  SlotStoreHeader hdr_;
  std::vector<uint8_t> blob_;
  std::mutex mu_;     // guards the members below and serializes this process's use of the lock
  uint64_t readOff_ = 0; // next block boundary not yet read
  // The furthest end any block header claims, valid block or not: a block cut
  // short by a crash still owns its extent, and nothing may be written inside
  // it -- once the file grows past it, that header passes its checks and a
  // reader jumps over everything the extent contains.
  uint64_t claimedEnd_ = 0;
};

// Header (de)serialization, exposed for tests.
std::vector<uint8_t> encodeSlotHeader(const SlotStoreHeader& h);
bool decodeSlotHeader(const uint8_t* p, size_t n, SlotStoreHeader& h);

} // namespace ucache
