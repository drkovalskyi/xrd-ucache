// The cold replica run: with `recompress = on`, a TTree file with no replica
// yet is served to its reader in a TRANSIENT layout (transpose/FillLayout.h)
// in which every basket of a convertible branch sits in a slot four times its
// stored length. The reader's first request for a slot fetches the ORIGINAL
// basket from the origin (the same bytes, in the same requests, a plain cold
// pass would fetch), converts it to ZSTD-1 as it arrives and answers from the
// converted record. When the process's last handle on the file closes, the
// converted baskets become today's compact replica through today's publish,
// and from then on the file is served warm by the ordinary replica path.
//
// What is NOT stored: a converted basket's original bytes never enter the
// byte cache (the cache would otherwise hold the same data twice). The byte
// cache takes only what cannot be converted: the file's own records (header,
// keys list, streamers), branches whose codec is not listed, and baskets whose
// conversion does not fit their slot.
//
// Nothing persists before the publish. Converted baskets are staged in an
// already-unlinked file next to the entry, so a crash leaves no trace and the
// file simply has no replica yet. Two processes reading one file cold each
// stage their own copy; the first complete publish wins.
//
// A handle keeps the layout it was shown for its whole life: a replica
// published meanwhile (by another handle or process) serves FUTURE opens.
//
// Thread-safety: every entry point is thread-safe. The per-file state is shared
// by every handle of the file in the process and guards itself; requests run
// on the executor, conversions on a pool of their own.
#pragma once

#include <XrdCl/XrdClXRootDResponses.hh>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace ucache {

struct HandleState;
class FileEntry;
class UrlKey;
class ColdFill;

// Join (or start) the cold run of `key` for a handle that just set up `entry`.
// Null when the file is not served this way: not a TTree, nothing convertible,
// a layout the reader could not be shown, or no room to stage. The origin
// validators are the ones the entry was opened with; the published replica
// carries them.
std::shared_ptr<ColdFill> coldAttach(const std::shared_ptr<HandleState>& st,
                                     const std::shared_ptr<FileEntry>& entry, const UrlKey& key,
                                     uint64_t originMtime, uint8_t cksumKind, uint32_t originCksum);

// The handle is done with the run. The last handle's detach publishes.
void coldDetach(const std::shared_ptr<ColdFill>& cf);

// The file size the reader is shown.
uint64_t coldVirtualSize(const ColdFill& cf);

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
