#include "trace/callstack_interner.h"

#include <algorithm>

namespace tachyon::trace {

namespace {
constexpr size_t kInitialSlots = 1024;
}

CallstackInterner::CallstackInterner() {
  m_nodes.push_back(CallstackNodeRec{CallstackIdx::invalid(), FrameIdx::invalid()});
  m_table.assign(kInitialSlots, kInvalidIdx);
}

CallstackIdx CallstackInterner::insertNew(size_t slot, CallstackIdx parent, FrameIdx frame) {
  // Entries in the table = size() - 1 (root is not hashed). Keep load <= 0.7 after this insert.
  if (size() * 10 > m_table.size() * 7) {
    rehash(m_table.size() * 2);
    const size_t slots = m_table.size();
    slot = slotFor(parent, frame, slots);
    while (m_table[slot] != kInvalidIdx) slot = (slot + 1 == slots) ? 0 : slot + 1;
  }
  const uint32_t idx = static_cast<uint32_t>(m_nodes.size());
  m_nodes.push_back(CallstackNodeRec{parent, frame});
  m_table[slot] = idx;
  return CallstackIdx(idx);
}

void CallstackInterner::rehash(size_t newSlots) {
  std::vector<uint32_t, HugePageAllocator<uint32_t>> fresh(newSlots, kInvalidIdx);
  for (size_t i = 1; i < m_nodes.size(); ++i) {
    size_t slot = slotFor(m_nodes[i].parent, m_nodes[i].frame, newSlots);
    while (fresh[slot] != kInvalidIdx) slot = (slot + 1 == newSlots) ? 0 : slot + 1;
    fresh[slot] = static_cast<uint32_t>(i);
  }
  m_table.swap(fresh);
  m_worstAfterResize = std::max(m_worstAfterResize, bytesPerNode());
}

void CallstackInterner::reserve(size_t n) {
  m_nodes.reserve(n);
  size_t slots = m_table.size();
  while (n * 10 > slots * 7) slots *= 2;
  if (slots != m_table.size()) rehash(slots);
}

void CallstackInterner::compact() {
  m_nodes.shrink_to_fit();
  // Smallest slot count with (size()-1) / slots <= 0.7, never below the initial size.
  const size_t entries = m_nodes.size() - 1;
  size_t slots = (entries * 10 + 6) / 7;
  slots = std::max(slots, kInitialSlots);
  if (slots != m_table.size()) rehash(slots);
  m_table.shrink_to_fit();
}

}  // namespace tachyon::trace
