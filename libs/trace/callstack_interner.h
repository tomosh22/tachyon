// Callstack DAG interner: a flat node vector plus an open-addressing hash that stores only node indices.
#pragma once

#include "core/hash.h"
#include "core/strong_id.h"
#include "trace/trace_format.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tachyon::trace {

// Node 0 is the root {parent = invalid, frame = invalid}. A node (parent, frame) exists at most once.
// The hash maps hash(parent, frame) -> node index (4 bytes per slot) and is verified against the node array.
// Load factor <= 0.7; the table starts at 1024 slots and grows by doubling (compact() may later pick any
// size).
class CallstackInterner {
 public:
  // Not thread-safe.
  CallstackInterner();

  // Not thread-safe. Returns the node for (parent, frame), creating it if needed.
  CallstackIdx intern(CallstackIdx parent, FrameIdx frame) {
    // Fast path: nodes created as one chain are adjacent, so the child of `parent` is very often parent + 1.
    // (parent, frame) is unique, so a match here is the node; the check touches memory we already loaded.
    const size_t guess = static_cast<size_t>(parent.get()) + 1;
    if (guess < m_nodes.size() && m_nodes[guess].parent == parent && m_nodes[guess].frame == frame)
      return CallstackIdx(static_cast<uint32_t>(guess));
    const size_t slots = m_table.size();
    size_t slot = slotFor(parent, frame, slots);
    for (;; slot = (slot + 1 == slots) ? 0 : slot + 1) {
      const uint32_t idx = m_table[slot];
      if (idx == kInvalidIdx) break;
      const CallstackNodeRec& n = m_nodes[idx];
      if (n.parent == parent && n.frame == frame) return CallstackIdx(idx);
    }
    return insertNew(slot, parent, frame);
  }

  // Not thread-safe. Interns a stack given leaf-first; returns the leaf node (0 for an empty span).
  CallstackIdx internStack(std::span<const FrameIdx> leafToRoot) {
    CallstackIdx node(0);
    for (size_t i = leafToRoot.size(); i > 0; --i) node = intern(node, leafToRoot[i - 1]);
    return node;
  }

  // Not thread-safe. Reserves node capacity for n nodes and a hash table that keeps load <= 0.7.
  void reserve(size_t n);
  // Not thread-safe. Call once interning is done: releases excess node capacity and rebuilds the hash with
  // the fewest slots that keep load <= 0.7 (not limited to powers of two). Does not affect growth while
  // interning.
  void compact();

  // Thread-safe for concurrent const access.
  std::span<const CallstackNodeRec> nodes() const { return m_nodes; }
  size_t size() const { return m_nodes.size(); }
  const CallstackNodeRec& node(CallstackIdx idx) const { return m_nodes[idx.get()]; }

  // Memory accounting (thread-safe for const access).
  size_t nodeCapacityBytes() const { return m_nodes.capacity() * sizeof(CallstackNodeRec); }
  size_t hashTableBytes() const { return m_table.size() * sizeof(uint32_t); }
  size_t hashTableSlots() const { return m_table.size(); }
  double bytesPerNode() const {
    return static_cast<double>(nodeCapacityBytes() + hashTableBytes()) / static_cast<double>(size());
  }
  // Worst bytes/node observed immediately after a hash resize (rehash complete, nodes capacity as then).
  double worstBytesPerNodeAfterResize() const { return m_worstAfterResize; }

 private:
  // Maps a hash to [0, slots) with a multiply-shift range reduction (slot counts need not be powers of two).
  static size_t slotFor(CallstackIdx parent, FrameIdx frame, size_t slots) {
    const uint64_t h = core::hashU32Pair(parent.get(), frame.get());
    return static_cast<size_t>(((h >> 32) * static_cast<uint64_t>(slots)) >> 32);
  }
  CallstackIdx insertNew(size_t slot, CallstackIdx parent, FrameIdx frame);
  void rehash(size_t newSlots);

  std::vector<CallstackNodeRec> m_nodes;
  std::vector<uint32_t> m_table;
  double m_worstAfterResize = 0;
};

}  // namespace tachyon::trace
