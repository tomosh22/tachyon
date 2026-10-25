// Append-only string table owned by the SymbolService. Every StringIdx in a ResolvedAddress / InlineFrame /
// LineRange / FunctionBounds indexes THIS table (SymbolService::symbolStrings()), never TraceModel::strings.
#pragma once

#include "core/strong_id.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tachyon::symbols {

// Same read API as trace::StringTable (get / find / size, index 0 is always ""), but safe to read while other
// threads intern:
//  * get() is lock-free and wait-free. Entries live in fixed-size pages whose addresses never change and the
//    characters live in blocks that are never freed or moved, so a returned string_view stays valid for the
//    lifetime of the table (unlike trace::StringTable, where intern() invalidates views).
//  * Publication: intern() writes the entry, then release-stores the published count; get() acquire-loads the
//    count, so an index handed out by intern() (or read from a published ResolvedAddress) is always readable.
//  * intern() / find() take an internal mutex and are safe from any thread.
class SymbolStringTable {
 public:
  // Thread-safe. Pre-interns "" as index 0.
  SymbolStringTable();
  ~SymbolStringTable();
  SymbolStringTable(const SymbolStringTable&) = delete;
  SymbolStringTable& operator=(const SymbolStringTable&) = delete;

  // Thread-safe (internal mutex). Returns the existing index for an equal string or appends a new one.
  // Returns an invalid index only if the table is full (about 67 M strings).
  StringIdx intern(std::string_view s);
  // Thread-safe (internal mutex). Index of s or an invalid index.
  StringIdx find(std::string_view s) const;
  // Thread-safe and lock-free, concurrent with intern(). The view stays valid until the table is destroyed;
  // invalid or not-yet-published indices yield "".
  std::string_view get(StringIdx idx) const;
  // Thread-safe and lock-free. Number of strings excluding the implicit "" at index 0 (same as StringTable).
  size_t size() const { return m_count.load(std::memory_order_acquire) - 1; }

 private:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPageSize = 1u << kPageShift;
  static constexpr uint32_t kMaxPages = 16384;
  static constexpr size_t kBlockSize = 64 * 1024;

  struct Entry {
    const char* data = nullptr;
    uint32_t length = 0;
  };

  const char* store(std::string_view s);  // caller holds m_mutex

  mutable std::mutex m_mutex;
  std::unique_ptr<std::atomic<Entry*>[]> m_pages;
  std::vector<std::unique_ptr<Entry[]>> m_pageStorage;
  std::vector<std::unique_ptr<char[]>> m_blocks;
  size_t m_blockUsed = kBlockSize;
  std::unordered_map<std::string_view, uint32_t> m_index;
  std::atomic<uint32_t> m_count{0};
};

}  // namespace tachyon::symbols
