#include "report/report_builder.h"

#include "analysis/instances.h"
#include "analysis/query.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string_view>

namespace tachyon::report {

namespace {

using analysis::AggregationOptions;
using analysis::Filter;
using analysis::FunctionKey;
using analysis::QueryContext;

// Result<T> error -> Result<U> error.
template <typename T>
Error errorOf(const Result<T>& r) {
  return r.error();
}

std::string platformName(uint32_t platform) {
  if (platform == trace::kPlatformWindows) return "windows";
  if (platform == trace::kPlatformLinux) return "linux";
  return "unknown";
}

std::string hexAddress(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string threadName(const model::TraceModel& m, ThreadIdx t) {
  return t.valid() && t.get() < m.threads.size() ? m.threads[t.get()].name : std::string();
}

uint32_t threadTid(const model::TraceModel& m, ThreadIdx t) {
  return t.valid() && t.get() < m.threads.size() ? m.threads[t.get()].tid : 0;
}

TraceSummary buildTraceSummary(const model::TraceModel& m) {
  TraceSummary s;
  const uint64_t hz = m.header.clock_frequency_hz;
  const uint64_t span = m.header.end_ts > m.header.start_ts ? m.header.end_ts - m.header.start_ts : 0;
  s.durationMs = ticksToMs(span, hz);
  s.platform = platformName(m.header.platform);
  s.clockFrequencyHz = hz;
  s.samplingHz = m.captureInfo.sampling_hz;
  s.eventsLost = m.captureInfo.events_lost;
  s.partial = m.isPartial;
  s.threadCount = m.threads.size();
  for (const model::SampleColumns& c : m.samplesByThread) s.sampleCount += c.ts.size();
  s.hasInstrumentation = !m.scopeEvents.empty() || (m.header.flags & trace::kFlagHasInstrumentation) != 0;
  if (!m.captureInfo.raw_json.empty()) {
    const nlohmann::json doc = nlohmann::json::parse(m.captureInfo.raw_json, nullptr, false);
    if (doc.is_object()) {
      if (const auto it = doc.find("warnings"); it != doc.end() && it->is_array()) {
        for (const auto& w : *it) {
          if (!w.is_string()) continue;
          if (s.warnings.size() >= kMaxWarnings) break;
          s.warnings.push_back(w.get<std::string>());
        }
      }
      if (const auto it = doc.find("throttle"); it != doc.end() && it->is_object()) {
        if (const auto e = it->find("events"); e != it->end() && e->is_number() && e->get<double>() >= 0)
          s.throttleEvents = static_cast<uint64_t>(std::llround(e->get<double>()));
      }
    }
  }
  return s;
}

std::string seriesLabel(const std::string& name, const std::string& thread) {
  return thread.empty() ? name : name + "@" + thread;
}

Result<void> buildFrames(const QueryContext& ctx, const ReportOptions& options, Report& out) {
  const model::TraceModel& m = ctx.model();
  const uint64_t hz = m.header.clock_frequency_hz;
  std::vector<analysis::FrameSeries> all = analysis::frameSeries(ctx);
  std::vector<analysis::FrameSeries> selected;
  for (size_t i = 0; i < all.size(); ++i) {
    if (options.frameSeries && std::string(m.strings.get(all[i].name)) != *options.frameSeries) continue;
    selected.push_back(all[i]);
  }
  if (options.frameSeries && selected.empty())
    return makeError(ErrorCode::InvalidArgument,
                     "the trace has no frame series named '" + *options.frameSeries + "'");
  out.frameSeriesTotal = selected.size();
  if (selected.size() > kMaxFrameSeries) selected.resize(kMaxFrameSeries);
  for (size_t si = 0; si < selected.size(); ++si) {
    const analysis::FrameSeries& s = selected[si];
    FrameSeriesRow row;
    row.name = std::string(m.strings.get(s.name));
    if (s.thread.valid()) {
      row.threadTid = threadTid(m, s.thread);
      row.threadName = threadName(m, s.thread);
      if (row.threadName.empty()) row.threadName = "tid" + std::to_string(*row.threadTid);
    }
    row.label = seriesLabel(row.name, row.threadName);
    row.isDefault = si == 0;
    row.markCount = s.markCount;
    analysis::FrameOptions fo;
    fo.series = s;
    fo.topFunctions = 0;
    auto frames = analysis::framesShared(ctx, Filter{}, fo);
    if (!frames.ok()) return errorOf(frames);
    const auto& fs = **frames;
    row.frameCount = fs.size();
    if (!fs.empty()) {
      std::vector<uint64_t> d;
      d.reserve(fs.size());
      uint64_t sum = 0, maxTicks = 0, slowestStart = 0;
      for (const analysis::FrameSummary& f : fs) {
        d.push_back(f.duration);
        sum += f.duration;
        if (f.duration > maxTicks) {
          maxTicks = f.duration;
          row.slowestFrameNumber = f.number;
          slowestStart = f.start;
        }
        if (ticksToMs(f.duration, hz) > options.frameBudgetMs) ++row.overBudget;
      }
      (void)slowestStart;
      std::sort(d.begin(), d.end());
      row.p50Ms = roundMs(ticksToMs(lowerQuantile(d, 50, 100), hz));
      row.p95Ms = roundMs(ticksToMs(lowerQuantile(d, 95, 100), hz));
      row.p99Ms = roundMs(ticksToMs(lowerQuantile(d, 99, 100), hz));
      row.maxMs = roundMs(ticksToMs(d.back(), hz));
      row.meanMs = roundMs(ticksToMs(sum, hz) / static_cast<double>(d.size()));
    }
    out.frames.push_back(std::move(row));
  }
  return {};
}

Result<void> buildThreads(const QueryContext& ctx, Report& out) {
  const model::TraceModel& m = ctx.model();
  const uint64_t hz = m.header.clock_frequency_hz;
  struct Item {
    ThreadRow row;
    uint64_t runningTicks;
  };
  std::vector<Item> items;
  for (uint32_t i = 0; i < m.threads.size(); ++i) {
    auto t = analysis::threadStateTotalsShared(ctx, Filter{}, ThreadIdx(i));
    if (!t.ok()) return errorOf(t);
    const analysis::ThreadStateTotals& tt = **t;
    const uint64_t samples = i < m.samplesByThread.size() ? m.samplesByThread[i].ts.size() : 0;
    if (tt.runningTicks + tt.readyTicks + tt.waitingTicks == 0 && samples == 0) continue;
    Item it;
    it.row.tid = m.threads[i].tid;
    it.row.name = m.threads[i].name;
    it.row.runningMs = roundMs(ticksToMs(tt.runningTicks, hz));
    it.row.readyMs = roundMs(ticksToMs(tt.readyTicks, hz));
    it.row.waitingMs = roundMs(ticksToMs(tt.waitingTicks, hz));
    it.row.unknownMs = roundMs(ticksToMs(tt.unknownTicks, hz));
    it.row.sampleCount = samples;
    it.runningTicks = tt.runningTicks;
    items.push_back(std::move(it));
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.runningTicks != b.runningTicks) return a.runningTicks > b.runningTicks;
    return a.row.tid < b.row.tid;
  });
  if (items.size() > kMaxThreads) items.resize(kMaxThreads);
  for (Item& it : items) out.threads.push_back(std::move(it.row));
  return {};
}

// Splits functionKeyName() into module / name / kind.
FunctionReportRow describeKey(const QueryContext& ctx, FunctionKey key) {
  FunctionReportRow row;
  const std::string full = analysis::functionKeyName(ctx, key);
  constexpr std::string_view kScopePrefix = "scope:";
  if (key.isScope() || full.rfind(kScopePrefix, 0) == 0) {
    row.isScope = true;
    row.name = full.rfind(kScopePrefix, 0) == 0 ? full.substr(kScopePrefix.size()) : full;
    return row;
  }
  const size_t bang = full.find('!');
  if (bang == std::string::npos) {
    row.name = full;
  } else {
    row.module = full.substr(0, bang);
    row.name = full.substr(bang + 1);
  }
  return row;
}

Result<void> buildFunctions(const QueryContext& ctx, const ReportOptions& options, Report& out) {
  const uint64_t hz = ctx.model().header.clock_frequency_hz;
  auto rowsR = analysis::bottomUpFunctionsShared(ctx, Filter{});
  if (!rowsR.ok()) return errorOf(rowsR);
  const std::vector<analysis::FunctionRow>& rows = **rowsR;  // exclusive desc, inclusive desc, key
  uint64_t total = 0;
  for (const analysis::FunctionRow& r : rows) total += r.exclusiveTicks;
  out.functionCount = rows.size();
  out.totalExclusiveMs = roundMs(ticksToMs(total, hz));
  if (options.topN == 0 || rows.empty() || total == 0) return {};

  // Rows tied with the N-th one compete on their names, so those are named before the final cut.
  const size_t n = std::min<size_t>(options.topN, rows.size());
  const uint64_t cutoff = rows[n - 1].exclusiveTicks;
  std::vector<std::pair<const analysis::FunctionRow*, FunctionReportRow>> cand;
  for (const analysis::FunctionRow& r : rows) {
    if (r.exclusiveTicks == 0 || r.exclusiveTicks < cutoff) break;
    FunctionReportRow row = describeKey(ctx, r.key);
    row.exclusiveMs = roundMs(ticksToMs(r.exclusiveTicks, hz));
    row.inclusiveMs = roundMs(ticksToMs(r.inclusiveTicks, hz));
    row.exclusivePct = roundPct(100.0 * static_cast<double>(r.exclusiveTicks) / static_cast<double>(total));
    row.samples = r.instances;
    cand.emplace_back(&r, std::move(row));
  }
  std::stable_sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
    if (a.first->exclusiveTicks != b.first->exclusiveTicks)
      return a.first->exclusiveTicks > b.first->exclusiveTicks;
    if (a.second.name != b.second.name) return a.second.name < b.second.name;
    return a.second.module < b.second.module;
  });
  if (cand.size() > n) cand.resize(n);
  for (auto& c : cand) out.functions.push_back(std::move(c.second));
  return {};
}

Result<void> buildScopes(const QueryContext& ctx, const ReportOptions& options, Report& out) {
  const model::TraceModel& m = ctx.model();
  const uint64_t hz = m.header.clock_frequency_hz;
  std::set<uint32_t> ids;
  for (const trace::ScopeEventRec& packed : m.scopeEvents) {
    const trace::ScopeEventRec e = packed;  // packed record: copy before using its members
    if (e.kind == 0 && e.id.valid()) ids.insert(e.id.get());
  }
  struct Item {
    ScopeReportRow row;
    uint64_t totalTicks;
    StringIdx id;
  };
  std::vector<Item> items;
  for (const uint32_t raw : ids) {
    const StringIdx id(raw);
    auto sr = analysis::scopeStatsShared(ctx, Filter{}, id);
    if (!sr.ok()) return errorOf(sr);
    const analysis::Stats& st = **sr;
    if (st.count == 0) continue;
    Item it;
    it.id = id;
    it.totalTicks = st.totalTicks;
    it.row.name = std::string(m.strings.get(id));
    it.row.count = st.count;
    it.row.minMs = roundMs(ticksToMs(static_cast<uint64_t>(st.min), hz));
    it.row.medianMs = roundMs(ticksToMs(static_cast<uint64_t>(st.median), hz));
    it.row.maxMs = roundMs(ticksToMs(static_cast<uint64_t>(st.max), hz));
    it.row.totalMs = roundMs(ticksToMs(st.totalTicks, hz));
    it.row.meanMs = roundMs(ticksToMs(st.totalTicks, hz) / static_cast<double>(st.count));
    it.row.unterminated = st.unterminated;
    it.row.orphanEnds = st.orphanEnds;
    items.push_back(std::move(it));
  }
  out.scopeCount = items.size();
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.totalTicks != b.totalTicks) return a.totalTicks > b.totalTicks;
    return a.row.name < b.row.name;
  });
  if (items.size() > options.topN) items.resize(options.topN);
  for (Item& it : items) {
    // Stats carries no p95: take it from the instance list, which is exactly the stats' enumeration.
    auto inst = analysis::scopeInstances(ctx, Filter{}, it.id);
    if (!inst.ok()) return errorOf(inst);
    std::vector<uint64_t> d;
    d.reserve((*inst)->size());
    for (const analysis::Instance& i : **inst) d.push_back(i.end - i.begin);
    std::sort(d.begin(), d.end());
    if (!d.empty()) it.row.p95Ms = roundMs(ticksToMs(lowerQuantile(d, 95, 100), hz));
    out.scopes.push_back(std::move(it.row));
  }
  return {};
}

Result<void> buildLocks(const QueryContext& ctx, const ReportOptions& options, Report& out) {
  const model::TraceModel& m = ctx.model();
  const uint64_t hz = m.header.clock_frequency_hz;
  auto lr = analysis::lockSummaryShared(ctx, Filter{});
  if (!lr.ok()) return errorOf(lr);
  const analysis::LockSummary& ls = **lr;
  out.lockCount = ls.locks.size();
  out.unparsedLockWaits = ls.unparsedWaits;
  struct Item {
    LockReportRow row;
    uint64_t totalTicks;
  };
  std::vector<Item> items;
  for (const analysis::LockRow& l : ls.locks) {
    Item it;
    it.totalTicks = l.totalWaitTicks;
    it.row.name = l.name.valid() ? std::string(m.strings.get(l.name)) : std::string();
    it.row.address = hexAddress(l.address);
    it.row.contentions = l.contentionCount;
    it.row.totalWaitMs = roundMs(ticksToMs(l.totalWaitTicks, hz));
    it.row.maxWaitMs = roundMs(ticksToMs(l.maxWaitTicks, hz));
    it.row.unterminated = l.unterminated;
    for (const analysis::LockWaiter& w : l.topThreads) {
      LockWaiterRow wr;
      wr.tid = threadTid(m, w.thread);
      wr.name = threadName(m, w.thread);
      wr.waits = w.waits;
      wr.waitMs = roundMs(ticksToMs(w.waitTicks, hz));
      it.row.topThreads.push_back(std::move(wr));
    }
    items.push_back(std::move(it));
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.totalTicks != b.totalTicks) return a.totalTicks > b.totalTicks;
    if (a.row.name != b.row.name) return a.row.name < b.row.name;
    return a.row.address < b.row.address;
  });
  if (items.size() > options.topN) items.resize(options.topN);
  for (Item& it : items) out.locks.push_back(std::move(it.row));
  return {};
}

}  // namespace

double ticksToMs(uint64_t ticks, uint64_t clockFrequencyHz) {
  if (clockFrequencyHz == 0) return 0;
  return static_cast<double>(ticks) * 1000.0 / static_cast<double>(clockFrequencyHz);
}

double roundMs(double ms) {
  return std::round(ms * 1e6) / 1e6;
}

double roundPct(double pct) {
  return std::round(pct * 1e4) / 1e4;
}

uint64_t lowerQuantile(const std::vector<uint64_t>& sorted, uint64_t num, uint64_t den) {
  if (sorted.empty()) return 0;
  return sorted[static_cast<size_t>((static_cast<uint64_t>(sorted.size() - 1) * num) / den)];
}

Result<Report> buildReport(const model::TraceModel& model, const symbols::SymbolService& symbols,
                           const ReportOptions& options) {
  Report r;
  r.trace = buildTraceSummary(model);
  r.settings.topN = options.topN;
  r.settings.frameBudgetMs = options.frameBudgetMs;
  r.settings.blendScopes = options.blendScopes && !model.scopeEvents.empty();
  r.settings.frameSeriesFilter = options.frameSeries;
  AggregationOptions ao;
  ao.blendScopes = r.settings.blendScopes;
  const QueryContext ctx(model, symbols, ao);
  if (Result<void> s = buildFrames(ctx, options, r); !s.ok()) return s.error();
  if (Result<void> s = buildThreads(ctx, r); !s.ok()) return s.error();
  if (Result<void> s = buildFunctions(ctx, options, r); !s.ok()) return s.error();
  if (Result<void> s = buildScopes(ctx, options, r); !s.ok()) return s.error();
  if (Result<void> s = buildLocks(ctx, options, r); !s.ok()) return s.error();
  return r;
}

}  // namespace tachyon::report
