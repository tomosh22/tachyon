#include "analysis/function_registry.h"

#include <mutex>

namespace tachyon::analysis {

FunctionIdx FunctionRegistry::intern(const symbols::FunctionKey& key) {
  {
    std::shared_lock lock(m_mutex);
    const auto it = m_ids.find(key);
    if (it != m_ids.end()) return FunctionIdx(it->second);
  }
  std::unique_lock lock(m_mutex);
  const auto [it, inserted] = m_ids.try_emplace(key, static_cast<uint32_t>(m_keys.size()));
  if (inserted) m_keys.push_back(key);
  return FunctionIdx(it->second);
}

std::optional<FunctionIdx> FunctionRegistry::find(const symbols::FunctionKey& key) const {
  std::shared_lock lock(m_mutex);
  const auto it = m_ids.find(key);
  if (it == m_ids.end()) return std::nullopt;
  return FunctionIdx(it->second);
}

std::optional<symbols::FunctionKey> FunctionRegistry::key(FunctionIdx idx) const {
  std::shared_lock lock(m_mutex);
  if (!idx.valid() || idx.get() >= m_keys.size()) return std::nullopt;
  return m_keys[idx.get()];
}

size_t FunctionRegistry::size() const {
  std::shared_lock lock(m_mutex);
  return m_keys.size();
}

}  // namespace tachyon::analysis
