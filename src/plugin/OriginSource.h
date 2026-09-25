// A metadata parser's byte source over one cache entry: the byte cache when it
// holds the range, otherwise ONE synchronous page-rounded read from the origin,
// kept in the byte cache. What the parsers read this way is the file's own
// records -- header, directory, keys list, tree record, RNTuple anchor and
// envelopes -- which is exactly what the byte cache holds for any reader.
//
// Thread-safety: one instance per parse; the entry and the handle state it
// reads through are thread-safe.
#pragma once

#include "CacheStore.h"
#include "FileEntry.h"
#include "PluginSupport.h"
#include "ReadRounding.h"
#include "TreeMeta.h"
#include "UCacheFile.h"
#include "XrdClTimeout.h"

#include <XrdCl/XrdClFile.hh>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace ucache {

struct OriginSource : transpose::Source {
  static constexpr uint64_t kHeadBlock = 128 * 1024;

  std::shared_ptr<HandleState> st;
  std::shared_ptr<FileEntry> entry;

  bool has(uint64_t off, uint64_t n) override {
    return off + n >= off && off + n <= entry->fileSize();
  }
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (n == 0)
      return true;
    if (entry->hasRange(off, n) && entry->readCached(off, n, dst, /*account=*/false))
      return true;
    auto [ws, we] = roundSpan(entry->pageSize(), entry->fileSize(), off, n);
    return fetch(ws, we, dst, off, n);
  }
  // The file head as ONE block of the size ROOT's RNTuple reader asks for it
  // in (128 KiB at offset 0): piecemeal reads would otherwise leave that read
  // partly missing, and cost one more request. A TTree reader reads only the
  // first few hundred bytes, so this is for RNTuple files only.
  bool fetchHead() {
    const uint64_t n = std::min<uint64_t>(kHeadBlock, entry->fileSize());
    if (entry->hasRange(0, n))
      return true;
    std::vector<char> tmp(1);
    return fetch(0, n, tmp.data(), 0, 0);
  }

 private:
  bool fetch(uint64_t ws, uint64_t we, void* dst, uint64_t off, uint64_t n) {
    if (we - ws > UINT32_MAX)
      return false;
    std::vector<char> buf(we - ws);
    XrdCl::File* f = st->acquireInner();
    if (!f)
      return false;
    uint32_t got = 0;
    const uint64_t t0 = nowUs();
    XrdCl::XRootDStatus s = f->Read(ws, static_cast<uint32_t>(we - ws), buf.data(), got,
                                    static_cast<ucache::XrdTimeout>(0));
    st->releaseInner();
    if (!s.IsOK() || got < off + n - ws)
      return false;
    if (st->store) {
      auto& stats = st->store->stats();
      stats.originBytes.fetch_add(got, std::memory_order_relaxed);
      stats.originReads.fetch_add(1, std::memory_order_relaxed);
      stats.originRtUs.add(nowUs() - t0);
    }
    entry->writePages(ws, got, buf.data());
    if (n)
      std::memcpy(dst, buf.data() + (off - ws), n);
    return true;
  }
};

} // namespace ucache
