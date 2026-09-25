#include "CopyDetect.h"

#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <dlfcn.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif

#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define UCACHE_HAVE_BACKTRACE 1
#endif

namespace ucache {
namespace {

// A function whose size the loader does not report is matched in a window
// after its start, and a frame inside that window is then confirmed to belong
// to it by one symbol lookup. Only such frames pay for a lookup, and they are
// rare: the window is code the copy functions themselves occupy.
constexpr uintptr_t kProbeWindow = 64 * 1024;

struct Range {
  uintptr_t lo = 0, hi = 0;
  CopySignal sig = CopySignal::kNone;
  bool confirm = false; // hi is a bound only: confirm lo is the frame's function
};

// Changes whenever a library is loaded or unloaded.
struct Generation {
  uint64_t adds = 0, subs = 0;
  bool operator==(const Generation& o) const { return adds == o.adds && subs == o.subs; }
};

// ROOT's I/O library as last resolved: reused while it stays loaded at the
// same address, so a rebuild after an unrelated library loads costs no lookups.
struct RootIo {
  std::string path;
  uintptr_t base = 0;
  std::vector<Range> ranges;
};

struct Table {
  Generation gen;
  const void* anchor = nullptr;
  std::vector<Range> copyEngine; // fixed for the life of the anchor's library
  std::vector<RootIo> rootIo;
  std::vector<Range> all; // what a walk compares against
};

// Leaked, like the plugin's other process-wide state: detached threads may
// open files while the process exits.
std::shared_ptr<const Table>* tableSlot() {
  static auto* slot = new std::shared_ptr<const Table>();
  return slot;
}
std::atomic<const void*> gAnchor{nullptr};

#if defined(__APPLE__)
Generation loaderGeneration() { return {_dyld_image_count(), 0}; }
#else
int generationCb(struct dl_phdr_info* info, size_t size, void* data) {
  auto* g = static_cast<Generation*>(data);
  if (size >= offsetof(struct dl_phdr_info, dlpi_subs) + sizeof(info->dlpi_subs)) {
    g->adds = info->dlpi_adds;
    g->subs = info->dlpi_subs;
  }
  return 1; // the counters are the same in every entry: the first one is enough
}
Generation loaderGeneration() {
  Generation g;
  ::dl_iterate_phdr(generationCb, &g);
  return g;
}
#endif

void addFunction(std::vector<Range>& out, void* fn, CopySignal sig) {
  if (!fn)
    return;
  const auto lo = reinterpret_cast<uintptr_t>(fn);
#if defined(__GLIBC__)
  ::Dl_info info{};
  void* extra = nullptr;
  const bool found = ::dladdr1(fn, &info, &extra, RTLD_DL_SYMENT) != 0;
  const auto* sym = static_cast<const ElfW(Sym)*>(extra);
  if (found && sym && sym->st_size > 0 && info.dli_saddr == fn) {
    out.push_back({lo, lo + sym->st_size, sig, false});
    return;
  }
#endif
  // Mach-O records no symbol sizes, and a stripped ELF symbol may lack one.
  out.push_back({lo, lo + kProbeWindow, sig, true});
}

// Look `names` up in the already loaded library at `path` (never loading it).
void addFromLibrary(std::vector<Range>& out, const char* path, const char* const* names, size_t n,
                    CopySignal sig) {
  if (!path || !*path)
    return;
  void* h = ::dlopen(path, RTLD_LAZY | RTLD_NOLOAD);
  if (!h)
    return;
  for (size_t i = 0; i < n; ++i)
    addFunction(out, ::dlsym(h, names[i]), sig);
  ::dlclose(h); // drops only the reference RTLD_NOLOAD took
}

struct Loaded {
  std::string path;
  uintptr_t base;
};

#if !defined(__APPLE__)
struct ScanOut {
  std::vector<Range>* gfal;
  std::vector<Loaded>* rootIo;
};
// Runs under the loader's lock: collect, never load or look anything up here.
int scanCb(struct dl_phdr_info* info, size_t, void* data) {
  auto* s = static_cast<ScanOut*>(data);
  if (!info->dlpi_name || !*info->dlpi_name)
    return 0; // the program itself
  const std::string name = info->dlpi_name;
  if (isGfalXrootdObject(name)) {
    for (int i = 0; i < info->dlpi_phnum; ++i) {
      const auto& ph = info->dlpi_phdr[i];
      if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X)) {
        const uintptr_t lo = info->dlpi_addr + ph.p_vaddr;
        s->gfal->push_back({lo, lo + ph.p_memsz, CopySignal::kGfal, false});
      }
    }
  } else if (isRootIoObject(name)) {
    s->rootIo->push_back({name, static_cast<uintptr_t>(info->dlpi_addr)});
  }
  return 0;
}
#endif

std::shared_ptr<const Table> buildTable(Generation gen, const Table* prev) {
  auto t = std::make_shared<Table>();
  t->gen = gen;
  t->anchor = gAnchor.load(std::memory_order_acquire);
  std::vector<Range> gfal;
  std::vector<Loaded> rootIo;
#if defined(__APPLE__)
  for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i)
    if (const char* name = _dyld_get_image_name(i); name && isRootIoObject(name))
      rootIo.push_back({name, reinterpret_cast<uintptr_t>(_dyld_get_image_header(i))});
#else
  ScanOut s{&gfal, &rootIo};
  ::dl_iterate_phdr(scanCb, &s);
#endif
  if (prev && prev->anchor == t->anchor) {
    t->copyEngine = prev->copyEngine; // the anchor's library cannot have moved
  } else if (t->anchor) {
    ::Dl_info info{};
    if (::dladdr(t->anchor, &info) != 0 && info.dli_fname)
      addFromLibrary(t->copyEngine, info.dli_fname, kCopyEngineSymbols,
                     sizeof kCopyEngineSymbols / sizeof kCopyEngineSymbols[0],
                     CopySignal::kCopyEngine);
  }
  for (const Loaded& l : rootIo) {
    RootIo r{l.path, l.base, {}};
    const RootIo* same = nullptr;
    if (prev)
      for (const RootIo& p : prev->rootIo)
        if (p.path == l.path && p.base == l.base)
          same = &p;
    if (same)
      r.ranges = same->ranges;
    else
      addFromLibrary(r.ranges, l.path.c_str(), &kRootCpSymbol, 1, CopySignal::kRootCp);
    t->rootIo.push_back(std::move(r));
  }
  t->all = t->copyEngine;
  for (const RootIo& r : t->rootIo)
    t->all.insert(t->all.end(), r.ranges.begin(), r.ranges.end());
  t->all.insert(t->all.end(), gfal.begin(), gfal.end());
  return t;
}

// The table for the libraries loaded now. Rebuilt without a lock when the set
// changes; two threads that notice at once each build one and the last store
// wins -- both are correct.
std::shared_ptr<const Table> currentTable() {
  const Generation gen = loaderGeneration();
  std::shared_ptr<const Table> t = std::atomic_load_explicit(tableSlot(), std::memory_order_acquire);
  if (t && t->gen == gen && t->anchor == gAnchor.load(std::memory_order_acquire))
    return t;
  t = buildTable(gen, t.get());
  std::atomic_store_explicit(tableSlot(), t, std::memory_order_release);
  return t;
}

std::string computeExecutable() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size); // reports the size it needs
  std::string p(size + 1, '\0');
  if (_NSGetExecutablePath(p.data(), &size) != 0)
    return "";
  p.resize(std::strlen(p.c_str()));
  char real[PATH_MAX];
  if (::realpath(p.c_str(), real)) // an alias installed as a link names its target
    p = real;
  return executableBaseName(p);
#elif defined(__linux__)
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0)
    return "";
  buf[n] = '\0';
  return executableBaseName(buf);
#else
  return "";
#endif
}

} // namespace

const char* copySignalName(CopySignal s) {
  switch (s) {
    case CopySignal::kNone: return "no copy";
    case CopySignal::kExecutable: return "a copy tool";
    case CopySignal::kCopyEngine: return "XRootD's copy engine";
    case CopySignal::kRootCp: return "ROOT's TFile::Cp";
    case CopySignal::kGfal: return "gfal2's xrootd plugin";
  }
  return "?";
}

std::string executableBaseName(const std::string& path) {
  static const std::string kDeleted = " (deleted)";
  std::string p = path;
  if (p.size() > kDeleted.size() &&
      p.compare(p.size() - kDeleted.size(), kDeleted.size(), kDeleted) == 0)
    p.resize(p.size() - kDeleted.size());
  const auto slash = p.find_last_of('/');
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

bool isCopyToolExecutable(const std::string& base) {
  for (const char* name : kCopyToolExecutables)
    if (base == name)
      return true;
  return false;
}

const std::string& hostExecutable() {
  static const std::string* exe = new std::string(computeExecutable());
  return *exe;
}

bool isGfalXrootdObject(const std::string& path) {
  return executableBaseName(path).rfind(kGfalXrootdPrefix, 0) == 0;
}

bool isRootIoObject(const std::string& path) {
  const std::string b = executableBaseName(path);
  return b == "libRIO.so" || b == "libRIO.dylib" || b.rfind("libRIO.so.", 0) == 0;
}

void copyDetectInit(const void* xrdclAnchor) {
  gAnchor.store(xrdclAnchor, std::memory_order_release);
  (void)hostExecutable();
#ifdef UCACHE_HAVE_BACKTRACE
  void* one[1];
  (void)::backtrace(one, 1); // the first call may load the unwinder: not inside an open
#endif
  std::atomic_store_explicit(tableSlot(), buildTable(loaderGeneration(), nullptr),
                             std::memory_order_release);
}

CopySignal copierSignal() {
  if (isCopyToolExecutable(hostExecutable()))
    return CopySignal::kExecutable;
  return copierStackSignal(kCopyStackDepth);
}

CopySignal copierStackSignal(int depth) {
#ifdef UCACHE_HAVE_BACKTRACE
  constexpr int kMaxDepth = 256;
  if (depth <= 1)
    return CopySignal::kNone;
  if (depth > kMaxDepth)
    depth = kMaxDepth;
  const std::shared_ptr<const Table> t = currentTable();
  if (t->all.empty())
    return CopySignal::kNone;
  void* pcs[kMaxDepth];
  // A walk that stops early -- code without unwind information -- simply
  // sees fewer frames: the copy goes unrecognised, as before this existed.
  const int n = ::backtrace(pcs, depth);
  for (int i = 1; i < n; ++i) { // frame 0 is this function
    // A return address points past its call; the call is one byte back.
    const uintptr_t pc = reinterpret_cast<uintptr_t>(pcs[i]) - 1;
    for (const Range& r : t->all) {
      if (pc < r.lo || pc >= r.hi)
        continue;
      if (r.confirm) {
        ::Dl_info info{};
        if (::dladdr(reinterpret_cast<void*>(pc), &info) == 0 ||
            reinterpret_cast<uintptr_t>(info.dli_saddr) != r.lo)
          continue;
      }
      return r.sig;
    }
  }
#else
  (void)depth;
#endif
  return CopySignal::kNone;
}

} // namespace ucache
