// Sorted range tables with binary-search lookup (WP-20). Shared by every provider.
#pragma once

#include "symbols/symbol_provider.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tachyon::symbols {

// Immutable after construction; all members are thread-safe for concurrent const access.
class FunctionRangeTable {
 public:
  static constexpr size_t kNotFound = static_cast<size_t>(-1);

  FunctionRangeTable() = default;
  // Not thread-safe (constructor). Stable-sorts by startRva. Overlaps are tolerated: a lookup matches the
  // last function starting at or before the address.
  explicit FunctionRangeTable(std::vector<FunctionBounds> functions);

  // Index into all() of the function containing rva (O(log n)), or kNotFound for gaps / out of range.
  size_t findIndex(uint64_t rva) const;
  // The function containing rva, or nullptr.
  const FunctionBounds* find(uint64_t rva) const;
  // Index of the function whose startRva == rva exactly, or kNotFound.
  size_t findExact(uint64_t startRva) const;
  const std::vector<FunctionBounds>& all() const { return m_functions; }

 private:
  std::vector<FunctionBounds> m_functions;
};

// Thread-safe: pure function. The range of `ranges` (sorted by rva, non-overlapping) containing rva, or
// nullptr.
const LineRange* findLineRange(std::span<const LineRange> ranges, uint64_t rva);

}  // namespace tachyon::symbols
