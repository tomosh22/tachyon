#include "viewer/callgraph/subtree_resolver.h"

#include "analysis/attribution.h"

namespace tachyon::viewer::callgraph {

using symbols::FunctionKey;

std::vector<FunctionKey> frameKeys(const analysis::QueryContext& ctx, FrameIdx frame, bool caller) {
  std::vector<FunctionKey> keys;
  const model::TraceModel& m = ctx.model();
  if (!frame.valid() || frame.get() >= m.frames.size()) return keys;
  const model::Frame& f = m.frames[frame.get()];
  const analysis::KernelFrames kf = ctx.options().kernelFrames;
  if (kf != analysis::KernelFrames::Show && analysis::isKernelFrame(m, f)) {
    if (kf == analysis::KernelFrames::Group) keys.push_back(analysis::kKernelKey);
    return keys;
  }
  if (!f.module.valid()) {
    keys.push_back(FunctionKey{ModuleIdx::invalid(), f.addr_or_rva});
    return keys;
  }
  const symbols::ResolvedAddress& r =
      caller ? ctx.symbols().lookupCaller(frame) : ctx.symbols().lookup(frame);
  if (!r.resolved) {
    keys.push_back(FunctionKey{f.module, f.addr_or_rva});
    return keys;
  }
  keys.push_back(FunctionKey{f.module, r.functionStartRva});
  if (ctx.options().expandInline) {
    for (size_t i = r.inlines.size(); i-- > 0;)
      keys.push_back(FunctionKey{f.module, symbols::kInlineKeyBit | r.inlines[i].function.get()});
  }
  return keys;
}

namespace {

struct Step {
  uint32_t node;
  uint32_t matched;  // path keys matched so far
  bool afterKernel;  // the last emitted key was the merged kernel frame (KernelFrames::Group)
};

// Matches the keys of a frame against path[matched..]. Returns the new matched count, or nullopt on a
// mismatch.
std::optional<uint32_t> advance(const analysis::QueryContext& ctx, const std::vector<FunctionKey>& keys,
                                std::span<const FunctionKey> path, uint32_t matched, bool& afterKernel) {
  const bool group = ctx.options().kernelFrames == analysis::KernelFrames::Group;
  for (const FunctionKey& k : keys) {
    const bool kernel = k == analysis::kKernelKey;
    if (group && kernel && afterKernel) continue;  // runs of kernel frames merge into one
    if (matched >= path.size() || !(path[matched] == k)) return std::nullopt;
    ++matched;
    afterKernel = kernel;
  }
  return matched;
}

}  // namespace

std::optional<CallstackIdx> resolveNodePath(const analysis::QueryContext& ctx, const NodeChildIndex& children,
                                            CallstackIdx start, std::span<const FunctionKey> path) {
  const model::TraceModel& m = ctx.model();
  const uint32_t first = start.valid() ? start.get() : 0;
  if (path.empty()) return CallstackIdx(first);
  std::vector<Step> stack;
  stack.push_back({first, 0, false});
  while (!stack.empty()) {
    const Step s = stack.back();
    stack.pop_back();
    // A completed node is returned when it is popped, which keeps the depth-first order (lowest index first).
    if (s.matched == path.size()) return CallstackIdx(s.node);
    const std::span<const uint32_t> kids = children.children(s.node);
    for (size_t i = kids.size(); i-- > 0;) {  // reversed: the lowest node index is popped first
      const uint32_t child = kids[i];
      const FrameIdx frame = m.nodes[child].frame;
      bool afterKernel = s.afterKernel;
      if (const auto viaCaller = advance(ctx, frameKeys(ctx, frame, true), path, s.matched, afterKernel)) {
        stack.push_back({child, *viaCaller, afterKernel});
        continue;
      }
      // The node may be the leaf of samples (exact address) and complete the path only that way.
      bool afterKernelExact = s.afterKernel;
      const auto viaExact = advance(ctx, frameKeys(ctx, frame, false), path, s.matched, afterKernelExact);
      if (viaExact && *viaExact == path.size()) stack.push_back({child, *viaExact, afterKernelExact});
    }
  }
  return std::nullopt;
}

}  // namespace tachyon::viewer::callgraph
