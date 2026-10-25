#include "analysis/stats.h"

#include "analysis/attribution.h"
#include "analysis/detail/query_support.h"
#include "analysis/fibers.h"
#include "analysis/query_cache.h"
#include "analysis/scope_blend.h"
#include "analysis/scope_pairing.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace tachyon::analysis {

using namespace detail;

size_t statsBytes(const Stats& s) {
  return sizeof(Stats) + 2 * kAllocOverhead + 32 + vectorBytes(s.histogram);
}

Stats makeStats(std::vector<uint64_t>& d) {
  Stats s;
  const size_t n = d.size();
  if (n == 0) return s;
  s.count = saturate32(n);
  uint64_t lo = UINT64_MAX, hi = 0, total = 0;
  uint32_t top = 0;
  for (uint64_t v : d) {
    lo = std::min(lo, v);
    hi = std::max(hi, v);
    total += v;
  }
  top = statsBucketOf(hi);
  s.histogram.assign(top + 1, 0);
  for (uint64_t v : d) ++s.histogram[statsBucketOf(v)];
  s.totalTicks = total;
  s.min = static_cast<double>(lo);
  s.max = static_cast<double>(hi);
  s.mean = static_cast<double>(total) / static_cast<double>(n);

  // Lower order-statistic quantiles: q is d[floor(q * (n - 1))] of the sorted durations (no interpolation).
  const double qs[3] = {0.25, 0.5, 0.75};
  size_t pos[3];
  for (int i = 0; i < 3; ++i) pos[i] = static_cast<size_t>(std::floor(qs[i] * static_cast<double>(n - 1)));
  size_t start = 0;
  for (size_t idx : pos) {      // ascending
    if (idx < start) continue;  // already in place
    std::nth_element(d.begin() + static_cast<std::ptrdiff_t>(start),
                     d.begin() + static_cast<std::ptrdiff_t>(idx), d.end());
    start = idx + 1;
  }
  s.p25 = static_cast<double>(d[pos[0]]);
  s.median = static_cast<double>(d[pos[1]]);
  s.p75 = static_cast<double>(d[pos[2]]);
  return s;
}

namespace {

Error cancelled() {
  return cancelledError();
}

// Cache key of a query that depends on the time range and the thread selection only.
QueryKey timeThreadKey(const ResolvedFilter& f, QueryKind kind, uint64_t arg) {
  QueryKey k;
  k.kind = kind;
  k.arg = arg;
  k.filter.t0 = f.canonical.t0;
  k.filter.t1 = f.canonical.t1;
  k.filter.threads = f.canonical.threads;
  k.options = f.options;
  return k;
}

}  // namespace

Result<std::shared_ptr<const Stats>> scopeStatsShared(const QueryContext& ctx, const Filter& filter,
                                                      StringIdx scopeId, Cancel cancel) {
  if (!scopeId.valid()) return makeError(ErrorCode::InvalidArgument, "scopeStats: invalid scope id");
  auto rf = resolveFilterBasic(ctx, filter);
  if (!rf) return rf.error();
  if (ctx.model().statesByThread.size() != ctx.model().threads.size())
    return makeError(ErrorCode::InvalidArgument, "thread states are not built (call buildThreadStates)");
  return cachedOrCompute<Stats>(
      ctx, timeThreadKey(*rf, QueryKind::ScopeStats, scopeId.get()),
      [&]() -> Result<Stats> {
        auto paired = pairedScopesFor(ctx, cancel);
        if (!paired) return paired.error();
        const PairedScopes& ps = **paired;
        std::vector<uint64_t> durations;
        uint32_t unterminated = 0;
        uint64_t orphans = 0;
        if (rf->t0 < rf->t1) {
          for (ThreadIdx t : rf->threads) {
            if (cancel.requested()) return cancelled();
            const std::vector<ScopeInstance>& list = ps.byThread[t.get()];
            auto it = std::lower_bound(list.begin(), list.end(), rf->t0,
                                       [](const ScopeInstance& s, uint64_t ts) { return s.begin < ts; });
            size_t scanned = 0;
            for (; it != list.end() && it->begin < rf->t1; ++it) {
              if ((++scanned & 0xFFFFu) == 0 && cancel.requested()) return cancelled();
              if (it->id != scopeId) continue;
              durations.push_back(it->end - it->begin);
              if (it->unterminated) ++unterminated;
            }
            const std::vector<uint64_t>& orph = ps.orphanEndTs[t.get()];
            orphans += static_cast<uint64_t>(std::lower_bound(orph.begin(), orph.end(), rf->t1) -
                                             std::lower_bound(orph.begin(), orph.end(), rf->t0));
          }
        }
        Stats s = makeStats(durations);
        s.unterminated = unterminated;
        s.orphanEnds = saturate32(orphans);
        return s;
      },
      statsBytes);
}

Result<Stats> scopeStats(const QueryContext& ctx, const Filter& filter, StringIdx scopeId, Cancel cancel) {
  auto r = scopeStatsShared(ctx, filter, scopeId, cancel);
  if (!r) return r.error();
  return **r;
}

Result<std::shared_ptr<const Stats>> functionStatsShared(const QueryContext& ctx, const Filter& filter,
                                                         FunctionKey key, Cancel cancel) {
  if (key.isScope())
    return scopeStatsShared(ctx, filter, StringIdx(static_cast<uint32_t>(key.value)), cancel);
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  return cachedOrCompute<Stats>(
      ctx, resultKey(*rf, QueryKind::FunctionStats, key),
      [&]() -> Result<Stats> {
        std::vector<uint64_t> durations;
        if (rf->t0 < rf->t1) {
          const model::TraceModel& m = ctx.model();
          KeyTable table;
          StackFlattener flat(ctx, table);
          flat.setCancel(cancel);
          auto blend = blendIndexIfEnabled(ctx, cancel);
          if (!blend) return blend.error();
          flat.setBlend(blend->get());
          static const std::vector<ScopeSegment> kNoSegments;
          const uint32_t target = table.intern(key);
          const uint32_t required = rf->function ? table.intern(*rf->function) : kUnset;
          const uint64_t maxGap = 2 * m.sampleIntervalTicks;
          // Depth (position of the outermost occurrence) of the function in the stack of a callstack node, or
          // -1 when the sample is filtered out or the stack lacks the function. Per (node, kernel-leaf flag).
          std::unordered_map<uint64_t, int32_t> memo;
          std::vector<uint32_t> ids;
          CancelMeter meter{cancel};
          bool aborted = false;

          for (ThreadIdx thread : rf->threads) {
            ScopeCursor cursor(flat.blending() && thread.get() < (*blend)->byThread.size()
                                   ? (*blend)->byThread[thread.get()]
                                   : kNoSegments);
            bool open = false;
            int32_t runDepth = 0;
            uint64_t firstTs = 0, lastTs = 0, lastWeight = 0;
            auto close = [&]() {
              if (open) durations.push_back(lastTs - firstTs + lastWeight);
              open = false;
            };
            const bool done = forEachWeightedSample(
                m, thread, rf->t0, rf->t1, cancel,
                [&](uint64_t ts, CallstackIdx cs, uint64_t weight, uint16_t flags) {
                  if (weight == 0 || aborted) return;  // not Running: skipped, breaks nothing
                  const bool kernelLeaf = (flags & trace::kSampleFlagKernelLeaf) != 0;
                  const uint32_t stack = flat.blending() ? cursor.stackAt(ts) : 0u;
                  const uint64_t mk =
                      (uint64_t{stack} << 33) | (uint64_t{cs.get()} << 1) | (kernelLeaf ? 1u : 0u);
                  auto [it, fresh] = memo.try_emplace(mk, -1);
                  if (fresh) {
                    const bool kept = flat.flatten(cs, rf->subtree, kernelLeaf, ids, required, stack);
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
            if (!done || aborted) return cancelled();
            close();
          }
        }
        return makeStats(durations);
      },
      statsBytes);
}

Result<Stats> functionStats(const QueryContext& ctx, const Filter& filter, FunctionKey key, Cancel cancel) {
  auto r = functionStatsShared(ctx, filter, key, cancel);
  if (!r) return r.error();
  return **r;
}

Result<std::shared_ptr<const ThreadStateTotals>> threadStateTotalsShared(const QueryContext& ctx,
                                                                         const Filter& filter,
                                                                         ThreadIdx thread, Cancel cancel) {
  const model::TraceModel& m = ctx.model();
  if (!thread.valid() || thread.get() >= m.threads.size())
    return makeError(ErrorCode::InvalidArgument, "threadStateTotals: invalid ThreadIdx");
  if (m.statesByThread.size() != m.threads.size())
    return makeError(ErrorCode::InvalidArgument, "thread states are not built (call buildThreadStates)");
  QueryKey key;
  key.kind = QueryKind::ThreadStateTotals;
  key.arg = thread.get();
  key.filter.t0 = filter.t0;
  key.filter.t1 = filter.t1;
  if (key.filter.t0 >= key.filter.t1) key.filter.t0 = key.filter.t1 = 0;
  return cachedOrCompute<ThreadStateTotals>(
      ctx, key,
      [&]() -> Result<ThreadStateTotals> {
        ThreadStateTotals t;
        const uint64_t t0 = key.filter.t0, t1 = key.filter.t1;
        if (t0 >= t1) return t;
        const auto& ivs = m.statesByThread[thread.get()];
        auto it = std::partition_point(ivs.begin(), ivs.end(),
                                       [&](const model::ThreadStateInterval& iv) { return iv.end <= t0; });
        size_t n = 0;
        for (; it != ivs.end() && it->start < t1; ++it) {
          if ((++n & 0xFFFFu) == 0 && cancel.requested()) return cancelled();
          const uint64_t a = std::max(it->start, t0), b = std::min(it->end, t1);
          if (b <= a) continue;
          switch (it->state) {
            case model::ThreadState::Running:
              t.runningTicks += b - a;
              ++t.runningIntervals;
              break;
            case model::ThreadState::Ready:
              t.readyTicks += b - a;
              ++t.readyIntervals;
              break;
            case model::ThreadState::Waiting:
              t.waitingTicks += b - a;
              ++t.waitingIntervals;
              break;
            case model::ThreadState::Unknown: t.unknownTicks += b - a; break;
          }
        }
        return t;
      },
      [](const ThreadStateTotals&) { return sizeof(ThreadStateTotals) + 2 * kAllocOverhead + 32; });
}

Result<ThreadStateTotals> threadStateTotals(const QueryContext& ctx, const Filter& filter, ThreadIdx thread,
                                            Cancel cancel) {
  auto r = threadStateTotalsShared(ctx, filter, thread, cancel);
  if (!r) return r.error();
  return **r;
}

}  // namespace tachyon::analysis
