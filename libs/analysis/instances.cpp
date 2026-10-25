#include "analysis/instances.h"

#include "analysis/attribution.h"
#include "analysis/detail/instance_enum.h"
#include "analysis/detail/query_support.h"
#include "analysis/query_cache.h"
#include "analysis/scope_pairing.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <unordered_map>

namespace tachyon::analysis {

namespace detail {

namespace {

Result<std::shared_ptr<const PairedScopes>> pairedFor(const QueryContext& ctx, Cancel cancel) {
  QueryKey k;
  k.kind = QueryKind::ScopePairs;
  return cachedOrCompute<PairedScopes>(
      ctx, k, [&]() { return pairScopes(ctx.model(), cancel); },
      [](const PairedScopes& p) { return p.heapBytes() + 2 * kAllocOverhead + 32; });
}

}  // namespace

Result<EnumeratedInstances> enumerateScopeInstances(const QueryContext& ctx, const ResolvedFilter& rf,
                                                    StringIdx scopeId, Cancel cancel) {
  if (ctx.model().statesByThread.size() != ctx.model().threads.size())
    return makeError(ErrorCode::InvalidArgument, "thread states are not built (call buildThreadStates)");
  auto paired = pairedFor(ctx, cancel);
  if (!paired) return paired.error();
  const PairedScopes& ps = **paired;
  EnumeratedInstances out;
  if (rf.t0 >= rf.t1) return out;
  for (ThreadIdx t : rf.threads) {
    if (cancel.requested()) return cancelledError();
    const std::vector<ScopeInstance>& list = ps.byThread[t.get()];
    auto it = std::lower_bound(list.begin(), list.end(), rf.t0,
                               [](const ScopeInstance& s, uint64_t ts) { return s.begin < ts; });
    size_t scanned = 0;
    for (; it != list.end() && it->begin < rf.t1; ++it) {
      if ((++scanned & 0xFFFFu) == 0 && cancel.requested()) return cancelledError();
      if (it->id != scopeId) continue;
      out.list.push_back(Instance{t, it->begin, it->end});
      if (it->unterminated) ++out.unterminated;
    }
    const std::vector<uint64_t>& orph = ps.orphanEndTs[t.get()];
    out.orphanEnds += static_cast<uint64_t>(std::lower_bound(orph.begin(), orph.end(), rf.t1) -
                                            std::lower_bound(orph.begin(), orph.end(), rf.t0));
  }
  return out;
}

Result<EnumeratedInstances> enumerateFunctionInstances(const QueryContext& ctx, const ResolvedFilter& rf,
                                                       FunctionKey key, Cancel cancel) {
  EnumeratedInstances out;
  if (rf.t0 >= rf.t1) return out;
  const model::TraceModel& m = ctx.model();
  KeyTable table;
  StackFlattener flat(ctx, table);
  flat.setCancel(cancel);
  const uint32_t target = table.intern(key);
  const uint32_t required = rf.function ? table.intern(*rf.function) : kUnset;
  const uint64_t maxGap = 2 * m.sampleIntervalTicks;
  // Depth (position of the outermost occurrence) of the function in the stack of a callstack node, or
  // -1 when the sample is filtered out or the stack lacks the function. Per (node, kernel-leaf flag).
  std::unordered_map<uint64_t, int32_t> memo;
  std::vector<uint32_t> ids;
  CancelMeter meter{cancel};
  bool aborted = false;

  for (ThreadIdx thread : rf.threads) {
    bool open = false;
    int32_t runDepth = 0;
    uint64_t firstTs = 0, lastTs = 0, lastWeight = 0;
    auto close = [&]() {
      if (open) out.list.push_back(Instance{thread, firstTs, lastTs + lastWeight});
      open = false;
    };
    const bool done = forEachWeightedSample(
        m, thread, rf.t0, rf.t1, cancel, [&](uint64_t ts, CallstackIdx cs, uint64_t weight, uint16_t flags) {
          if (weight == 0 || aborted) return;  // not Running: skipped, breaks nothing
          const bool kernelLeaf = (flags & trace::kSampleFlagKernelLeaf) != 0;
          const uint64_t mk = (uint64_t{cs.get()} << 1) | (kernelLeaf ? 1u : 0u);
          auto [it, fresh] = memo.try_emplace(mk, -1);
          if (fresh) {
            const bool kept = flat.flatten(cs, rf.subtree, kernelLeaf, ids, required);
            if (flat.aborted()) {
              aborted = true;
              return;
            }
            meter.add(flat.lastCost() + ids.size());
            if (kept) {
              const auto f = std::find(ids.begin(), ids.end(), target);
              if (f != ids.end()) it->second = static_cast<int32_t>(f - ids.begin());
            }
            if (meter.due()) {
              aborted = true;
              return;
            }
          }
          const int32_t depth = it->second;
          if (depth < 0) {
            close();
            return;
          }
          if (open && depth == runDepth && ts - lastTs <= maxGap) {
            lastTs = ts;
            lastWeight = weight;
            return;
          }
          close();
          open = true;
          runDepth = depth;
          firstTs = lastTs = ts;
          lastWeight = weight;
        });
    if (!done || aborted) return cancelledError();
    close();
  }
  return out;
}

}  // namespace detail

namespace {

using detail::EnumeratedInstances;

size_t instancesBytes(const std::vector<Instance>& v) {
  return sizeof(v) + 2 * kAllocOverhead + 32 + vectorBytes(v);
}

// Sorted by begin, then thread, then end: a total order, so the result does not depend on the thread order of
// the enumeration.
void sortInstances(std::vector<Instance>& v) {
  std::sort(v.begin(), v.end(), [](const Instance& a, const Instance& b) {
    if (a.begin != b.begin) return a.begin < b.begin;
    if (a.thread != b.thread) return a.thread < b.thread;
    return a.end < b.end;
  });
}

}  // namespace

Result<std::shared_ptr<const std::vector<Instance>>> scopeInstances(const QueryContext& ctx,
                                                                    const Filter& filter, StringIdx scopeId,
                                                                    Cancel cancel) {
  using namespace detail;
  if (!scopeId.valid()) return makeError(ErrorCode::InvalidArgument, "scopeInstances: invalid scope id");
  auto rf = resolveFilterBasic(ctx, filter);
  if (!rf) return rf.error();
  if (ctx.model().statesByThread.size() != ctx.model().threads.size())
    return makeError(ErrorCode::InvalidArgument, "thread states are not built (call buildThreadStates)");
  // Scopes depend on the time range and the thread selection only (like the scope statistics).
  QueryKey key;
  key.kind = QueryKind::Instances;
  key.function = FunctionKey::scope(scopeId);
  key.filter.t0 = rf->canonical.t0;
  key.filter.t1 = rf->canonical.t1;
  key.filter.threads = rf->canonical.threads;
  return cachedOrCompute<std::vector<Instance>>(
      ctx, key,
      [&]() -> Result<std::vector<Instance>> {
        auto en = enumerateScopeInstances(ctx, *rf, scopeId, cancel);
        if (!en) return en.error();
        sortInstances(en->list);
        return std::move(en->list);
      },
      instancesBytes);
}

Result<std::shared_ptr<const std::vector<Instance>>> functionInstances(const QueryContext& ctx,
                                                                       const Filter& filter, FunctionKey key,
                                                                       Cancel cancel) {
  using namespace detail;
  if (key.isScope()) return scopeInstances(ctx, filter, StringIdx(static_cast<uint32_t>(key.value)), cancel);
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  return cachedOrCompute<std::vector<Instance>>(
      ctx, resultKey(*rf, QueryKind::Instances, key),
      [&]() -> Result<std::vector<Instance>> {
        auto en = enumerateFunctionInstances(ctx, *rf, key, cancel);
        if (!en) return en.error();
        sortInstances(en->list);
        return std::move(en->list);
      },
      instancesBytes);
}

}  // namespace tachyon::analysis
