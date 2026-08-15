// Shared test helpers: RAII temp dir + deterministic random buffers.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ftw.h>
#include <random>
#include <string>
#include <unistd.h> // mkdtemp lives here on BSD-derived libcs, in <stdlib.h> on glibc
#include <vector>

namespace ucache::test {

inline int rmCb(const char* path, const struct stat*, int, struct FTW*) {
  return ::remove(path);
}

class TempDir {
 public:
  TempDir() {
    const char* base = ::getenv("TMPDIR");
    tpl_ = std::string(base ? base : "/tmp") + "/ucache-test-XXXXXX";
    path_ = ::mkdtemp(tpl_.data());
  }
  ~TempDir() {
    if (!path_.empty())
      ::nftw(path_.c_str(), rmCb, 16, FTW_DEPTH | FTW_PHYS);
  }
  const std::string& path() const { return path_; }

 private:
  std::string tpl_;
  std::string path_;
};

inline std::vector<uint8_t> randomBytes(size_t n, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<uint8_t> out(n);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v = rng();
    std::memcpy(out.data() + i, &v, 8);
  }
  for (; i < n; ++i)
    out[i] = static_cast<uint8_t>(rng());
  return out;
}

} // namespace ucache::test
