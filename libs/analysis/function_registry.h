// FunctionRegistry (WP-33): compact FunctionIdx handles for FunctionKeys, used by Filter::function and the
// UI.
#pragma once

#include "core/strong_id.h"
#include "symbols/function_key.h"

#include <cstddef>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace tachyon::analysis {

// Append-only bidirectional map FunctionKey <-> FunctionIdx. FunctionKey stays the identity of a function;
// FunctionIdx is a dense handle (0, 1, 2, ... in interning order) that is cheap to store in a Filter. Indices
// are never reused or invalidated. A registry belongs to one QueryContext (QueryContext::functions()): its
// indices mean nothing in another context, and they follow the FunctionKeys the context saw, so rebuild the
// context (and drop the handles) when module resolution changes.
class FunctionRegistry {
 public:
  FunctionRegistry() = default;
  FunctionRegistry(const FunctionRegistry&) = delete;
  FunctionRegistry& operator=(const FunctionRegistry&) = delete;

  // Thread-safe. Returns the handle of `key`, adding it on first sight.
  FunctionIdx intern(const symbols::FunctionKey& key);
  // Thread-safe. The handle of `key` if it was interned before.
  std::optional<FunctionIdx> find(const symbols::FunctionKey& key) const;
  // Thread-safe. The key behind a handle; nullopt for an invalid or never issued handle.
  std::optional<symbols::FunctionKey> key(FunctionIdx idx) const;
  // Thread-safe. Number of interned keys.
  size_t size() const;

 private:
  mutable std::shared_mutex m_mutex;
  std::unordered_map<symbols::FunctionKey, uint32_t> m_ids;
  std::vector<symbols::FunctionKey> m_keys;
};

}  // namespace tachyon::analysis
