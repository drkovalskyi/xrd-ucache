#include "Holdout.h"

namespace ucache {
namespace {
// FNV-1a. Not for security — only for a stable, well-spread mapping from key
// (plus window) to a bucket, identical across processes, builds and hosts.
// std::hash is NOT usable here: libstdc++ hashes strings by pointer-free but
// unspecified means, and nothing guarantees it agrees between two processes of
// different builds, which is exactly what this decision must do.
uint64_t fnv1a(const std::string& s, uint64_t seed) {
  uint64_t h = 1469598103934665603ull ^ seed;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  // Final avalanche: FNV's low bits are weak, and the decision reads a modulus.
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return h;
}
} // namespace

bool holdoutSelected(const std::string& key, int permille, uint64_t rotateSeconds,
                     uint64_t nowS) {
  if (permille <= 0 || key.empty())
    return false;
  if (permille >= 1000)
    return true;
  const uint64_t window = rotateSeconds ? nowS / rotateSeconds : 0;
  return fnv1a(key, window) % 1000ull < static_cast<uint64_t>(permille);
}

} // namespace ucache
