// The slot run. A TTree (or RNTuple) file is shown to its reader in its SLOT
// layout (transpose/FillLayout.h): every basket of a convertible branch sits
// in a slot k times its stored length past the original end. The reader's
// first request for a slot fetches the ORIGINAL basket -- the same bytes, in
// the same requests, a plain cold pass would fetch -- converts it to ZSTD-1 as
// it arrives and answers from the converted record.
//
// The layout is the file's address space for good: the first open fixes it in
// the file's slot store (core/SlotStore.h), and every later open, in any
// process, is shown the same one. Converted records go to the store in
// batches and are served from it; a slot nobody has read yet is converted when
// someone does. So a reader can close and reopen at any time, several
// processes reading different parts of a file build one store between them,
// and a process that ends abruptly loses at most its last few seconds of work.
//
// What is NOT stored: a converted basket's original never enters the byte
// cache, and one that was there already is released once its record is
// committed. The byte cache holds only what cannot be converted: the file's
// own records (header, keys list, streamers), branches whose codec is not
// listed, and baskets whose conversion does not fit their slot.
//
// A file with a store is always served in its layout, whatever `recompress`
// says, and a slot read for the first time is converted as usual:
// `recompress = off` only stops a file with no store from getting one.
//
// A file this process reads directly (max_read_fraction, ReadRule.h) is still
// served in its layout -- the reader may hold its offsets -- but what the
// process converts is served and never committed, and nothing it fetches
// enters the byte cache except the file's own records.
//
// Thread-safety: every entry point is thread-safe. The per-file state is shared
// by every handle of the file in the process and guards itself; requests run
// on the executor, conversions on a pool of their own.
#pragma once

#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ucache {

struct HandleState;
class FileEntry;
class UrlKey;
class ColdFill;

// How a slot run may be set up.
enum class AttachMode : uint8_t {
  kExisting, // only from a store that exists and fits the file
  kCreate,   // that, or a new store (recompression on, no replica)
  kMatch,    // only the layout this process already showed (its hash given)
};

// Join (or start) the slot run of `key` for a handle that just set up `entry`.
// Null when the file is not served this way: no fitting store (and, for
// kCreate, not a TTree or RNTuple, nothing convertible, a layout the reader
// could not be shown, or a store that cannot be created). The origin
// validators are the ones the entry was opened with; the store carries them.
std::shared_ptr<ColdFill> coldAttach(const std::shared_ptr<HandleState>& st,
                                     const std::shared_ptr<FileEntry>& entry, const UrlKey& key,
                                     uint64_t originMtime, uint8_t cksumKind, uint32_t originCksum,
                                     AttachMode mode, uint64_t matchHash = 0);

// Which layout this process has shown a file (by key) in. A reader may hold
// offsets from any open it made, so a file is never shown a different layout
// later in the same process: a slot layout stays that slot layout; a compact
// replica's stays compact (or falls back to the original); only the original
// may later become either. `hash` = the slot layout's hash.
enum class ShownLayout : uint8_t { kNone, kOriginal, kCompact, kSlot };
ShownLayout shownLayout(const std::string& key, uint64_t& hash);
// Record that `s` is being shown, with its identity `hash` (a slot layout's
// hash; a compact replica's id; 0 for the original); returns the layout that
// holds for the file in this process from now on (and its identity). When that
// is not `s` and `hash`, another handle got there first, and the caller must
// serve the winner instead. For a slot layout, `run` is the run being shown:
// its slot factor and codecs are kept, so a store removed later is rebuilt
// with them (AttachMode::kMatch) rather than with the settings of the day.
ShownLayout noteShownLayout(const std::string& key, ShownLayout s, uint64_t hash = 0,
                            uint64_t* winnerHash = nullptr, const ColdFill* run = nullptr);

// The handle is done with the run. The process's last handle on the file
// commits what it converted.
void coldDetach(const std::shared_ptr<ColdFill>& cf);

// Commit every record still in memory, for every file. The plugin's periodic
// checkpoint calls it, and so does exit.
void coldCheckpoint();

// The file size the reader is shown.
uint64_t coldVirtualSize(const ColdFill& cf);
// The hash of the layout the run serves.
uint64_t coldLayoutHash(const ColdFill& cf);

// The ORIGINAL-file ranges a read of [off, off+len) of the layout carries: a
// slot is its basket, the relocated tree record is the original tree key, the
// original file's range reads as itself, padding carries nothing.
void coldOriginRanges(const ColdFill& cf, uint64_t off, uint64_t len,
                      std::vector<std::pair<uint64_t, uint64_t>>& out);

// Serve Read/VectorRead-shaped chunks, each already inside the virtual size.
// Completes `user` exactly once: with the chunks (VectorReadInfo when isVRead,
// ChunkInfo of the first chunk otherwise), or with the origin's error.
void coldServe(std::shared_ptr<HandleState> st, std::shared_ptr<FileEntry> entry,
               std::shared_ptr<ColdFill> cf, XrdCl::ChunkList chunks, bool isVRead,
               XrdCl::ResponseHandler* user);

} // namespace ucache
