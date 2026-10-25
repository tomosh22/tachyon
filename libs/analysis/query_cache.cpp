#include "analysis/query_cache.h"

#include "core/hash.h"

namespace tachyon::analysis {

uint64_t hashFilter(const Filter& f) {
  uint64_t h = core::hashU64(f.t0);
  h = core::hashU64(h ^ f.t1);
  h = core::hashU64(h ^ (f.threads ? 0x1ull : 0x2ull));
  if (f.threads) {
    h = core::hashU64(h ^ f.threads->size());
    if (!f.threads->empty()) h = core::hashBytes(f.threads->data(), f.threads->size() * sizeof(ThreadIdx), h);
  }
  h = core::hashU64(h ^ (f.subtreeRoot ? (0x100000000ull | f.subtreeRoot->get()) : 0x3ull));
  h = core::hashU64(h ^ (f.function ? (0x100000000ull | f.function->get()) : 0x4ull));
  return h;
}

uint64_t hashKey(const QueryKey& k) {
  uint64_t h = hashFilter(k.filter);
  h = core::hashU64(h ^ static_cast<uint64_t>(k.kind));
  h = core::hashU64(h ^ k.function.module.get());
  h = core::hashU64(h ^ k.function.value);
  h = core::hashU64(h ^ k.arg);
  return h;
}

QueryCache::List::iterator QueryCache::lookupLocked(const QueryKey& key, uint64_t hash) {
  const auto range = m_index.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second->key == key) return it->second;  // exact equality, not just the hash
  return m_lru.end();
}

std::shared_ptr<const void> QueryCache::findErased(const QueryKey& key) {
  const uint64_t hash = hashKey(key);
  std::lock_guard lock(m_mutex);
  CacheCounters& c = m_counters[static_cast<size_t>(key.kind)];
  const auto it = lookupLocked(key, hash);
  if (it == m_lru.end()) {
    ++c.misses;
    return nullptr;
  }
  ++c.hits;
  m_lru.splice(m_lru.begin(), m_lru, it);
  return it->value;
}

void QueryCache::eraseLocked(List::iterator it) {
  const auto range = m_index.equal_range(it->hash);
  for (auto i = range.first; i != range.second; ++i) {
    if (i->second == it) {
      m_index.erase(i);
      break;
    }
  }
  m_bytes -= it->bytes;
  m_lru.erase(it);
}

void QueryCache::evictLocked() {
  while (m_bytes > m_budget && !m_lru.empty()) {
    const auto last = std::prev(m_lru.end());
    ++m_counters[static_cast<size_t>(last->key.kind)].evictions;
    eraseLocked(last);
  }
}

bool QueryCache::insertErased(const QueryKey& key, std::shared_ptr<const void> value, size_t bytes) {
  const uint64_t hash = hashKey(key);
  std::lock_guard lock(m_mutex);
  const auto existing = lookupLocked(key, hash);
  if (existing != m_lru.end()) eraseLocked(existing);
  bytes += kEntryOverhead + (key.filter.threads ? vectorBytes(*key.filter.threads) : 0);
  if (bytes > m_budget) {
    ++m_counters[static_cast<size_t>(key.kind)].rejected;
    return false;
  }
  m_lru.push_front(Entry{key, std::move(value), bytes, hash});
  m_index.emplace(hash, m_lru.begin());
  m_bytes += bytes;
  evictLocked();
  return true;
}

void QueryCache::setBudget(size_t budgetBytes) {
  std::lock_guard lock(m_mutex);
  m_budget = budgetBytes;
  evictLocked();
}

size_t QueryCache::budget() const {
  std::lock_guard lock(m_mutex);
  return m_budget;
}

void QueryCache::clear() {
  std::lock_guard lock(m_mutex);
  m_index.clear();
  m_lru.clear();
  m_bytes = 0;
}

CacheCounters QueryCache::counters(QueryKind kind) const {
  std::lock_guard lock(m_mutex);
  return m_counters[static_cast<size_t>(kind)];
}

CacheCounters QueryCache::resultCounters() const {
  std::lock_guard lock(m_mutex);
  CacheCounters sum;
  for (size_t k = 0; k < kKinds; ++k) {
    if (k == static_cast<size_t>(QueryKind::Weights)) continue;
    sum.hits += m_counters[k].hits;
    sum.misses += m_counters[k].misses;
    sum.evictions += m_counters[k].evictions;
    sum.rejected += m_counters[k].rejected;
  }
  return sum;
}

CacheCounters QueryCache::totalCounters() const {
  std::lock_guard lock(m_mutex);
  CacheCounters sum;
  for (const CacheCounters& c : m_counters) {
    sum.hits += c.hits;
    sum.misses += c.misses;
    sum.evictions += c.evictions;
    sum.rejected += c.rejected;
  }
  return sum;
}

size_t QueryCache::entryCount() const {
  std::lock_guard lock(m_mutex);
  return m_lru.size();
}

size_t QueryCache::bytesUsed() const {
  std::lock_guard lock(m_mutex);
  return m_bytes;
}

}  // namespace tachyon::analysis
