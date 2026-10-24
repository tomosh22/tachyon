// Bump allocator for short-lived, same-lifetime allocations (parsing, conversion scratch).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace tachyon::core {

// Not thread-safe: one Arena per thread or external synchronization.
class Arena {
 public:
  explicit Arena(size_t blockSize = 64 * 1024) : m_blockSize(blockSize) {}
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
  Arena(Arena&&) noexcept = default;
  Arena& operator=(Arena&&) noexcept = default;

  // Not thread-safe. Returns size bytes aligned to `align` (power of two). Never returns null.
  void* allocate(size_t size, size_t align = alignof(std::max_align_t));

  // Not thread-safe. Allocates and default-constructs `count` objects; T must be trivially destructible.
  template <typename T>
  std::span<T> allocateArray(size_t count) {
    static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
    T* p = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
    for (size_t i = 0; i < count; ++i) new (p + i) T();
    return {p, count};
  }

  // Not thread-safe. Constructs a T in the arena; T must be trivially destructible.
  template <typename T, typename... Args>
  T* make(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>, "Arena never runs destructors");
    return new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }

  // Not thread-safe. Copies a string into the arena (not NUL-terminated unless the input contains one).
  std::string_view copyString(std::string_view s);

  // Not thread-safe. Releases all allocations but keeps the first block for reuse.
  void reset();

  // Thread-safe for concurrent const access.
  size_t bytesAllocated() const { return m_bytesAllocated; }
  size_t bytesReserved() const { return m_bytesReserved; }

 private:
  struct Block {
    std::unique_ptr<std::byte[]> data;
    size_t size = 0;
  };
  std::vector<Block> m_blocks;
  size_t m_blockSize;
  size_t m_offset = 0;  // into m_blocks.back()
  size_t m_bytesAllocated = 0;
  size_t m_bytesReserved = 0;
};

}  // namespace tachyon::core
