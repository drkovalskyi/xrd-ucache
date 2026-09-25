#include "ReadMap.h"

#include "RNTupleMeta.h"
#include "TreeMeta.h"

#include <algorithm>
#include <iterator>
#include <numeric>

namespace ucache::transpose {

namespace {

constexpr uint64_t kRNTupleHead = 128 * 1024;

// Sort the units by offset, carrying the per-unit arrays with them.
void sortUnits(std::vector<uint64_t>& off, std::vector<uint32_t>& len,
               std::vector<uint32_t>& group) {
  std::vector<uint32_t> idx(off.size());
  std::iota(idx.begin(), idx.end(), 0u);
  std::stable_sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return off[a] < off[b]; });
  auto permute = [&](auto& v) {
    auto out = v;
    for (size_t k = 0; k < idx.size(); ++k)
      out[k] = v[idx[k]];
    v.swap(out);
  };
  permute(off);
  permute(len);
  permute(group);
}

} // namespace

ReadMap ReadMap::fromTree(const FileMeta& fm) {
  ReadMap m;
  uint32_t g = 0;
  for (const auto& b : fm.branches) {
    const uint32_t group = g++;
    if (b.externalFile)
      continue;
    const size_t n = std::min(b.basketSeek.size(), b.basketBytes.size());
    for (size_t i = 0; i < n; ++i) {
      if (b.basketSeek[i] <= 0 || b.basketBytes[i] <= 0)
        continue;
      m.off_.push_back(static_cast<uint64_t>(b.basketSeek[i]));
      m.len_.push_back(static_cast<uint32_t>(b.basketBytes[i]));
      m.group_.push_back(group);
    }
  }
  sortUnits(m.off_, m.len_, m.group_);
  m.groupBytes_.assign(g, 0);
  for (size_t k = 0; k < m.off_.size(); ++k) {
    m.groupBytes_[m.group_[k]] += m.len_[k];
    m.total_ += m.len_[k];
  }
  return m;
}

ReadMap ReadMap::fromRNTuple(const RNTupleMeta& rm) {
  ReadMap m;
  m.rnt_ = true;
  m.headSkip_ = kRNTupleHead;
  uint32_t maxColumn = 0;
  for (const auto& cr : rm.ranges)
    for (const auto& p : cr.pages) {
      if (p.nbytes == 0)
        continue;
      m.off_.push_back(p.offset);
      m.len_.push_back(p.nbytes + (p.hasChecksum ? 8u : 0u));
      m.group_.push_back(cr.columnId);
      maxColumn = std::max(maxColumn, cr.columnId);
    }
  sortUnits(m.off_, m.len_, m.group_);
  // One unit per offset: a page two columns point at is still one page.
  size_t w = 0;
  for (size_t r = 0; r < m.off_.size(); ++r) {
    if (w > 0 && m.off_[r] == m.off_[w - 1])
      continue;
    m.off_[w] = m.off_[r];
    m.len_[w] = m.len_[r];
    m.group_[w] = m.group_[r];
    ++w;
  }
  m.off_.resize(w);
  m.len_.resize(w);
  m.group_.resize(w);
  m.groupBytes_.assign(m.off_.empty() ? 0 : maxColumn + 1, 0);
  for (size_t k = 0; k < m.off_.size(); ++k) {
    m.groupBytes_[m.group_[k]] += m.len_[k];
    m.total_ += m.len_[k];
  }
  return m;
}

std::vector<uint32_t>
ReadMap::groupsOf(const std::vector<std::pair<uint64_t, uint64_t>>& ranges) const {
  std::vector<uint32_t> groups;
  if (off_.empty())
    return groups;
  // Sorted and merged first, so a unit split over adjacent chunks counts whole
  // and a chunk asked for twice counts once.
  std::vector<std::pair<uint64_t, uint64_t>> iv;
  iv.reserve(ranges.size());
  for (const auto& [o, n] : ranges) {
    if (n == 0 || o + n < o)
      continue;
    const uint64_t a = std::max(o, headSkip_), b = o + n;
    if (a < b)
      iv.emplace_back(a, b);
  }
  std::sort(iv.begin(), iv.end());
  std::vector<std::pair<uint64_t, uint64_t>> merged;
  for (const auto& r : iv) {
    if (!merged.empty() && r.first <= merged.back().second)
      merged.back().second = std::max(merged.back().second, r.second);
    else
      merged.push_back(r);
  }
  std::vector<std::pair<uint32_t, uint64_t>> covered; // (unit, bytes covered)
  for (const auto& [a, b] : merged) {
    // The last unit starting at or before a (it may reach into [a, b)), then
    // on while units start before b.
    size_t k = static_cast<size_t>(std::upper_bound(off_.begin(), off_.end(), a) - off_.begin());
    if (k > 0)
      --k;
    for (; k < off_.size() && off_[k] < b; ++k) {
      const uint64_t ue = off_[k] + len_[k];
      if (ue > a)
        covered.emplace_back(static_cast<uint32_t>(k), std::min(b, ue) - std::max(a, off_[k]));
    }
  }
  std::sort(covered.begin(), covered.end());
  for (size_t i = 0; i < covered.size();) {
    const uint32_t k = covered[i].first;
    uint64_t n = 0;
    for (; i < covered.size() && covered[i].first == k; ++i)
      n += covered[i].second;
    if (2 * n >= len_[k])
      groups.push_back(group_[k]);
  }
  std::sort(groups.begin(), groups.end());
  groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
  return groups;
}

ReadMap::Share ReadMap::weigh(const std::vector<uint32_t>& groups) const {
  Share s;
  s.total = total_;
  for (uint32_t g : groups)
    if (g < groupBytes_.size()) {
      s.asked += groupBytes_[g];
      ++s.groups;
    }
  return s;
}

bool ReadMap::exceeds(const Share& s, int limitPercent) {
  return s.total && s.asked * 100 > static_cast<uint64_t>(limitPercent) * s.total;
}

int ReadMap::step(std::vector<uint32_t>& seen,
                  const std::vector<std::pair<uint64_t, uint64_t>>& ranges, int limitPercent,
                  Share& out) const {
  const std::vector<uint32_t> now = groupsOf(ranges);
  if (now.empty()) {
    out = weigh(seen);
    return -1;
  }
  std::vector<uint32_t> all;
  all.reserve(seen.size() + now.size());
  std::set_union(seen.begin(), seen.end(), now.begin(), now.end(), std::back_inserter(all));
  const bool grew = all.size() > seen.size();
  seen.swap(all);
  out = weigh(seen);
  if (exceeds(out, limitPercent))
    return 1;
  if (now.size() >= 2 && !grew)
    return 0;
  return -1;
}

bool ReadMap::touches(uint64_t off, uint64_t len) const {
  if (off_.empty() || len == 0)
    return false;
  const uint64_t a = std::max(off, headSkip_), b = off + len;
  if (a >= b)
    return false;
  size_t k = static_cast<size_t>(std::upper_bound(off_.begin(), off_.end(), a) - off_.begin());
  if (k > 0)
    --k;
  for (; k < off_.size() && off_[k] < b; ++k)
    if (off_[k] + len_[k] > a)
      return true;
  return false;
}

std::vector<std::pair<uint64_t, uint64_t>> ReadMap::dataRuns() const {
  std::vector<std::pair<uint64_t, uint64_t>> runs;
  for (size_t k = 0; k < off_.size(); ++k) {
    const uint64_t a = std::max(off_[k], headSkip_), b = off_[k] + len_[k];
    if (a >= b)
      continue;
    if (!runs.empty() && a <= runs.back().second)
      runs.back().second = std::max(runs.back().second, b);
    else
      runs.emplace_back(a, b);
  }
  return runs;
}

uint64_t ReadMap::memoryBytes() const {
  return off_.size() * (sizeof(uint64_t) + 2 * sizeof(uint32_t)) +
         groupBytes_.size() * sizeof(uint64_t);
}

FileMeta parseReaderTree(Source& src, int64_t size) {
  FileMeta fm = parseFile(src, size, "Events");
  if (!fm.error.empty() && fm.error.find("not found") != std::string::npos) {
    ContainerMeta cm = parseContainer(src, size);
    for (const auto& k : cm.keys)
      if (k.cls == "TTree") {
        fm = parseFile(src, size, k.name);
        break;
      }
  }
  return fm;
}

} // namespace ucache::transpose
