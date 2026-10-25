// Interval index with correct nested / overlapping semantics (WP-72), used for the ELF symbol table and for
// the DWARF function / inline / compile-unit ranges. Same technique as DiaSymbolTable: ranges sorted by start
// plus a running-maximum end array, so a lookup scans back only while some earlier range can still reach the
// address.
#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace tachyon::symbols {

struct IndexedRange {
  uint64_t lo = 0;
  uint64_t hi = 0;  // exclusive
  uint32_t payload = 0;
};

class RangeIndex {
 public:
  // Not thread-safe (build once). Drops empty ranges (hi <= lo); sorts by lo, equal starts keep input order.
  void build(std::vector<IndexedRange> ranges) {
    ranges.erase(
        std::remove_if(ranges.begin(), ranges.end(), [](const IndexedRange& r) { return r.hi <= r.lo; }),
        ranges.end());
    std::stable_sort(ranges.begin(), ranges.end(),
                     [](const IndexedRange& a, const IndexedRange& b) { return a.lo < b.lo; });
    m_maxEnd.clear();
    m_maxEnd.reserve(ranges.size());
    uint64_t running = 0;
    for (const IndexedRange& r : ranges) {
      running = std::max(running, r.hi);
      m_maxEnd.push_back(running);
    }
    m_ranges = std::move(ranges);
  }

  // Thread-safe after build (const). Calls fn(range) for every range containing addr, greatest start first
  // (later input first among equal starts), until fn returns true.
  template <typename Fn>
  void forEachContaining(uint64_t addr, Fn&& fn) const {
    auto it = std::upper_bound(m_ranges.begin(), m_ranges.end(), addr,
                               [](uint64_t v, const IndexedRange& r) { return v < r.lo; });
    for (size_t i = static_cast<size_t>(it - m_ranges.begin()); i > 0; --i) {
      const size_t j = i - 1;
      if (m_maxEnd[j] <= addr) break;
      const IndexedRange& r = m_ranges[j];
      if (addr < r.hi && fn(r)) return;
    }
  }

  // Thread-safe after build (const). The containing range with the greatest start, or nullptr.
  const IndexedRange* find(uint64_t addr) const {
    const IndexedRange* out = nullptr;
    forEachContaining(addr, [&](const IndexedRange& r) {
      out = &r;
      return true;
    });
    return out;
  }

  size_t size() const { return m_ranges.size(); }
  bool empty() const { return m_ranges.empty(); }
  const std::vector<IndexedRange>& ranges() const { return m_ranges; }

 private:
  std::vector<IndexedRange> m_ranges;
  std::vector<uint64_t> m_maxEnd;
};

}  // namespace tachyon::symbols
