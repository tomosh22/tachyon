#include "symbols/range_table.h"

#include <algorithm>

namespace tachyon::symbols {

FunctionRangeTable::FunctionRangeTable(std::vector<FunctionBounds> functions)
    : m_functions(std::move(functions)) {
  std::stable_sort(m_functions.begin(), m_functions.end(),
                   [](const FunctionBounds& a, const FunctionBounds& b) { return a.startRva < b.startRva; });
}

size_t FunctionRangeTable::findIndex(uint64_t rva) const {
  auto it = std::upper_bound(m_functions.begin(), m_functions.end(), rva,
                             [](uint64_t v, const FunctionBounds& f) { return v < f.startRva; });
  if (it == m_functions.begin()) return kNotFound;
  --it;
  if (rva - it->startRva >= it->size) return kNotFound;
  return static_cast<size_t>(it - m_functions.begin());
}

const FunctionBounds* FunctionRangeTable::find(uint64_t rva) const {
  const size_t i = findIndex(rva);
  return i == kNotFound ? nullptr : &m_functions[i];
}

size_t FunctionRangeTable::findExact(uint64_t startRva) const {
  const auto it = std::lower_bound(m_functions.begin(), m_functions.end(), startRva,
                                   [](const FunctionBounds& f, uint64_t v) { return f.startRva < v; });
  if (it == m_functions.end() || it->startRva != startRva) return kNotFound;
  return static_cast<size_t>(it - m_functions.begin());
}

const LineRange* findLineRange(std::span<const LineRange> ranges, uint64_t rva) {
  auto it = std::upper_bound(ranges.begin(), ranges.end(), rva,
                             [](uint64_t v, const LineRange& r) { return v < r.rva; });
  if (it == ranges.begin()) return nullptr;
  --it;
  if (rva - it->rva >= it->size) return nullptr;
  return &*it;
}

}  // namespace tachyon::symbols
