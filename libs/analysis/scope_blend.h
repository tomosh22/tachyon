// Scope blending (spec 3.8 rule 5, WP-53): which instrumentation scopes are open at each sample, and the
// weights of samples keyed by (callstack node, open-scope stack).
//
// Rule 5 recap (docs/specs/attribution-rules.md, with the monotonic-placement clarification). For a sample at
// `ts` on a thread, the open scopes S_0..S_k (outermost first) are those with begin <= ts < end. For each
// scope, match(S_i) = the depth of the deepest stack frame whose WHOLE function (symbol bounds,
// return-address rule: a non-leaf frame is looked up at address - 1) contains S_i.callerAddr, or the root if
// none; an unmapped frame (no module) matches only its exact address; a mapped frame whose function bounds
// are unknown (module without symbols) never matches. slot(S_i) = max(match(S_i), slot(S_{i-1})); the
// synthetic frame {kScopeModule, S_i.id} is inserted directly above the frame at depth slot-1 (slot 0:
// directly above the root). With inline expansion the scope goes between the physical function key and that
// frame's inline keys (the inline frames of the matched frame are children of the scope). Scopes of one slot
// keep their nesting order. The splice itself lives in detail::StackFlattener (the single place all queries
// share).
//
// The scopes open at a sample are found in O(1) amortized: BlendIndex precomputes, per thread, a sorted list
// of non-overlapping segments [begin, end) with a constant scope stack (a node of a trie of (parent, id,
// callerAddr)), built by one sweep over the paired scopes; ScopeCursor walks it along the samples.
//
// Execution context: per thread, or, with AggregationOptions::groupByFiber and FiberEvents in the trace, the
// fiber that was running on the thread (fibers.h): a segment of thread T is then a stretch of a fiber run on
// T during which that fiber's scope stack is constant. Scopes of a fiber that are open while it is switched
// out never show on a thread.
#pragma once

#include "analysis/attribution.h"
#include "analysis/fibers.h"
#include "analysis/query.h"
#include "analysis/scope_pairing.h"
#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace tachyon::analysis {

// One node of the trie of scope stacks. Node 0 is the empty stack (its other fields are unused).
struct ScopeStackNode {
  uint32_t parent = 0;
  StringIdx id;
  uint64_t callerAddr = 0;
};

// A stretch of one thread's timeline with a constant (non-empty) open-scope stack.
struct ScopeSegment {
  uint64_t begin = 0, end = 0;  // half-open
  uint32_t stack = 0;           // ScopeStackNode index (never 0)
};

struct BlendIndex {
  std::vector<ScopeStackNode> nodes;                // [0] = empty stack
  std::vector<std::vector<ScopeSegment>> byThread;  // per ThreadIdx; sorted, non-overlapping
  bool byFiber = false;                             // built from the fiber-aware pairing
  uint64_t segmentCount = 0;

  // Thread-safe (const access).
  bool empty() const { return segmentCount == 0; }
  size_t heapBytes() const;
  // Thread-safe. The open scopes (outermost first) of node `stack`.
  std::vector<ScopeStackNode> stackOf(uint32_t stack) const;
};

// Thread-safe for concurrent const access. Builds the index from a pairing: byThread of `paired` (per
// thread), or, when `fibers` is not null, from the fiber lanes (see fibers.h). Returns Cancelled if `cancel`
// fires.
Result<BlendIndex> buildBlendIndex(const model::TraceModel& model, const PairedScopes& paired,
                                   const PairedFibers* fibers, Cancel cancel = {});

// Thread-safe (the cache locks internally). The context's blend index, built on first use and cached
// (QueryKind::ScopePairs, arg 2 or 3). Honours ctx.options().groupByFiber. The returned index is empty (no
// segments) when the trace has no scope events.
Result<std::shared_ptr<const BlendIndex>> blendIndexFor(const QueryContext& ctx, Cancel cancel = {});

// Not thread-safe (one cursor per thread of control). Finds the scope stack open at a timestamp along one
// thread's segment list; one comparison pair per lookup while timestamps stay in the same stretch (a segment
// or the gap between two), a short walk when they advance, a binary search for any other jump.
class ScopeCursor {
 public:
  ScopeCursor() = default;
  explicit ScopeCursor(const std::vector<ScopeSegment>& segments) : m_seg(&segments) {}
  // The ScopeStackNode index open at `ts` (0 = none).
  uint32_t stackAt(uint64_t ts) {
    if (ts >= m_lo && ts < m_hi) return m_stack;
    return locate(ts);
  }

 private:
  uint32_t locate(uint64_t ts);
  const std::vector<ScopeSegment>* m_seg = nullptr;
  size_t m_pos = 0;             // first segment that ends after the cached stretch's start
  uint64_t m_lo = 1, m_hi = 0;  // the cached stretch [m_lo, m_hi) (initially empty)
  uint32_t m_stack = 0;
};

// Thread-safe for concurrent const access. As accumulateNodeWeights, but keyed by (leaf node, kernel-leaf
// flag, open scope stack): NodeWeight::scopeStack is the ScopeStackNode of the samples in `index`. The result
// is sorted by (node, kernelLeaf, scopeStack) and does not depend on `workers`.
Result<std::vector<NodeWeight>> accumulateBlendedWeights(const model::TraceModel& model,
                                                         const BlendIndex& index, uint64_t t0, uint64_t t1,
                                                         std::span<const ThreadIdx> threads, unsigned workers,
                                                         Cancel cancel, QueryExecutor* executor = nullptr);

// Thread-safe: pure function. The AggregationOptions bits that change query results (QueryKey::options).
inline uint32_t optionBits(const AggregationOptions& o) {
  return (o.blendScopes ? 1u : 0u) | (o.groupByFiber ? 2u : 0u);
}

}  // namespace tachyon::analysis
