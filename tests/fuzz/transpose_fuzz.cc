// Mutational fuzz for the ROOT-free transposer: the parser
// (TreeMeta) and overlay builder (Transposer) consume attacker-influenceable
// file bytes with hand-rolled bounds checks — this harness proves the
// fail-open contract holds under hostile input: parse/build either succeed
// or set `error`; they never crash, never throw, never scribble (run the
// ASan build for the memory-safety half of that claim).
//
//   transpose-fuzz [seed-root-file]     (or UCACHE_FUZZ_FILE)
//
// Stage 1 (always): parseKey/decompressFrames over random + magic-seeded
// buffers. Stage 2 (with a seed file — a SMALL ROOT-written TTree):
// whole-file mutants (byte flips biased into the header/tree-key/
// keys-list regions the parser actually reads, big-endian length bombs,
// truncations, extensions, zeroed spans) -> parseFile -> deriveHotBranches ->
// buildOverlay over an in-memory Source.
//
// Knobs: UCACHE_FUZZ_ITERS (file-level mutants, default 1000; buffer-level
// runs 20x that), UCACHE_FUZZ_SEED (default 424242).
// exit 0 = clean; 1 = invariant violation (iter+seed printed); 2 = setup.
#include "Transposer.h"
#include "TreeMeta.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ucache::transpose;

namespace {

uint64_t envU64(const char* name, uint64_t dflt) {
  const char* v = ::getenv(name);
  return v ? ::strtoull(v, nullptr, 10) : dflt;
}

struct MemSource : Source {
  const std::vector<uint8_t>& b;
  explicit MemSource(const std::vector<uint8_t>& v) : b(v) {}
  bool read(void* dst, uint64_t n, uint64_t off) override {
    if (off + n < off || off + n > b.size())
      return false;
    std::memcpy(dst, b.data() + off, n);
    return true;
  }
  bool has(uint64_t off, uint64_t n) override { return off + n >= off && off + n <= b.size(); }
};

// Regions the parser actually reads (from a pristine parse) — mutations
// land here 90% of the time so they hit live parse logic, not dead payload.
struct Region {
  uint64_t off, len;
};

int failures = 0;
void violate(const char* what, uint64_t iter, uint64_t seed) {
  std::fprintf(stderr, "FUZZ VIOLATION: %s (iter=%llu seed=%llu)\n", what,
               (unsigned long long)iter, (unsigned long long)seed);
  ++failures;
}

void bufferFuzz(uint64_t iters, uint64_t seed) {
  std::mt19937_64 rng(seed ^ 0xb0ffe2);
  static const char magics[4][2] = {{'X', 'Z'}, {'Z', 'L'}, {'Z', 'S'}, {'L', '4'}};
  std::vector<uint8_t> buf;
  for (uint64_t i = 0; i < iters; ++i) {
    buf.resize(rng() % 8192);
    for (auto& b : buf)
      b = static_cast<uint8_t>(rng());
    if (!buf.empty() && rng() % 2) { // seed a codec magic to reach frame logic
      const char* m = magics[rng() % 4];
      buf[0] = m[0];
      if (buf.size() > 1)
        buf[1] = m[1];
    }
    try {
      auto k = parseKey(buf.data(), buf.size(), rng() % (buf.size() + 1));
      (void)k;
      auto d = decompressFrames(buf.data(), buf.size(), rng() % (4u << 20));
      (void)d;
    } catch (const std::exception& e) {
      violate(e.what(), i, seed);
      return;
    }
  }
  std::printf("transpose-fuzz: buffer stage clean (%llu iters)\n",
              (unsigned long long)iters);
}

void mutate(std::vector<uint8_t>& m, const std::vector<Region>& regions,
            std::mt19937_64& rng) {
  auto pickOff = [&](uint64_t within) -> uint64_t {
    if (!regions.empty() && rng() % 10 != 0) {
      const Region& r = regions[rng() % regions.size()];
      uint64_t off = r.off + rng() % (r.len ? r.len : 1);
      if (off < within)
        return off;
    }
    return rng() % within;
  };
  switch (rng() % 5) {
  case 0: { // byte flips
    int n = 1 + rng() % 32;
    for (int i = 0; i < n && !m.empty(); ++i)
      m[pickOff(m.size())] ^= static_cast<uint8_t>(1 + rng() % 255);
    break;
  }
  case 1: { // big-endian length bomb: random 4-byte value at a live offset
    if (m.size() < 4)
      break;
    uint64_t off = pickOff(m.size() - 4);
    uint32_t v = static_cast<uint32_t>(rng());
    for (int i = 0; i < 4; ++i)
      m[off + i] = static_cast<uint8_t>(v >> (24 - 8 * i));
    break;
  }
  case 2: // truncate
    m.resize(rng() % (m.size() + 1));
    break;
  case 3: { // extend with noise
    size_t extra = 1 + rng() % 65536;
    size_t old = m.size();
    m.resize(old + extra);
    for (size_t i = old; i < m.size(); ++i)
      m[i] = static_cast<uint8_t>(rng());
    break;
  }
  case 4: { // zero a span
    if (m.empty())
      break;
    uint64_t off = pickOff(m.size());
    uint64_t len = std::min<uint64_t>(1 + rng() % 4096, m.size() - off);
    std::memset(m.data() + off, 0, len);
    break;
  }
  }
}

int fileFuzz(const char* path, uint64_t iters, uint64_t seed) {
  // Pristine image.
  int fd = ::open(path, O_RDONLY);
  if (fd < 0) {
    std::perror("open seed");
    return 2;
  }
  off_t size = ::lseek(fd, 0, SEEK_END);
  std::vector<uint8_t> pristine(size);
  for (off_t done = 0; done < size;) {
    ssize_t r = ::pread(fd, pristine.data() + done, size - done, done);
    if (r <= 0) {
      std::fprintf(stderr, "short seed read\n");
      return 2;
    }
    done += r;
  }
  ::close(fd);

  // The pristine file must parse and build — otherwise the seed is unusable
  // and every mutant would just exercise the early-out.
  FileMeta fm0 = parseFile(path);
  if (!fm0.error.empty() || fm0.branches.empty()) {
    std::fprintf(stderr, "seed does not parse (%s) — wrong seed file?\n",
                 fm0.error.c_str());
    return 2;
  }
  std::vector<Region> regions = {{0, 8192},
                                 {static_cast<uint64_t>(fm0.treeKey.seekkey),
                                  static_cast<uint64_t>(fm0.treeKey.nbytes)},
                                 {static_cast<uint64_t>(fm0.keyslistSeek), 65536},
                                 {fm0.dirSeekKeysOff, 16}};
  {
    MemSource src(pristine);
    Overlay ov = buildOverlay(fm0, src, deriveHotBranches(fm0, src));
    if (!ov.error.empty() || ov.tdata.empty()) {
      std::fprintf(stderr, "seed does not build (%s)\n", ov.error.c_str());
      return 2;
    }
  }

  // Working file for parseFile (path API); rewritten per mutant.
  std::string tmpl = std::string(::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp") +
                     "/transpose-fuzz-XXXXXX";
  std::vector<char> tpath(tmpl.begin(), tmpl.end());
  tpath.push_back('\0');
  int tfd = ::mkstemp(tpath.data());
  if (tfd < 0) {
    std::perror("mkstemp");
    return 2;
  }

  std::mt19937_64 rng(seed);
  uint64_t parsedOk = 0, builtOk = 0;
  std::vector<uint8_t> m;
  for (uint64_t i = 0; i < iters && !failures; ++i) {
    m = pristine;
    mutate(m, regions, rng);
    if (::ftruncate(tfd, 0) != 0 ||
        ::pwrite(tfd, m.data(), m.size(), 0) != static_cast<ssize_t>(m.size())) {
      std::perror("mutant write");
      return 2;
    }
    try {
      FileMeta fm = parseFile(tpath.data());
      if (!fm.error.empty())
        continue; // refused: the fail-open answer
      ++parsedOk;
      MemSource src(m);
      // Bounded rotating hot set: a full-set build on every parsed mutant is
      // O(file); 16 branches exercise the same per-branch patch arithmetic.
      std::vector<std::string> names;
      if (!fm.branches.empty()) {
        size_t start = rng() % fm.branches.size();
        for (size_t k = 0; k < std::min<size_t>(16, fm.branches.size()); ++k)
          names.push_back(fm.branches[(start + k) % fm.branches.size()].name);
      }
      auto hot = withCounters(fm, names, src);
      Overlay ov = buildOverlay(fm, src, hot);
      if (ov.error.empty()) {
        ++builtOk;
        if (ov.tdata.empty() && !hot.empty())
          violate("successful build produced an empty overlay", i, seed);
      }
    } catch (const std::exception& e) {
      violate(e.what(), i, seed); // parse/build must not throw (fail open)
    }
  }
  ::close(tfd);
  ::unlink(tpath.data());
  if (!failures)
    std::printf("transpose-fuzz: file stage clean (%llu mutants, %llu parsed, %llu built)\n",
                (unsigned long long)iters, (unsigned long long)parsedOk,
                (unsigned long long)builtOk);
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  uint64_t iters = envU64("UCACHE_FUZZ_ITERS", 1000);
  uint64_t seed = envU64("UCACHE_FUZZ_SEED", 424242);
  const char* file = argc > 1 ? argv[1] : ::getenv("UCACHE_FUZZ_FILE");

  bufferFuzz(iters * 20, seed);
  if (file && *file) {
    int rc = fileFuzz(file, iters, seed);
    if (rc)
      return rc;
  } else {
    std::printf("transpose-fuzz: no seed file (arg/UCACHE_FUZZ_FILE); buffer stage only\n");
  }
  if (failures) {
    std::fprintf(stderr, "transpose-fuzz: %d violation(s), seed=%llu\n", failures,
                 (unsigned long long)seed);
    return 1;
  }
  std::printf("transpose-fuzz: clean\n");
  return 0;
}
