// Maps a node of the merged top-down tree (a path of FunctionKeys) back to a callstack node of the model, the
// representation Filter::subtreeRoot needs (WP-43 "filter to subtree").
//
// The tree merges callstack nodes by FunctionKey per depth, so several model nodes can sit behind one tree
// node; the filter takes a single CallstackIdx, so the first match in model order (depth first, children in
// increasing node index) is used. Frames are keyed exactly like the aggregation does (return-address variant
// for non-leaf positions, inline expansion, kernel frame grouping / hiding); a node that is matched only by
// the exact-address (leaf) variant is accepted for the last step of the path.
#pragma once

#include "analysis/query.h"
#include "core/strong_id.h"
#include "symbols/function_key.h"
#include "viewer/analysis_context.h"

#include <optional>
#include <span>
#include <vector>

namespace tachyon::viewer::callgraph {

// Thread-safe (const access). The keys one frame contributes to a stack, outermost first (empty for a hidden
// kernel frame). `caller` selects the return-address variant.
std::vector<symbols::FunctionKey> frameKeys(const analysis::QueryContext& ctx, FrameIdx frame, bool caller);

// Thread-safe (const access). The callstack node reached from `start` (invalid or node 0 = the stack root;
// any other node is taken as already matched) by following `path`, the keys of the tree nodes below the tree
// root. nullopt if no node matches. Iterative; O(nodes below `start`) worst case. An empty path returns
// `start`.
std::optional<CallstackIdx> resolveNodePath(const analysis::QueryContext& ctx, const NodeChildIndex& children,
                                            CallstackIdx start, std::span<const symbols::FunctionKey> path);

}  // namespace tachyon::viewer::callgraph
