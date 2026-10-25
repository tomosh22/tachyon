// QueryCache (WP-33): LRU cache of query results, bounded by an approximate byte budget.
#pragma once

#include "analysis/query.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace tachyon::analysis {

// Which query produced an entry. Weights is the internal per-leaf-node weight table that the tree and row
// queries share; the others are result kinds. WP-34 / WP-35 queries use the reserved kinds.
enum class QueryKind : uint8_t {
  Weights,
  TopDown,
  BottomUp,
  Butterfly,
  Lines,
  Instructions,
  FunctionStats,
  ScopeStats,
  ThreadStateTotals,
  Frames,
};

// Identity of a cached result: the query, its extra arguments, and the (canonical) filter. A hit requires
// exact equality of all three; the hash only selects the bucket.
struct QueryKey {
  QueryKind kind = QueryKind::BottomUp;
  FunctionKey function{};  // function argument (butterfly, lines, ...), else default
  uint64_t arg = 0;        // any other scalar argument (scope id, thread index, ...)
  Filter filter;

  // Thread-safe (value type).
  bool operator==(const QueryKey&) const = default;
};

// Thread-safe: pure function. 64-bit hash of every field of the filter.
uint64_t hashFilter(const Filter& filter);
// Thread-safe: pure function. Hash of the whole key (kind, extra arguments, filter hash).
uint64_t hashKey(const QueryKey& key);

// Approximate heap accounting shared by the producers of cached values. Every heap block is charged
// kAllocOverhead bytes of allocator header/rounding on top of its requested size.
inline constexpr size_t kAllocOverhead = 16;
// Fixed cost charged per entry on top of the value: list node, index node and bucket, shared_ptr control
// block and the key (without the heap block of its thread list, which is added per entry).
inline constexpr size_t kEntryOverhead = 384;

// Thread-safe: pure function. Heap bytes of a vector: its capacity, not its size.
template <typename V>
size_t vectorBytes(const V& v) {
  return v.capacity() * sizeof(typename V::value_type) + (v.capacity() != 0 ? kAllocOverhead : 0);
}

struct CacheCounters {
  uint64_t hits = 0, misses = 0, evictions = 0;
  uint64_t rejected = 0;  // results larger than the whole budget, not cached
};

class QueryCache {
 public:
  static constexpr size_t kDefaultBudgetBytes = size_t{256} << 20;

  // Thread-safe (constructor).
  explicit QueryCache(size_t budgetBytes = kDefaultBudgetBytes) : m_budget(budgetBytes) {}
  QueryCache(const QueryCache&) = delete;
  QueryCache& operator=(const QueryCache&) = delete;

  // Thread-safe. Returns the cached value for `key` (and makes it the most recently used) or nullptr. Counts
  // a hit or a miss. T must be the type the entry was inserted with.
  template <typename T>
  std::shared_ptr<const T> find(const QueryKey& key) {
    return std::static_pointer_cast<const T>(findErased(key));
  }

  // Thread-safe. Stores `value` (approximate heap size `bytes`; the entry's own overhead, kEntryOverhead plus
  // the key's thread list, is added by the cache) as the most recently used entry, replacing an
  // entry with an equal key, then evicts least recently used entries until the total is within the budget.
  // A value larger than the whole budget is not stored (counted as rejected). Returns whether it was stored.
  template <typename T>
  bool insert(const QueryKey& key, std::shared_ptr<const T> value, size_t bytes) {
    return insertErased(key, std::static_pointer_cast<const void>(std::move(value)), bytes);
  }

  // Thread-safe. Changes the budget and evicts down to it.
  void setBudget(size_t budgetBytes);
  size_t budget() const;
  // Thread-safe. Drops every entry (counters are kept). Values already handed out stay alive.
  void clear();

  // All: thread-safe.
  // Counters over the entries of kind `kind` only, and over all kinds but Weights (query results).
  CacheCounters counters(QueryKind kind) const;
  CacheCounters resultCounters() const;
  CacheCounters totalCounters() const;
  size_t entryCount() const;
  size_t bytesUsed() const;

 private:
  struct Entry {
    QueryKey key;
    std::shared_ptr<const void> value;
    size_t bytes = 0;
    uint64_t hash = 0;
  };
  using List = std::list<Entry>;

  std::shared_ptr<const void> findErased(const QueryKey& key);
  bool insertErased(const QueryKey& key, std::shared_ptr<const void> value, size_t bytes);
  void eraseLocked(List::iterator it);
  void evictLocked();
  List::iterator lookupLocked(const QueryKey& key, uint64_t hash);

  static constexpr size_t kKinds = 10;
  mutable std::mutex m_mutex;
  List m_lru;  // front = most recently used
  std::unordered_multimap<uint64_t, List::iterator> m_index;
  size_t m_budget;
  size_t m_bytes = 0;
  CacheCounters m_counters[kKinds];
};

}  // namespace tachyon::analysis
