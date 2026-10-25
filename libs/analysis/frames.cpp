#include "analysis/frames.h"

#include "analysis/attribution.h"
#include "analysis/detail/parallel.h"
#include "analysis/detail/query_support.h"
#include "analysis/query_cache.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <atomic>
#include <optional>
#include <thread>
#include <unordered_map>

namespace tachyon::analysis {

using namespace detail;

namespace {

uint64_t seriesKey(ThreadIdx thread, StringIdx name) {
  return (uint64_t{thread.get()} << 32) | name.get();
}

bool slowerFirst(const FrameSpan& a, const FrameSpan& b) {
  const uint64_t da = a.end - a.start, db = b.end - b.start;
  if (da != db) return da > db;
  if (a.start != b.start) return a.start < b.start;
  return a.index < b.index;
}

size_t summariesBytes(const std::vector<FrameSummary>& v) {
  size_t bytes = sizeof(v) + 2 * kAllocOverhead + 32 + vectorBytes(v);
  for (const FrameSummary& f : v) bytes += vectorBytes(f.threads) + vectorBytes(f.topFunctions);
  return bytes;
}

// Resolves the series a query works on: the explicit one (which must exist) or the default. nullopt-like
// result (markCount 0) when the trace has no marks and none was requested.
Result<FrameSeries> pickSeries(const QueryContext& ctx, const FrameOptions& options) {
  const std::vector<FrameSeries> all = frameSeries(ctx);
  if (options.series) {
    for (const FrameSeries& s : all)
      if (s.sameSeries(*options.series)) return s;
    return makeError(ErrorCode::InvalidArgument, "FrameOptions::series has no frame marks");
  }
  if (all.empty()) return FrameSeries{};
  return all.front();
}

struct FrameJob {
  const QueryContext& ctx;
  const ResolvedFilter& rf;
  uint32_t topN;
  Cancel cancel;
};

// Fills threads and topFunctions of `out` for the span.
class FrameDetailer {
 public:
  explicit FrameDetailer(const FrameJob& job)
      : m_job(job), m_model(job.ctx.model()), m_flat(job.ctx, m_table) {
    m_flat.setCancel(job.cancel);
    if (job.rf.function) m_required = m_table.intern(*job.rf.function);
  }

  // False if cancelled.
  bool detail(const FrameSpan& span, FrameSummary& out) {
    m_touched.clear();
    const auto& threads = m_job.rf.threads;
    if (m_cursors.empty()) m_cursors.assign(threads.size(), Cursor{});
    const bool rewind = span.start < m_lastStart;  // frames of a worker normally ascend: reuse the cursors
    m_lastStart = span.start;
    const uint64_t interval = m_model.sampleIntervalTicks;
    for (size_t k = 0; k < threads.size(); ++k) {
      const ThreadIdx thread = threads[k];
      Cursor& cur = m_cursors[k];
      FrameThreadTime tt;
      tt.thread = thread;
      const auto& ivs = m_model.statesByThread[thread.get()];
      const model::SampleColumns& cols = m_model.samplesByThread[thread.get()];
      const uint64_t* ts = cols.ts.data();
      const size_t n = cols.ts.size();
      // First interval ending after the frame start, first sample at or after it.
      if (rewind || cur.state == kNone) {
        cur.state = static_cast<size_t>(
            std::partition_point(ivs.begin(), ivs.end(),
                                 [&](const model::ThreadStateInterval& iv) { return iv.end <= span.start; }) -
            ivs.begin());
        cur.sample = static_cast<size_t>(std::lower_bound(ts, ts + n, span.start) - ts);
      } else {
        size_t steps = 0;
        while (cur.state < ivs.size() && ivs[cur.state].end <= span.start) {
          if (++steps > 16) {  // a long jump: binary search the rest
            cur.state = static_cast<size_t>(
                std::partition_point(
                    ivs.begin() + static_cast<std::ptrdiff_t>(cur.state), ivs.end(),
                    [&](const model::ThreadStateInterval& iv) { return iv.end <= span.start; }) -
                ivs.begin());
            break;
          }
          ++cur.state;
        }
        steps = 0;
        while (cur.sample < n && ts[cur.sample] < span.start) {
          if (++steps > 16) {
            cur.sample = static_cast<size_t>(std::lower_bound(ts + cur.sample, ts + n, span.start) - ts);
            break;
          }
          ++cur.sample;
        }
      }
      for (size_t it = cur.state; it < ivs.size() && ivs[it].start < span.end; ++it) {
        if (ivs[it].state != model::ThreadState::Running) continue;
        const uint64_t a = std::max(ivs[it].start, span.start), b = std::min(ivs[it].end, span.end);
        if (b > a) tt.runningTicks += b - a;
      }
      // Samples of the frame with the rule 1 weight (same computation as forEachWeightedSample).
      size_t si = cur.state;
      for (size_t i = cur.sample; i < n && ts[i] < span.end; ++i) {
        if ((++m_polled & 0xFFFFu) == 0 && m_job.cancel.requested()) return false;
        const uint64_t t = ts[i];
        while (si < ivs.size() && ivs[si].end <= t) ++si;
        uint64_t weight = 0;
        if (si < ivs.size() && ivs[si].state == model::ThreadState::Running && ivs[si].start <= t)
          weight = std::min(interval, ivs[si].end - t);
        const uint16_t flags = i < cols.flags.size() ? cols.flags[i] : uint16_t{0};
        const int64_t leaf = leafOf(cols.callstack[i], (flags & trace::kSampleFlagKernelLeaf) != 0);
        if (m_aborted) return false;
        if (leaf == kFiltered) continue;
        tt.sampleTicks += weight;
        if (leaf == kNoFunction) continue;
        const size_t id = static_cast<size_t>(leaf);
        if (id >= m_ticks.size()) {
          m_ticks.resize(m_table.size(), 0);
          m_samples.resize(m_table.size(), 0);
        }
        if (m_samples[id] == 0) m_touched.push_back(static_cast<uint32_t>(id));
        m_ticks[id] += weight;
        ++m_samples[id];
      }
      if (tt.runningTicks != 0 || tt.sampleTicks != 0) out.threads.push_back(tt);
    }
    // Top functions by exclusive ticks.
    std::vector<FrameFunctionTime> rows;
    rows.reserve(m_touched.size());
    for (uint32_t id : m_touched) {
      if (m_ticks[id] != 0) rows.push_back(FrameFunctionTime{m_table.key(id), m_ticks[id], m_samples[id]});
      m_ticks[id] = 0;
      m_samples[id] = 0;
    }
    auto better = [](const FrameFunctionTime& a, const FrameFunctionTime& b) {
      if (a.exclusiveTicks != b.exclusiveTicks) return a.exclusiveTicks > b.exclusiveTicks;
      if (a.samples != b.samples) return a.samples > b.samples;
      return a.key < b.key;
    };
    const size_t n = std::min<size_t>(m_job.topN, rows.size());
    std::partial_sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(n), rows.end(), better);
    rows.resize(n);
    rows.shrink_to_fit();
    out.topFunctions = std::move(rows);
    return true;
  }

 private:
  static constexpr int64_t kFiltered = -1;    // the sample does not pass the filter
  static constexpr int64_t kNoFunction = -2;  // passes, but has no frame to attribute to (empty stack)

  // Leaf function id (KeyTable id) of the sample's stack as the query sees it, memoized per node.
  int64_t leafOf(CallstackIdx cs, bool kernelLeaf) {
    const uint64_t mk = (uint64_t{cs.get()} << 1) | (kernelLeaf ? 1u : 0u);
    auto [it, fresh] = m_memo.try_emplace(mk, kFiltered);
    if (!fresh) return it->second;
    const bool kept = m_flat.flatten(cs, m_job.rf.subtree, kernelLeaf, m_ids, m_required);
    if (m_flat.aborted()) {
      m_aborted = true;
      m_memo.erase(it);
      return kFiltered;
    }
    m_meter.add(m_flat.lastCost() + m_ids.size());
    if (m_meter.due()) m_aborted = true;
    if (kept) it->second = m_ids.empty() ? kNoFunction : static_cast<int64_t>(m_ids.back());
    return it->second;
  }

  static constexpr size_t kNone = ~size_t{0};
  struct Cursor {
    size_t state = kNone;  // first state interval that ends after the last frame start
    size_t sample = 0;     // first sample at or after the last frame start
  };

  const FrameJob& m_job;
  const model::TraceModel& m_model;
  std::vector<Cursor> m_cursors;  // per selected thread
  uint64_t m_lastStart = 0;
  size_t m_polled = 0;
  KeyTable m_table;
  StackFlattener m_flat;
  uint32_t m_required = kUnset;
  std::unordered_map<uint64_t, int64_t> m_memo;
  std::vector<uint32_t> m_ids;
  std::vector<uint64_t> m_ticks;
  std::vector<uint32_t> m_samples;
  std::vector<uint32_t> m_touched;
  CancelMeter m_meter{m_job.cancel};
  bool m_aborted = false;
};

Result<std::vector<FrameSummary>> computeFrames(const QueryContext& ctx, const ResolvedFilter& rf,
                                                const FrameSeries& series, uint32_t topN,
                                                const uint32_t* slowest, Cancel cancel) {
  std::vector<FrameSummary> out;
  if (rf.t0 >= rf.t1 || (slowest != nullptr && *slowest == 0)) return out;
  std::vector<FrameSpan> spans = frameSpans(ctx.model(), series);
  spans.erase(std::remove_if(spans.begin(), spans.end(),
                             [&](const FrameSpan& s) { return !(s.start < rf.t1 && s.end > rf.t0); }),
              spans.end());
  if (slowest != nullptr) {
    if (spans.size() > *slowest) {
      std::nth_element(spans.begin(), spans.begin() + *slowest, spans.end(), slowerFirst);
      spans.resize(*slowest);
    }
    std::sort(spans.begin(), spans.end(), slowerFirst);
  }
  const FrameJob job{ctx, rf, topN, cancel};
  const size_t n = spans.size();
  out.resize(n);
  // Frames are independent: workers pull chunks of frames from a shared counter and each owns a detailer.
  constexpr size_t kChunk = 64;
  unsigned workers =
      ctx.options().threads != 0 ? ctx.options().threads : std::max(1u, std::thread::hardware_concurrency());
  workers = static_cast<unsigned>(std::min<size_t>(workers, (n + kChunk * 8 - 1) / (kChunk * 8)));
  if (QueryExecutor* ex = ctx.executor(); ex != nullptr) workers = std::min(workers, ex->threadCount() + 1);
  workers = std::max(workers, 1u);
  std::atomic<size_t> next{0};
  std::atomic<bool> stopped{false};
  auto work = [&](unsigned) {
    std::optional<FrameDetailer> detailer;
    for (;;) {
      if (stopped.load(std::memory_order_relaxed)) return;
      const size_t begin = next.fetch_add(kChunk, std::memory_order_relaxed);
      if (begin >= n) return;
      if (cancel.requested()) {
        stopped.store(true, std::memory_order_relaxed);
        return;
      }
      if (!detailer) detailer.emplace(job);
      const size_t end = std::min(begin + kChunk, n);
      for (size_t i = begin; i < end; ++i) {
        const FrameSpan& s = spans[i];
        FrameSummary& f = out[i];
        f.index = s.index;
        f.number = s.number;
        f.start = s.start;
        f.end = s.end;
        f.duration = s.end - s.start;
        f.markThread = series.thread;
        f.seriesName = series.name;
        if (!detailer->detail(s, f)) {
          stopped.store(true, std::memory_order_relaxed);
          return;
        }
      }
    }
  };
  runSlots(ctx.executor(), workers, work);
  if (stopped.load() || cancel.requested()) return cancelledError();
  return out;
}

QueryKey framesKey(const ResolvedFilter& rf, QueryKind kind, const FrameSeries& series, uint32_t topN,
                   uint32_t n) {
  QueryKey k = resultKey(rf, kind, FunctionKey{ModuleIdx(series.thread.get()), series.name.get()});
  k.arg = (uint64_t{topN} << 32) | n;
  return k;
}

}  // namespace

std::vector<FrameSpan> frameSpans(const model::TraceModel& m, const FrameSeries& series) {
  struct Mark {
    uint64_t ts, number;
  };
  std::vector<Mark> marks;
  for (const model::FrameMark& fm : m.frameMarks)
    if (fm.thread == series.thread && fm.name == series.name) marks.push_back(Mark{fm.ts, fm.frame_number});
  std::stable_sort(marks.begin(), marks.end(), [](const Mark& a, const Mark& b) { return a.ts < b.ts; });
  std::vector<FrameSpan> out;
  if (marks.size() < 2) return out;
  out.reserve(marks.size() - 1);
  for (size_t i = 0; i + 1 < marks.size(); ++i)
    out.push_back(FrameSpan{i, marks[i].number, marks[i].ts, marks[i + 1].ts});
  return out;
}

std::vector<FrameSeries> frameSeries(const QueryContext& ctx) {
  struct Acc {
    FrameSeries series;
    uint64_t firstTs = 0;
  };
  std::unordered_map<uint64_t, Acc> acc;
  for (const model::FrameMark& fm : ctx.model().frameMarks) {
    auto [it, fresh] = acc.try_emplace(seriesKey(fm.thread, fm.name));
    Acc& a = it->second;
    if (fresh) {
      a.series.thread = fm.thread;
      a.series.name = fm.name;
      a.firstTs = fm.ts;
    }
    ++a.series.markCount;
    a.firstTs = std::min(a.firstTs, fm.ts);
  }
  std::vector<Acc> sorted;
  sorted.reserve(acc.size());
  for (auto& [k, a] : acc) sorted.push_back(a);
  std::sort(sorted.begin(), sorted.end(), [](const Acc& a, const Acc& b) {
    if (a.series.markCount != b.series.markCount) return a.series.markCount > b.series.markCount;
    if (a.firstTs != b.firstTs) return a.firstTs < b.firstTs;
    if (a.series.thread != b.series.thread) return a.series.thread < b.series.thread;
    return a.series.name < b.series.name;
  });
  std::vector<FrameSeries> out;
  out.reserve(sorted.size());
  for (const Acc& a : sorted) out.push_back(a.series);
  return out;
}

Result<std::shared_ptr<const std::vector<FrameSummary>>> framesShared(const QueryContext& ctx,
                                                                      const Filter& filter,
                                                                      FrameOptions options, Cancel cancel) {
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  auto series = pickSeries(ctx, options);
  if (!series) return series.error();
  return cachedOrCompute<std::vector<FrameSummary>>(
      ctx, framesKey(*rf, QueryKind::Frames, *series, options.topFunctions, 0),
      [&]() { return computeFrames(ctx, *rf, *series, options.topFunctions, nullptr, cancel); },
      summariesBytes);
}

Result<std::vector<FrameSummary>> frames(const QueryContext& ctx, const Filter& filter, FrameOptions options,
                                         Cancel cancel) {
  auto r = framesShared(ctx, filter, options, cancel);
  if (!r) return r.error();
  return **r;
}

Result<std::shared_ptr<const std::vector<FrameSummary>>> slowestFramesShared(const QueryContext& ctx,
                                                                             const Filter& filter, uint32_t n,
                                                                             FrameOptions options,
                                                                             Cancel cancel) {
  auto rf = resolveFilter(ctx, filter);
  if (!rf) return rf.error();
  auto series = pickSeries(ctx, options);
  if (!series) return series.error();
  return cachedOrCompute<std::vector<FrameSummary>>(
      ctx, framesKey(*rf, QueryKind::SlowestFrames, *series, options.topFunctions, n),
      [&]() { return computeFrames(ctx, *rf, *series, options.topFunctions, &n, cancel); }, summariesBytes);
}

Result<std::vector<FrameSummary>> slowestFrames(const QueryContext& ctx, const Filter& filter, uint32_t n,
                                                FrameOptions options, Cancel cancel) {
  auto r = slowestFramesShared(ctx, filter, n, options, cancel);
  if (!r) return r.error();
  return **r;
}

}  // namespace tachyon::analysis
