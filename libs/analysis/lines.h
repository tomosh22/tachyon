// Internals shared by linesForFunction (lines.cpp) and instructionsForFunction (instructions.cpp): finding
// the frame that holds the outermost occurrence of a function in a callstack. Not part of the public API (the
// queries are declared in query.h).
#pragma once

#include "analysis/attribution.h"
#include "analysis/detail/query_support.h"
#include "analysis/query.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace tachyon::analysis::detail {

// A frame of a stack that belongs to the function, with the lookup variant it was matched by.
struct FunctionHit {
  FrameIdx frame;
  bool caller = false;  // true: matched as a return address (lookupCaller), false: at its exact address
};

// Finds, for callstack nodes, the matching frame closest to the stack root ("outermost occurrence").
// Not thread-safe: one instance per query call; it memoizes per callstack node.
class FunctionMatcher {
 public:
  // `subtree`: the (non-root) node at which stacks are cut (frames above it never match), or invalid.
  FunctionMatcher(const QueryContext& ctx, FunctionKey target, CallstackIdx subtree);

  // The outermost matching frame of the stack ending in `leaf`. `kernelLeaf`: the sample carries
  // kSampleFlagKernelLeaf (the first user frame is then at its exact address). Stacks that do not pass
  // through the subtree node must be rejected by the caller beforehand.
  std::optional<FunctionHit> match(CallstackIdx leaf, bool kernelLeaf);

  // As match(), for a Filter::subtreePath: only the frames at or below the callstack node `cut` (the
  // StackPass::cutNode() of the same sample, the node of the frame holding the path's last key) can match.
  // cut == 0 (the path ends on no physical frame) matches nothing. Not memoized: costs O(stack depth).
  std::optional<FunctionHit> matchWithin(CallstackIdx leaf, bool kernelLeaf, uint32_t cut);

  // Source line (in the function's own file) of a hit; 0 when unknown. See linesForFunction.
  uint32_t lineOf(const FunctionHit& hit);

  // The exact address of the hit frame: its rva (or absolute address when unmapped).
  uint64_t addressOf(const FunctionHit& hit) const;

  // False for keys that cannot match any frame (scope, root and kernel pseudo keys).
  bool matchable() const { return m_matchable; }

 private:
  static constexpr int64_t kUnknown = -2, kNone = -1;

  bool frameMatches(FrameIdx frame, bool caller) const;
  int64_t outermostCaller(uint32_t node);
  std::optional<FunctionHit> matchWalk(uint32_t leaf, bool kernelLeaf, uint32_t cut);
  void ensureOwnLines();

  const QueryContext& m_ctx;
  const model::TraceModel& m_model;
  FunctionKey m_target;
  uint32_t m_subtree = 0;  // 0 = none
  bool m_matchable = true;
  bool m_inline = false;
  uint32_t m_inlineName = 0;
  std::vector<int64_t>
      m_memo;  // per node: outermost caller-variant matching frame at or below, kNone, kUnknown
  std::vector<uint32_t> m_work;
  bool m_ownLoaded = false;
  bool m_haveFile = false;
  StringIdx m_file;
  std::vector<symbols::LineRange> m_ownLines;  // function lines in its own file, sorted by rva
};

// Passes the stack-level parts of a Filter (subtreeRoot, function) for callstack nodes, memoized per
// (node, kernelLeaf). With neither set, every stack passes without any work.
class StackPass {
 public:
  StackPass(const QueryContext& ctx, const ResolvedFilter& f);
  // True when the stack ending in `node` is selected. Sets aborted() when the cancel flag stopped the walk.
  bool pass(CallstackIdx node, bool kernelLeaf, Cancel cancel);
  bool aborted() const { return m_aborted; }
  size_t lastCost() const { return m_cost; }
  bool hasPath() const { return m_hasPath; }
  // With a subtreePath: the cut node (see StackFlattener::pathCutNode) of the stack of the last pass() call
  // that returned true.
  uint32_t cutNode() const { return m_lastCut; }

 private:
  bool m_trivial;
  bool m_hasPath = false;
  uint32_t m_lastCut = 0;
  std::vector<uint32_t> m_cutMemo[2];
  CallstackIdx m_subtree;
  KeyTable m_table;
  StackFlattener m_flat;
  uint32_t m_required = kUnset;
  std::vector<uint8_t> m_memo[2];  // 0 unknown, 1 pass, 2 reject
  std::vector<uint32_t> m_ids;
  bool m_aborted = false;
  size_t m_cost = 0;
};

// The per-leaf-node sample weights of the filter's time range and threads (shared with the aggregation
// queries' cache entry when scope blending is off; computed privately otherwise).
Result<std::shared_ptr<const std::vector<NodeWeight>>> nodeWeightsFor(const QueryContext& ctx,
                                                                      const ResolvedFilter& f, Cancel cancel);

}  // namespace tachyon::analysis::detail
