// instructionsForFunction (WP-34).
#include "analysis/lines.h"

#include <algorithm>
#include <unordered_map>

namespace tachyon::analysis {

namespace {

using namespace detail;

size_t instructionsBytes(const std::vector<InstructionAgg>& v) {
  return sizeof(std::vector<InstructionAgg>) + 2 * kAllocOverhead + 32 + vectorBytes(v);
}

}  // namespace

Result<std::shared_ptr<const std::vector<InstructionAgg>>> instructionsForFunctionShared(
    const QueryContext& ctx, const Filter& filter, FunctionKey key, Cancel cancel) {
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  return cachedOrCompute<std::vector<InstructionAgg>>(
      ctx, resultKey(*rf, QueryKind::Instructions, key),
      [&]() -> Result<std::vector<InstructionAgg>> {
        FunctionMatcher matcher(ctx, key, rf->subtree);
        if (!matcher.matchable() || rf->t0 >= rf->t1) return std::vector<InstructionAgg>{};
        auto w = nodeWeightsFor(ctx, *rf, cancel);
        if (!w) return w.error();

        StackPass stackPass(ctx, *rf);
        CancelMeter meter{cancel};
        std::unordered_map<uint64_t, uint64_t> byRva;
        for (const NodeWeight& nw : **w) {
          if (meter.due()) return cancelledError();
          if (nw.ticks == 0) continue;
          const bool kept = stackPass.pass(nw.node, nw.kernelLeaf, cancel);
          if (stackPass.aborted()) return cancelledError();
          meter.add(stackPass.lastCost());
          if (!kept) continue;
          const std::optional<FunctionHit> hit = matcher.match(nw.node, nw.kernelLeaf);
          if (!hit) continue;
          byRva[matcher.addressOf(*hit)] += nw.ticks;
        }
        std::vector<InstructionAgg> out;
        out.reserve(byRva.size());
        for (const auto& [rva, ticks] : byRva) out.push_back(InstructionAgg{rva, ticks});
        std::sort(out.begin(), out.end(),
                  [](const InstructionAgg& a, const InstructionAgg& b) { return a.rva < b.rva; });
        return out;
      },
      instructionsBytes);
}

Result<std::vector<InstructionAgg>> instructionsForFunction(const QueryContext& ctx, const Filter& filter,
                                                            FunctionKey key, Cancel cancel) {
  auto r = instructionsForFunctionShared(ctx, filter, key, cancel);
  if (!r) return r.error();
  return std::vector<InstructionAgg>(**r);
}

}  // namespace tachyon::analysis
