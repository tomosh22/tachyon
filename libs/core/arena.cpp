#include "core/arena.h"

#include <algorithm>
#include <cstring>

namespace tachyon::core {

void* Arena::allocate(size_t size, size_t align) {
  if (!m_blocks.empty()) {
    Block& block = m_blocks.back();
    const uintptr_t base = reinterpret_cast<uintptr_t>(block.data.get());
    const uintptr_t aligned = (base + m_offset + align - 1) & ~(static_cast<uintptr_t>(align) - 1);
    const size_t newOffset = static_cast<size_t>(aligned - base) + size;
    if (newOffset <= block.size) {
      m_offset = newOffset;
      m_bytesAllocated += size;
      return reinterpret_cast<void*>(aligned);
    }
  }
  const size_t blockSize = std::max(m_blockSize, size + align);
  m_blocks.push_back(Block{std::make_unique<std::byte[]>(blockSize), blockSize});
  m_bytesReserved += blockSize;
  m_offset = 0;
  return allocate(size, align);
}

std::string_view Arena::copyString(std::string_view s) {
  if (s.empty()) return {};
  char* p = static_cast<char*>(allocate(s.size(), 1));
  std::memcpy(p, s.data(), s.size());
  return {p, s.size()};
}

void Arena::reset() {
  if (m_blocks.size() > 1) {
    Block first = std::move(m_blocks.front());
    m_blocks.clear();
    m_blocks.push_back(std::move(first));
  }
  m_bytesReserved = m_blocks.empty() ? 0 : m_blocks.front().size;
  m_offset = 0;
  m_bytesAllocated = 0;
}

}  // namespace tachyon::core
