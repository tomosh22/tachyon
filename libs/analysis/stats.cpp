#include "analysis/stats.h"

#include "analysis/attribution.h"
#include "analysis/detail/instance_enum.h"
#include "analysis/detail/query_support.h"
#include "analysis/query_cache.h"

#include <algorithm>
#include <cmath>

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

// Cache key of a query that depends on the time range and the thread selection only.
QueryKey timeThreadKey(const ResolvedFilter& f, QueryKind kind, uint64_t arg) {
  QueryKey k;
  k.kind = kind;
  k.arg = arg;
  k.filter.t0 = f.canonical.t0;
  k.filter.t1 = f.canonical.t1;
  k.filter.threads = f.canonical.threads;
  return k;
}

// Durations (ticks) of the enumerated instances: the same list the instance queries return.
std::vector<uint64_t> durationsOf(const std::vector<Instance>& list) {
  std::vector<uint64_t> d;
  d.reserve(list.size());
  for (const Instance& i : list) d.push_back(i.end - i.begin);
  return d;
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
        auto en = enumerateScopeInstances(ctx, *rf, scopeId, cancel);
        if (!en) return en.error();
        std::vector<uint64_t> d = durationsOf(en->list);
        Stats s = makeStats(d);
        s.unterminated = en->unterminated;
        s.orphanEnds = saturate32(en->orphanEnds);
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
        auto en = enumerateFunctionInstances(ctx, *rf, key, cancel);
        if (!en) return en.error();
        std::vector<uint64_t> d = durationsOf(en->list);
        return makeStats(d);
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
          if ((++n & 0xFFFFu) == 0 && cancel.requested()) return cancelledError();
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
