// Presence bitmap, one bit per page. Maintains a running popcount
// so coverage queries are O(1).
//
// Thread-safety: NOT thread-safe on its own; FileEntry guards it with its
// mutex (single writer discipline, readers under the same lock).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ucache {

class PageBitmap {
 public:
  PageBitmap() = default;
  explicit PageBitmap(uint64_t npages) { reset(npages); }

  void reset(uint64_t npages) {
    npages_ = npages;
    bits_.assign((npages + 7) / 8, 0);
    count_ = 0;
  }

  uint64_t npages() const { return npages_; }
  uint64_t count() const { return count_; }

  bool get(uint64_t i) const { return bits_[i >> 3] & (1u << (i & 7)); }

  void set(uint64_t i) {
    uint8_t m = 1u << (i & 7);
    if (!(bits_[i >> 3] & m)) {
      bits_[i >> 3] |= m;
      ++count_;
    }
  }

  void clear(uint64_t i) {
    uint8_t m = 1u << (i & 7);
    if (bits_[i >> 3] & m) {
      bits_[i >> 3] &= ~m;
      --count_;
    }
  }

  void clearAll() {
    std::fill(bits_.begin(), bits_.end(), 0);
    count_ = 0;
  }

  // All pages in [first, last] present? (inclusive; atomic-chunk test, §5.2)
  bool rangeSet(uint64_t first, uint64_t last) const {
    for (uint64_t i = first; i <= last; ++i)
      if (!get(i))
        return false;
    return true;
  }

  // Serialization surface (MetaFile).
  const uint8_t* raw() const { return bits_.data(); }
  uint8_t* raw() { return bits_.data(); }
  size_t rawSize() const { return bits_.size(); }
  // Recompute count_ after raw() was loaded externally.
  void recount() {
    count_ = 0;
    for (uint64_t i = 0; i < npages_; ++i)
      if (get(i))
        ++count_;
  }

 private:
  std::vector<uint8_t> bits_;
  uint64_t npages_ = 0;
  uint64_t count_ = 0;
};

} // namespace ucache
