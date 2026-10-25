#include "analysis/fibers.h"

#include "analysis/query_cache.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace tachyon::analysis {

namespace {

// Execution context: a fiber or the thread context of one thread.
struct Ctx {
  FiberLane lane;
  struct Open {
    uint32_t laneScope;  // index into lane.scopes
    uint32_t thread;     // thread of the Begin
    uint32_t threadIdx;  // index into PairedScopes::byThread[thread]
  };
  std::vector<Open> open;
  bool running = false;
  uint32_t thread = kInvalidIdx;  // where a running fiber runs
  uint64_t since = 0;
};

void computePieces(FiberLane& lane) {
  const std::vector<FiberRun>& runs = lane.runs;
  for (uint32_t s = 0; s < lane.scopes.size(); ++s) {
    const ScopeInstance& si = lane.scopes[s];
    if (si.end <= si.begin) continue;
    size_t r =
        static_cast<size_t>(std::partition_point(runs.begin(), runs.end(),
                                                 [&](const FiberRun& run) { return run.end <= si.begin; }) -
                            runs.begin());
    for (; r < runs.size() && runs[r].begin < si.end; ++r) {
      const uint64_t b = std::max(runs[r].begin, si.begin), e = std::min(runs[r].end, si.end);
      if (e > b) lane.pieces.push_back(ScopePiece{s, runs[r].thread, b, e});
    }
  }
  std::sort(lane.pieces.begin(), lane.pieces.end(), [&](const ScopePiece& a, const ScopePiece& b) {
    if (a.begin != b.begin) return a.begin < b.begin;
    const uint32_t da = lane.scopes[a.scope].depth, db = lane.scopes[b.scope].depth;
    if (da != db) return da < db;
    return a.scope < b.scope;
  });
}

}  // namespace

size_t PairedFibers::heapBytes() const {
  size_t bytes = sizeof(PairedFibers) + scopes.heapBytes() + vectorBytes(lanes);
  for (const FiberLane& l : lanes)
    bytes += vectorBytes(l.runs) + vectorBytes(l.scopes) + vectorBytes(l.pieces);
  return bytes;
}

Result<PairedFibers> pairScopesByFiber(const model::TraceModel& m, Cancel cancel) {
  PairedFibers out;
  if (!hasFiberEvents(m)) {
    auto plain = pairScopes(m, cancel);
    if (!plain) return plain.error();
    out.scopes = std::move(*plain);
    return out;
  }
  out.fiberAware = true;
  const size_t threadCount = m.threads.size();
  PairedScopes& ps = out.scopes;
  ps.byThread.resize(threadCount);
  ps.orphanEndTs.resize(threadCount);

  std::unordered_map<uint64_t, uint32_t> fiberCtx;  // fiber id -> index into ctxs
  std::vector<Ctx> ctxs;
  ctxs.reserve(threadCount + 16);
  // Thread contexts first: ctxs[t] is the thread context of ThreadIdx t, running from the start.
  for (uint32_t t = 0; t < threadCount; ++t) {
    Ctx c;
    c.lane.fiberId = 0;
    c.lane.thread = ThreadIdx(t);
    c.running = true;
    c.thread = t;
    c.since = 0;
    ctxs.push_back(std::move(c));
  }
  std::vector<uint64_t> cur(threadCount, 0);  // fiber running on each thread (0 = thread context)

  auto fiberFor = [&](uint64_t id) -> Ctx& {
    auto [it, inserted] = fiberCtx.try_emplace(id, static_cast<uint32_t>(ctxs.size()));
    if (inserted) {
      Ctx c;
      c.lane.fiberId = id;
      ctxs.push_back(std::move(c));
    }
    return ctxs[it->second];
  };
  auto closeRun = [](Ctx& c, uint64_t ts) {
    if (ts > c.since) c.lane.runs.push_back(FiberRun{ThreadIdx(c.thread), c.since, ts});
    c.running = false;
  };
  // F stops running at `ts`; its thread returns to the thread context.
  auto stopFiber = [&](Ctx& f, uint64_t ts) {
    if (!f.running) return;
    closeRun(f, ts);
    const uint32_t t = f.thread;
    if (t < threadCount && cur[t] == f.lane.fiberId) {
      cur[t] = 0;
      Ctx& tc = ctxs[t];
      tc.running = true;
      tc.since = ts;
    }
  };
  auto startFiber = [&](Ctx& f, uint32_t t, uint64_t ts) {
    if (f.running && f.thread == t && cur[t] == f.lane.fiberId) return;  // double switch_end
    stopFiber(f, ts);                                                    // running elsewhere
    if (cur[t] != 0) stopFiber(ctxs[fiberCtx.at(cur[t])], ts);           // implicit switch away
    Ctx& tc = ctxs[t];
    if (tc.running) closeRun(tc, ts);
    cur[t] = f.lane.fiberId;
    f.running = true;
    f.thread = t;
    f.since = ts;
  };

  const uint64_t traceEnd = m.header.end_ts;
  auto endOpen = [&](Ctx& c, uint64_t ts) {
    for (const Ctx::Open& o : c.open) {
      ScopeInstance& a = c.lane.scopes[o.laneScope];
      a.end = std::max(ts, a.begin);  // flagged unterminated already
      ps.byThread[o.thread][o.threadIdx].end = a.end;
      ++ps.unterminatedCount;
    }
    c.open.clear();
  };

  TidLookup tids(m);
  const std::vector<trace::ScopeEventRec>& events = m.scopeEvents;
  const std::vector<model::FiberSwitch>& fev = m.fiberSwitches;
  size_t si = 0, fi = 0;
  size_t polled = 0;
  while (si < events.size() || fi < fev.size()) {
    if ((++polled & 0xFFFFu) == 0 && cancel.requested())
      return makeError(ErrorCode::Cancelled, "query cancelled");
    trace::ScopeEventRec e;
    bool takeFiber;
    if (si < events.size()) {
      std::memcpy(&e, &events[si], sizeof e);
      // At equal timestamps: the End events that open the scope stream's tie group, then the fiber events,
      // then the rest of the scope events (see fibers.h).
      takeFiber =
          fi < fev.size() && (fev[fi].ts < e.ts || (fev[fi].ts == e.ts && e.kind != trace::kScopeEnd));
    } else {
      takeFiber = true;
    }
    if (takeFiber) {
      const model::FiberSwitch& f = fev[fi++];
      const uint32_t t = f.thread.valid() && f.thread.get() < threadCount ? f.thread.get() : kInvalidIdx;
      if (f.kind == trace::kFiberRegister) {
        if (f.fiber_id != 0) fiberFor(f.fiber_id);
      } else if (f.kind == trace::kFiberSwitchEnd) {
        if (f.fiber_id != 0 && t != kInvalidIdx) startFiber(fiberFor(f.fiber_id), t, f.ts);
      } else if (f.kind == trace::kFiberSwitchBegin) {
        if (f.fiber_id != 0) stopFiber(fiberFor(f.fiber_id), f.ts);
      } else if (f.kind == trace::kFiberUnregister) {
        if (f.fiber_id != 0) {
          Ctx& c = fiberFor(f.fiber_id);
          stopFiber(c, f.ts);
          endOpen(c, f.ts);
        }
      }
      continue;
    }
    ++si;
    const uint32_t t = tids.find(e.tid);
    if (t == kInvalidIdx) {
      ++ps.droppedEvents;
      continue;
    }
    Ctx& c = cur[t] != 0 ? ctxs[fiberCtx.at(cur[t])] : ctxs[t];
    if (e.kind == trace::kScopeBegin) {
      ScopeInstance inst;
      inst.id = e.id;
      inst.begin = inst.end = e.ts;
      inst.depth = static_cast<uint32_t>(c.open.size());
      inst.callerAddr = e.caller_addr;
      inst.data = e.data;
      inst.color = e.color_rgba;
      inst.unterminated = true;
      auto& list = ps.byThread[t];
      c.open.push_back(
          Ctx::Open{static_cast<uint32_t>(c.lane.scopes.size()), t, static_cast<uint32_t>(list.size())});
      c.lane.scopes.push_back(inst);
      list.push_back(inst);
    } else if (e.kind == trace::kScopeEnd) {
      if (c.open.empty()) {
        ps.orphanEndTs[t].push_back(e.ts);
        continue;
      }
      const Ctx::Open o = c.open.back();
      c.open.pop_back();
      ScopeInstance& a = c.lane.scopes[o.laneScope];
      ScopeInstance& b = ps.byThread[o.thread][o.threadIdx];
      a.end = b.end = std::max(e.ts, a.begin);
      a.unterminated = b.unterminated = false;
    }
  }
  for (Ctx& c : ctxs) {
    if (c.running) {
      // A fiber still running ends with the trace; the thread context is open-ended.
      const uint64_t end = c.lane.fiberId == 0 ? UINT64_MAX : std::max(traceEnd, c.since);
      closeRun(c, end);
    }
    endOpen(c, traceEnd);
  }

  // Lanes: every fiber; thread contexts that hold scopes.
  for (Ctx& c : ctxs) {
    if (c.lane.fiberId == 0 && c.lane.scopes.empty()) continue;
    computePieces(c.lane);
    out.lanes.push_back(std::move(c.lane));
  }
  std::sort(out.lanes.begin(), out.lanes.end(), [](const FiberLane& a, const FiberLane& b) {
    if (a.fiberId != b.fiberId) return a.fiberId < b.fiberId;
    return a.thread < b.thread;
  });
  return out;
}

Result<std::shared_ptr<const PairedFibers>> pairedFibersFor(const QueryContext& ctx, Cancel cancel) {
  QueryKey k;
  k.kind = QueryKind::ScopePairs;
  k.arg = 1;
  QueryCache& cache = ctx.cache();
  if (auto hit = cache.find<PairedFibers>(k)) return hit;
  auto r = pairScopesByFiber(ctx.model(), cancel);
  if (!r) return r.error();
  const size_t bytes = r->heapBytes() + 2 * kAllocOverhead + 32;
  auto value = std::make_shared<const PairedFibers>(std::move(*r));
  cache.insert(k, value, bytes);
  return value;
}

Result<std::shared_ptr<const PairedScopes>> pairedScopesFor(const QueryContext& ctx, Cancel cancel) {
  if (ctx.options().groupByFiber && hasFiberEvents(ctx.model())) {
    auto pf = pairedFibersFor(ctx, cancel);
    if (!pf) return pf.error();
    return std::shared_ptr<const PairedScopes>(*pf, &(*pf)->scopes);  // aliasing: keeps the pairing alive
  }
  QueryKey k;
  k.kind = QueryKind::ScopePairs;
  QueryCache& cache = ctx.cache();
  if (auto hit = cache.find<PairedScopes>(k)) return hit;
  auto r = pairScopes(ctx.model(), cancel);
  if (!r) return r.error();
  const size_t bytes = r->heapBytes() + 2 * kAllocOverhead + 32;
  auto value = std::make_shared<const PairedScopes>(std::move(*r));
  cache.insert(k, value, bytes);
  return value;
}

}  // namespace tachyon::analysis
