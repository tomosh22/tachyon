// Interned UTF-8 string table; StringIdx is the ordinal, index 0 is always "".
#pragma once

#include "core/result.h"
#include "core/strong_id.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::trace {

class StringTable {
 public:
  // Not thread-safe. Pre-interns "" as index 0.
  StringTable();

  // Not thread-safe. Returns the existing index for an equal string or appends a new one.
  StringIdx intern(std::string_view s);
  // Thread-safe for concurrent const access. Returns the index of s or an invalid index.
  StringIdx find(std::string_view s) const;
  // Thread-safe for concurrent const access. The view is invalidated by the next intern(); invalid or
  // out-of-range indices yield "".
  std::string_view get(StringIdx idx) const;
  // Thread-safe for concurrent const access.
  size_t size() const { return m_offsets.size() - 1; }

  // Thread-safe for concurrent const access. Appends strings [first, first+count) in chunk-payload form
  // (uint32 length prefix + bytes each).
  void serialize(std::vector<std::byte>& out, size_t first, size_t count) const;
  // Not thread-safe. Appends the strings of a Strings chunk payload, preserving ordinals (duplicates are kept
  // as separate ordinals; find() returns the first).
  Result<void> deserializeAppend(std::span<const std::byte> payload, uint64_t recordCount);

 private:
  void insertIndex(uint32_t idx);
  void grow();

  std::string m_data;
  std::vector<uint64_t> m_offsets;  // size()+1 entries
  std::vector<uint32_t> m_table;    // open addressing, kInvalidIdx = empty
};

}  // namespace tachyon::trace
