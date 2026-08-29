#include "ReadFootprint.h"

#include <cstdio>

namespace ucache {

void ReadFootprint::note(uint64_t off, uint64_t len) {
  if (!len)
    return;
  const uint64_t first = off / kBucket;
  const uint64_t last = (off + len - 1) / kBucket;
  // A hostile or corrupt range must not turn into an enormous allocation:
  // this is a diagnostic, and refusing it costs only the signature.
  if (last < first || last > (1ull << 32))
    return;
  std::lock_guard<std::mutex> g(mu_);
  const size_t need = static_cast<size_t>(last / 64) + 1;
  if (bits_.size() < need)
    bits_.resize(need, 0);
  for (uint64_t b = first; b <= last; ++b)
    bits_[b / 64] |= (1ull << (b % 64));
}

bool ReadFootprint::empty() const {
  std::lock_guard<std::mutex> g(mu_);
  for (uint64_t w : bits_)
    if (w)
      return false;
  return true;
}

namespace {
uint64_t lastBucket(uint64_t limitBytes) {
  return limitBytes ? (limitBytes - 1) / ReadFootprint::kBucket
                    : ~uint64_t(0);
}
} // namespace

uint64_t ReadFootprint::count(uint64_t limitBytes) const {
  std::lock_guard<std::mutex> g(mu_);
  const uint64_t last = lastBucket(limitBytes);
  uint64_t n = 0;
  for (size_t i = 0; i < bits_.size(); ++i) {
    uint64_t w = bits_[i];
    while (w) {
      const uint64_t b = i * 64 + static_cast<uint64_t>(__builtin_ctzll(w));
      w &= w - 1;
      if (b <= last)
        ++n;
    }
  }
  return n;
}

std::vector<uint64_t> ReadFootprint::buckets(uint64_t limitBytes) const {
  std::lock_guard<std::mutex> g(mu_);
  const uint64_t last = lastBucket(limitBytes);
  std::vector<uint64_t> out;
  for (size_t i = 0; i < bits_.size(); ++i) {
    uint64_t w = bits_[i];
    while (w) {
      const uint64_t b = i * 64 + static_cast<uint64_t>(__builtin_ctzll(w));
      w &= w - 1;
      if (b <= last)
        out.push_back(b);
    }
  }
  return out;
}

std::string ReadFootprint::sig(uint64_t limitBytes) const {
  std::lock_guard<std::mutex> g(mu_);
  const uint64_t last = lastBucket(limitBytes);
  uint64_t h = 1469598103934665603ull;
  bool any = false;
  for (size_t i = 0; i < bits_.size(); ++i) {
    uint64_t w = bits_[i];
    while (w) {
      const uint64_t b = i * 64 + static_cast<uint64_t>(__builtin_ctzll(w));
      w &= w - 1;
      if (b > last)
        continue;
      any = true;
      for (int s = 0; s < 8; ++s) {
        h ^= static_cast<unsigned char>((b >> (8 * s)) & 0xff);
        h *= 1099511628211ull;
      }
    }
  }
  if (!any)
    return std::string();
  char buf[24];
  std::snprintf(buf, sizeof buf, "%012llx",
                static_cast<unsigned long long>(h & 0xffffffffffffull));
  return std::string(buf);
}

} // namespace ucache
