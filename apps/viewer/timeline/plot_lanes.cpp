#include "viewer/timeline/plot_lanes.h"

#include "viewer/timeline/scope_lanes.h"
#include "viewer/timeline/timeline_colors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

namespace tachyon::viewer::timeline {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

ImU32 toIm(Color32 c) {
  return static_cast<ImU32>(c);
}

// Min / max of the finite values of value[a, b).
inline void scanValues(const double* value, size_t a, size_t b, double& mn, double& mx) {
  for (size_t i = a; i < b; ++i) {
    const double v = value[i];
    if (!std::isfinite(v)) continue;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
}

inline void scanBlocks(const std::vector<double>& mins, const std::vector<double>& maxs, size_t a, size_t b,
                       double& mn, double& mx) {
  for (size_t i = a; i < b; ++i) {
    if (mins[i] < mn) mn = mins[i];
    if (maxs[i] > mx) mx = maxs[i];
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Index

std::shared_ptr<PlotIndex> buildPlotIndex(std::shared_ptr<const model::TraceModel> model,
                                          const std::atomic<bool>* cancel,
                                          const std::function<void(float)>& progress) {
  auto idx = std::make_shared<PlotIndex>();
  idx->model = model;
  const model::TraceModel& m = *model;
  size_t total = 0;
  for (const model::PlotSeries& p : m.plots) total += std::min(p.ts.size(), p.value.size());
  size_t done = 0;
  idx->series.resize(m.plots.size());
  for (size_t k = 0; k < m.plots.size(); ++k) {
    const model::PlotSeries& p = m.plots[k];
    PlotSeriesIndex& s = idx->series[k];
    s.name = p.name;
    s.n = std::min(p.ts.size(), p.value.size());
    if (std::is_sorted(p.ts.begin(), p.ts.begin() + static_cast<ptrdiff_t>(s.n))) {
      s.ts = p.ts.data();
      s.value = p.value.data();
    } else {
      std::vector<uint32_t> order(s.n);
      std::iota(order.begin(), order.end(), 0u);
      std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return p.ts[a] < p.ts[b]; });
      s.ownedTs.resize(s.n);
      s.ownedValue.resize(s.n);
      for (size_t i = 0; i < s.n; ++i) {
        s.ownedTs[i] = p.ts[order[i]];
        s.ownedValue[i] = p.value[order[i]];
      }
      s.ts = s.ownedTs.data();
      s.value = s.ownedValue.data();
    }
    const size_t nb0 = s.n / kPlotBlock0;
    s.min0.assign(nb0, kInf);
    s.max0.assign(nb0, -kInf);
    double gmn = kInf, gmx = -kInf;
    for (size_t b = 0; b < nb0; ++b) {
      scanValues(s.value, b * kPlotBlock0, (b + 1) * kPlotBlock0, s.min0[b], s.max0[b]);
      gmn = std::min(gmn, s.min0[b]);
      gmx = std::max(gmx, s.max0[b]);
      if ((b & 0x3FFF) == 0 && cancel && cancel->load(std::memory_order_relaxed)) return nullptr;
    }
    scanValues(s.value, nb0 * kPlotBlock0, s.n, gmn, gmx);
    s.hasFinite = gmn <= gmx;
    s.minValue = s.hasFinite ? gmn : 0.0;
    s.maxValue = s.hasFinite ? gmx : 0.0;
    const size_t nb1 = nb0 / kPlotBlock1;
    s.min1.assign(nb1, kInf);
    s.max1.assign(nb1, -kInf);
    for (size_t b = 0; b < nb1; ++b) {
      scanBlocks(s.min0, s.max0, b * kPlotBlock1, (b + 1) * kPlotBlock1, s.min1[b], s.max1[b]);
    }
    idx->sampleCount += s.n;
    done += s.n;
    if (progress && total > 0) progress(static_cast<float>(done) / static_cast<float>(total));
  }
  if (!std::is_sorted(m.markers.begin(), m.markers.end(),
                      [](const model::Marker& a, const model::Marker& b) { return a.ts < b.ts; })) {
    idx->markerOrder.resize(m.markers.size());
    std::iota(idx->markerOrder.begin(), idx->markerOrder.end(), 0u);
    std::stable_sort(idx->markerOrder.begin(), idx->markerOrder.end(),
                     [&](uint32_t a, uint32_t b) { return m.markers[a].ts < m.markers[b].ts; });
  }
  if (progress) progress(1.0f);
  return idx;
}

bool plotRangeMinMax(const PlotSeriesIndex& s, size_t i0, size_t i1, double& mn, double& mx) {
  mn = kInf;
  mx = -kInf;
  i1 = std::min(i1, s.n);
  if (i0 >= i1) return false;
  if (i1 - i0 < 2 * kPlotBlock0) {
    scanValues(s.value, i0, i1, mn, mx);
    return mn <= mx;
  }
  const size_t b0 = (i0 + kPlotBlock0 - 1) / kPlotBlock0, b1 = i1 / kPlotBlock0;  // full blocks [b0, b1)
  scanValues(s.value, i0, b0 * kPlotBlock0, mn, mx);
  scanValues(s.value, b1 * kPlotBlock0, i1, mn, mx);
  if (b1 - b0 < 2 * kPlotBlock1) {
    scanBlocks(s.min0, s.max0, b0, b1, mn, mx);
  } else {
    const size_t c0 = (b0 + kPlotBlock1 - 1) / kPlotBlock1, c1 = b1 / kPlotBlock1;
    scanBlocks(s.min0, s.max0, b0, c0 * kPlotBlock1, mn, mx);
    scanBlocks(s.min0, s.max0, c1 * kPlotBlock1, b1, mn, mx);
    scanBlocks(s.min1, s.max1, c0, c1, mn, mx);
  }
  return mn <= mx;
}

namespace {

// First index in [lo, n) whose ts is >= target, found by galloping from an interpolated guess: O(1) for
// evenly spaced samples (the usual plot), O(log distance) otherwise. `slope` is samples per tick.
size_t lowerBoundNear(const uint64_t* ts, size_t n, size_t lo, uint64_t target, double slope) {
  if (lo >= n) return n;
  if (ts[lo] >= target) return lo;  // ts[lo] < target from here on
  const double guess = static_cast<double>(lo) + static_cast<double>(target - ts[lo]) * slope;
  size_t gi = guess >= static_cast<double>(n - 1) ? n - 1 : static_cast<size_t>(guess);
  gi = std::max(gi, lo + 1);
  if (gi >= n) gi = n - 1;
  size_t a, b;  // ts[a] < target <= ts[b] (b == n: no such sample); the answer is in (a, b]
  if (ts[gi] >= target) {
    b = gi;
    size_t step = 1;
    a = gi > lo + step ? gi - step : lo;
    while (a > lo && ts[a] >= target) {
      b = a;
      step *= 2;
      a = gi > lo + step ? gi - step : lo;
    }
  } else {
    a = gi;
    size_t step = 1;
    b = a + step < n ? a + step : n;
    while (b < n && ts[b] < target) {
      a = b;
      step *= 2;
      b = a + step < n ? a + step : n;
    }
  }
  return static_cast<size_t>(std::lower_bound(ts + a + 1, ts + b, target) - ts);
}

}  // namespace

void queryPlotColumns(const PlotSeriesIndex& s, const PlotArea& area, float columnPixels, PlotWindow& out) {
  out.columns.clear();
  out.hasBefore = out.hasAfter = out.any = false;
  out.samples = 0;
  out.minValue = out.maxValue = 0;
  const size_t n = s.n;
  const double scale = area.view.span > 0.0 ? static_cast<double>(area.width) / area.view.span : 0.0;
  if (n == 0 || !(scale > 0.0)) return;
  const double base = static_cast<double>(area.limits.traceStart);
  const double cw = std::max(columnPixels, 1.0f);
  const size_t cols = static_cast<size_t>(std::ceil(static_cast<double>(area.width) / cw));
  const double tA = base + area.view.start, tB = base + area.view.end();
  const auto boundTick = [&](size_t c) -> uint64_t {
    const double t = c >= cols ? tB : tA + static_cast<double>(c) * cw / scale;
    return t > 0.0 ? static_cast<uint64_t>(std::ceil(t)) : 0;
  };
  const auto xOfTick = [&](uint64_t t) {
    return static_cast<float>(static_cast<double>(area.x0) +
                              ((static_cast<double>(t) - base) - area.view.start) * scale);
  };
  std::vector<size_t> bounds(cols + 1);
  const double slope = n > 1 && s.ts[n - 1] > s.ts[0]
                           ? static_cast<double>(n - 1) / static_cast<double>(s.ts[n - 1] - s.ts[0])
                           : 0.0;
  size_t lo = static_cast<size_t>(std::lower_bound(s.ts, s.ts + n, boundTick(0)) - s.ts);
  bounds[0] = lo;
  for (size_t c = 1; c <= cols; ++c) {
    lo = lowerBoundNear(s.ts, n, lo, boundTick(c), slope);
    bounds[c] = lo;
  }
  out.samples = bounds[cols] - bounds[0];
  double mn = kInf, mx = -kInf;
  const auto includeNeighbour = [&](size_t i, bool& has, double& value, float& x) {
    has = true;
    value = s.value[i];
    x = xOfTick(s.ts[i]);
    if (std::isfinite(value)) {
      mn = std::min(mn, value);
      mx = std::max(mx, value);
    }
  };
  if (bounds[0] > 0) includeNeighbour(bounds[0] - 1, out.hasBefore, out.beforeValue, out.beforeX);
  if (bounds[cols] < n) includeNeighbour(bounds[cols], out.hasAfter, out.afterValue, out.afterX);
  for (size_t c = 0; c < cols; ++c) {
    const size_t i0 = bounds[c], i1 = bounds[c + 1];
    if (i0 >= i1) continue;
    PlotColumn col;
    if (!plotRangeMinMax(s, i0, i1, col.min, col.max)) continue;  // only non-finite values
    col.x = static_cast<float>(static_cast<double>(area.x0) + (static_cast<double>(c) + 0.5) * cw);
    col.count = static_cast<uint32_t>(std::min<size_t>(i1 - i0, UINT32_MAX));
    col.first = std::isfinite(s.value[i0]) ? s.value[i0] : col.min;
    col.last = std::isfinite(s.value[i1 - 1]) ? s.value[i1 - 1] : col.max;
    mn = std::min(mn, col.min);
    mx = std::max(mx, col.max);
    out.columns.push_back(col);
  }
  out.any = mn <= mx;
  if (out.any) {
    out.minValue = mn;
    out.maxValue = mx;
  }
}

size_t nearestSample(const PlotSeriesIndex& s, uint64_t tick) {
  if (s.n == 0) return SIZE_MAX;
  const size_t i = static_cast<size_t>(std::lower_bound(s.ts, s.ts + s.n, tick) - s.ts);
  if (i == 0) return 0;
  if (i >= s.n) return s.n - 1;
  return (tick - s.ts[i - 1] <= s.ts[i] - tick) ? i - 1 : i;
}

// ---------------------------------------------------------------------------------------------------------
// Markers

bool layoutMarkers(const MarkerView& markers, const PlotArea& area, size_t maxItems,
                   std::vector<MarkerItem>& out) {
  out.clear();
  const size_t n = markers.size();
  const double scale = area.view.span > 0.0 ? static_cast<double>(area.width) / area.view.span : 0.0;
  if (n == 0 || !(scale > 0.0)) return true;
  const double base = static_cast<double>(area.limits.traceStart);
  const double tA = base + area.view.start, tB = base + area.view.end();
  const auto tsAt = [&](size_t k) { return markers.at(k).ts; };
  // First marker at or after the view start.
  const uint64_t tAFloor = tA > 0.0 ? static_cast<uint64_t>(std::floor(tA)) : 0;
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (tsAt(mid) < tAFloor) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  size_t pos = lo;
  while (pos < n && static_cast<double>(tsAt(pos)) < tB) {
    if (out.size() >= maxItems) return false;
    const double x =
        static_cast<double>(area.x0) + ((static_cast<double>(tsAt(pos)) - base) - area.view.start) * scale;
    const double px = std::floor(x);
    const double edgeTick = tA + (px + 1.0 - area.x0) / scale;
    const uint64_t edge = edgeTick > 0.0 ? static_cast<uint64_t>(std::ceil(edgeTick)) : 0;
    size_t a = pos + 1, b = n;
    while (a < b) {
      const size_t mid = (a + b) / 2;
      if (tsAt(mid) < edge) {
        a = mid + 1;
      } else {
        b = mid;
      }
    }
    MarkerItem it;
    it.x = static_cast<float>(px + 0.5);
    it.first = static_cast<uint32_t>(pos);
    it.count = static_cast<uint32_t>(a - pos);
    out.push_back(it);
    pos = a;
  }
  return true;
}

std::optional<size_t> markerItemAt(const std::vector<MarkerItem>& items, float x, float tolerancePx) {
  if (items.empty()) return std::nullopt;
  const auto it =
      std::lower_bound(items.begin(), items.end(), x, [](const MarkerItem& m, float v) { return m.x < v; });
  std::optional<size_t> best;
  float bestDist = tolerancePx + 0.001f;
  for (auto c = (it == items.begin() ? it : it - 1); c != items.end() && c <= it; ++c) {
    const float d = std::fabs(c->x - x);
    if (d <= bestDist) {
      bestDist = d;
      best = static_cast<size_t>(c - items.begin());
    }
  }
  return best;
}

std::vector<std::string> markerTooltipLines(const model::TraceModel& model, const MarkerView& markers,
                                            const MarkerItem& item, double clockHz) {
  std::vector<std::string> lines;
  if (item.first >= markers.size()) return lines;
  const model::Marker& m = markers.at(item.first);
  char buf[256];
  const std::string_view name = model.strings.get(m.name);
  if (item.count > 1) {
    std::snprintf(buf, sizeof buf, "%u markers (zoom in to separate them)", item.count);
    lines.push_back(buf);
    std::snprintf(buf, sizeof buf, "first: %.*s", static_cast<int>(name.size()), name.data());
    lines.push_back(buf);
  } else {
    lines.push_back("Marker: " + std::string(name));
    const std::string_view data = model.strings.get(m.data);
    if (!data.empty()) lines.push_back("data: " + std::string(data));
  }
  std::snprintf(buf, sizeof buf, "time: +%s",
                formatTimeNs(static_cast<double>(m.ts - std::min(m.ts, model.header.start_ts)) * 1e9 /
                                 std::max(clockHz, 1.0),
                             1.0)
                    .c_str());
  lines.push_back(buf);
  if (m.thread.valid() && m.thread.get() < model.threads.size()) {
    const model::Thread& th = model.threads[m.thread.get()];
    std::snprintf(buf, sizeof buf, "thread: %s (tid %u)", th.name.empty() ? "(unnamed)" : th.name.c_str(),
                  th.tid);
    lines.push_back(buf);
  } else {
    lines.push_back("global marker");
  }
  return lines;
}

std::vector<std::string> plotTooltipLines(const model::TraceModel& model, const PlotSeriesIndex& s,
                                          size_t sample, const PlotColumn* column, double clockHz) {
  std::vector<std::string> lines;
  if (sample >= s.n) return lines;
  char buf[256];
  const std::string_view name = model.strings.get(s.name);
  std::snprintf(buf, sizeof buf, "%.*s = %.6g", static_cast<int>(name.size()), name.data(), s.value[sample]);
  lines.push_back(buf);
  std::snprintf(
      buf, sizeof buf, "time: +%s",
      formatTimeNs(static_cast<double>(s.ts[sample] - std::min(s.ts[sample], model.header.start_ts)) * 1e9 /
                       std::max(clockHz, 1.0),
                   1.0)
          .c_str());
  lines.push_back(buf);
  if (column && column->count > 1) {
    std::snprintf(buf, sizeof buf, "%u samples under the cursor: %.6g .. %.6g", column->count, column->min,
                  column->max);
    lines.push_back(buf);
  }
  return lines;
}

// ---------------------------------------------------------------------------------------------------------
// Section layout

GlobalLayout layoutGlobalLanes(bool hasMarkers, size_t plotCount, bool plotsCollapsed,
                               const GlobalGeometry& geom) {
  GlobalLayout out;
  float y = 0;
  if (hasMarkers) {
    out.lanes.push_back({GlobalLane::Kind::Markers, 0, y, geom.markerLaneHeight});
    y += geom.markerLaneHeight + geom.gap;
  }
  if (plotCount > 0) {
    out.lanes.push_back({GlobalLane::Kind::PlotsHeader, 0, y, geom.plotHeaderHeight});
    y += geom.plotHeaderHeight + geom.gap;
    if (!plotsCollapsed) {
      for (size_t i = 0; i < plotCount; ++i) {
        out.lanes.push_back({GlobalLane::Kind::Plot, static_cast<uint32_t>(i), y, geom.plotLaneHeight});
        y += geom.plotLaneHeight + geom.gap;
      }
    }
  }
  out.height = out.lanes.empty() ? 0.0f : y + geom.sectionGap;
  return out;
}

const GlobalLane* globalLaneAt(const GlobalLayout& layout, float y) {
  for (const GlobalLane& l : layout.lanes) {
    if (y >= l.y0 && y < l.y0 + l.height) return &l;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------------------------------------
// The view

struct PlotLanesView::Shared {
  explicit Shared(std::shared_ptr<const model::TraceModel> m) : model(std::move(m)) {}
  const std::shared_ptr<const model::TraceModel> model;
  std::shared_ptr<PlotIndex> owner;  // written by the build before `index` is published
  std::atomic<const PlotIndex*> index{nullptr};
};

void PlotLanesView::reset() {
  if (m_jobs && m_job != 0) m_jobs->cancel(m_job);
  m_jobs = nullptr;
  m_job = 0;
  m_shared.reset();
  m_injected.reset();
  m_hasMarkers = false;
  m_plotCount = 0;
  m_plotsCollapsed = false;
  m_layoutValid = false;
  m_hover = {};
  m_stats = {};
  m_drawnLanes.clear();
  m_markerItems.clear();
  m_ranges.clear();
  m_windows.clear();
}

void PlotLanesView::start(const PanelContext& context, std::shared_ptr<const model::TraceModel> model,
                          bool buildInline) {
  reset();
  m_hasMarkers = !model->markers.empty();
  m_plotCount = model->plots.size();
  m_ranges.assign(m_plotCount, {0.0, 0.0});
  if (!hasContent()) return;
  m_shared = std::make_shared<Shared>(std::move(model));
  if (buildInline || !context.jobs) {
    m_shared->owner = buildPlotIndex(m_shared->model, nullptr);
    m_shared->index.store(m_shared->owner.get(), std::memory_order_release);
    return;
  }
  m_jobs = context.jobs;
  const PanelContext redraw = context;  // only the redraw hook is used from the completion
  std::shared_ptr<Shared> shared = m_shared;
  m_job = context.jobs->submit(
      "Indexing plots",
      [shared](JobContext& job) {
        auto idx = buildPlotIndex(shared->model, job.cancelFlag(), [&job](float f) { job.setProgress(f); });
        if (!idx) return;
        shared->owner = idx;
        shared->index.store(idx.get(), std::memory_order_release);
      },
      [redraw](JobQueue::JobId, JobOutcome) { redraw.requestRedraw(); });
}

void PlotLanesView::inject(std::shared_ptr<const PlotIndex> index) {
  m_injected = std::move(index);
  m_windows.clear();
  if (m_injected) {
    m_hasMarkers = !m_injected->model->markers.empty();
    m_plotCount = m_injected->model->plots.size();
    m_ranges.assign(m_plotCount, {0.0, 0.0});
  }
}

const PlotIndex* PlotLanesView::index() const {
  if (m_injected) return m_injected.get();
  return m_shared ? m_shared->index.load(std::memory_order_acquire) : nullptr;
}

bool PlotLanesView::ready() const {
  return index() != nullptr;
}

const GlobalLayout& PlotLanesView::layout(const GlobalGeometry& geom) {
  if (!m_visible) {
    static const GlobalLayout kEmpty;
    return kEmpty;
  }
  if (!m_layoutValid || m_layoutPlots != m_plotCount || m_layoutCollapsed != m_plotsCollapsed ||
      m_layoutMarkers != m_hasMarkers || m_layoutGeom.markerLaneHeight != geom.markerLaneHeight ||
      m_layoutGeom.plotLaneHeight != geom.plotLaneHeight ||
      m_layoutGeom.plotHeaderHeight != geom.plotHeaderHeight) {
    m_layout = layoutGlobalLanes(m_hasMarkers, m_plotCount, m_plotsCollapsed, geom);
    m_layoutGeom = geom;
    m_layoutPlots = m_plotCount;
    m_layoutCollapsed = m_plotsCollapsed;
    m_layoutMarkers = m_hasMarkers;
    m_layoutValid = true;
  }
  return m_layout;
}

std::optional<std::pair<double, double>> PlotLanesView::lastRange(uint32_t plot) const {
  if (plot >= m_ranges.size()) return std::nullopt;
  return m_ranges[plot];
}

void PlotLanesView::draw(ImDrawList* dl, const model::TraceModel& model, const PlotArea& area, float top,
                         float gutterX0, const GlobalGeometry& geom, const ImVec2* mouse) {
  m_stats = {};
  m_hover = {};
  m_drawnLanes.clear();
  if (!m_visible || !hasContent()) return;
  const GlobalLayout& lay = layout(geom);
  const float x1 = area.x0 + area.width;
  for (const GlobalLane& lane : lay.lanes) {
    const float y0 = top + lane.y0, y1 = y0 + lane.height;
    m_drawnLanes.push_back({lane, y0, y1});
    const bool over = mouse && mouse->y >= y0 && mouse->y < y1;
    if (lane.kind == GlobalLane::Kind::PlotsHeader) {
      dl->AddRectFilled(ImVec2(gutterX0, y0), ImVec2(x1, y1), IM_COL32(38, 38, 46, 255));
      char text[64];
      std::snprintf(text, sizeof text, "%s Plots (%zu)", m_plotsCollapsed ? ">" : "v", m_plotCount);
      dl->AddText(ImVec2(gutterX0 + 4.0f, y0), ImGui::GetColorU32(ImGuiCol_Text), text);
      if (over) {
        m_hover.kind = PlotHover::Kind::PlotsHeader;
        m_hover.lines = {m_plotsCollapsed ? "Click: show the plot lanes" : "Click: collapse the plot lanes"};
      }
    } else if (lane.kind == GlobalLane::Kind::Markers) {
      drawMarkers(dl, model, area, y0, y1, gutterX0, over ? mouse : nullptr);
    } else {
      drawPlot(dl, model, area, lane.index, y0, y1, gutterX0, over ? mouse : nullptr);
    }
  }
}

void PlotLanesView::drawMarkers(ImDrawList* dl, const model::TraceModel& model, const PlotArea& area,
                                float y0, float y1, float gutterX0, const ImVec2* mouse) {
  const float x0 = area.x0, x1 = area.x0 + area.width;
  dl->AddRectFilled(ImVec2(gutterX0, y0), ImVec2(x0, y1), IM_COL32(32, 32, 38, 255));
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(24, 24, 29, 255));
  char label[48];
  std::snprintf(label, sizeof label, "Markers (%zu)", model.markers.size());
  dl->AddText(ImVec2(gutterX0 + 4.0f, y0 + 1.0f), ImGui::GetColorU32(ImGuiCol_Text), label);
  const PlotIndex* idx = index();
  if (!idx) {
    dl->AddText(ImVec2(x0 + 4.0f, y0 + 1.0f), IM_COL32(150, 150, 160, 255), "indexing...");
    return;
  }
  const MarkerView view{&model.markers, &idx->markerOrder};
  layoutMarkers(view, area, static_cast<size_t>(area.width) + 8, m_markerItems);
  m_stats.markerTicks = static_cast<uint32_t>(m_markerItems.size());
  dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
  ImFont* font = ImGui::GetFont();
  const float fontSize = ImGui::GetFontSize();
  const float charPx = std::max(ImGui::CalcTextSize("0").x, 1.0f);
  uint32_t labels = 0;
  for (size_t i = 0; i < m_markerItems.size(); ++i) {
    const MarkerItem& it = m_markerItems[i];
    const model::Marker& mk = view.at(it.first);
    Color32 c = mk.color == 0xFFFFFFFFu || mk.color == 0
                    ? makeColor(240, 190, 70)
                    : makeColor(static_cast<uint8_t>(mk.color >> 24), static_cast<uint8_t>(mk.color >> 16),
                                static_cast<uint8_t>(mk.color >> 8));
    if (it.count > 1) c = scaleBrightness(c, 1.15f);
    dl->AddLine(ImVec2(it.x, y0 + 1.0f), ImVec2(it.x, y1 - 1.0f), toIm(c), it.count > 1 ? 2.0f : 1.0f);
    dl->AddTriangleFilled(ImVec2(it.x - 3.0f, y0 + 1.0f), ImVec2(it.x + 3.0f, y0 + 1.0f),
                          ImVec2(it.x, y0 + 6.0f), toIm(c));
    ++m_stats.markersDrawn;
    const float next = i + 1 < m_markerItems.size() ? m_markerItems[i + 1].x : x1;
    const float room = next - it.x - 6.0f;
    if (it.count == 1 && room > charPx * 4.0f && labels < 200) {
      const std::string text = clipLabel(model.strings.get(mk.name), room, charPx);
      if (!text.empty()) {
        ++labels;
        dl->AddText(font, fontSize, ImVec2(it.x + 4.0f, y0 + 4.0f), toIm(c), text.c_str());
      }
    }
  }
  dl->PopClipRect();
  if (mouse && mouse->x >= x0 && mouse->x < x1) {
    if (const auto hit = markerItemAt(m_markerItems, mouse->x, 4.0f)) {
      m_hover.kind = PlotHover::Kind::Marker;
      m_hover.index = static_cast<uint32_t>(*hit);
      m_hover.lines = markerTooltipLines(model, view, m_markerItems[*hit], area.clockHz);
    }
  }
}

void PlotLanesView::drawPlot(ImDrawList* dl, const model::TraceModel& model, const PlotArea& area,
                             uint32_t plot, float y0, float y1, float gutterX0, const ImVec2* mouse) {
  const float x0 = area.x0, x1 = area.x0 + area.width;
  const std::string_view nameView =
      plot < model.plots.size() ? model.strings.get(model.plots[plot].name) : "";
  const std::string name(nameView);
  dl->AddRectFilled(ImVec2(gutterX0, y0), ImVec2(x0, y1), IM_COL32(32, 32, 38, 255));
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(22, 22, 27, 255));
  {
    const ImVec4 clip(gutterX0, y0, x0 - 4.0f, y1);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(gutterX0 + 12.0f, y0 + 1.0f),
                ImGui::GetColorU32(ImGuiCol_Text), name.c_str(), nullptr, 0.0f, &clip);
  }
  const PlotIndex* idx = index();
  if (!idx || plot >= idx->series.size()) {
    dl->AddText(ImVec2(x0 + 4.0f, y0 + 1.0f), IM_COL32(150, 150, 160, 255), "indexing...");
    return;
  }
  const PlotSeriesIndex& s = idx->series[plot];
  ++m_stats.plotLanes;
  if (m_windows.size() < m_plotCount) m_windows.resize(m_plotCount);
  WindowCache& cache = m_windows[plot];
  if (!cache.valid || cache.x0 != area.x0 || cache.width != area.width || cache.start != area.view.start ||
      cache.span != area.view.span || cache.traceStart != area.limits.traceStart) {
    queryPlotColumns(s, area, 1.0f, cache.window);
    cache.valid = true;
    cache.x0 = area.x0;
    cache.width = area.width;
    cache.start = area.view.start;
    cache.span = area.view.span;
    cache.traceStart = area.limits.traceStart;
  }
  const PlotWindow& win = cache.window;
  m_stats.plotColumns += static_cast<uint32_t>(win.columns.size());
  m_stats.samplesInView += static_cast<uint32_t>(std::min<uint64_t>(win.samples, UINT32_MAX));

  // Value axis: the range of what is in view (with the neighbours that join the line), padded.
  double lo = 0, hi = 1;
  if (win.any) {
    lo = win.minValue;
    hi = win.maxValue;
  } else if (s.hasFinite) {
    lo = s.minValue;
    hi = s.maxValue;
  }
  if (!(hi > lo)) {
    const double pad = std::max(std::fabs(lo) * 0.1, 1.0);
    lo -= pad;
    hi += pad;
  } else {
    const double pad = (hi - lo) * 0.06;
    lo -= pad;
    hi += pad;
  }
  if (plot < m_ranges.size()) m_ranges[plot] = {lo, hi};
  const float top = y0 + 3.0f, bottom = y1 - 3.0f;
  const auto yOf = [&](double v) {
    const double f = (v - lo) / (hi - lo);
    return static_cast<float>(bottom - std::clamp(f, -0.5, 1.5) * (bottom - top));
  };
  const ImU32 color = toIm(hashColor(0xB10700ULL + s.name.get()));
  dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
  // Zero line when it is in range.
  if (lo < 0.0 && hi > 0.0)
    dl->AddLine(ImVec2(x0, yOf(0.0)), ImVec2(x1, yOf(0.0)), IM_COL32(255, 255, 255, 36));
  std::vector<ImVec2> pts;
  pts.reserve(win.columns.size() * 4 + 2);
  if (win.hasBefore && std::isfinite(win.beforeValue)) pts.emplace_back(win.beforeX, yOf(win.beforeValue));
  for (const PlotColumn& c : win.columns) {
    // first -> (the extreme nearer to first) -> (the other extreme) -> last keeps spikes in the line.
    const bool minFirst = std::fabs(c.first - c.min) <= std::fabs(c.first - c.max);
    pts.emplace_back(c.x, yOf(c.first));
    if (c.max > c.min) {
      pts.emplace_back(c.x, yOf(minFirst ? c.min : c.max));
      pts.emplace_back(c.x, yOf(minFirst ? c.max : c.min));
    }
    pts.emplace_back(c.x, yOf(c.last));
  }
  if (win.hasAfter && std::isfinite(win.afterValue)) pts.emplace_back(win.afterX, yOf(win.afterValue));
  if (pts.size() >= 2)
    dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), color, ImDrawFlags_None, 1.5f);
  else if (pts.size() == 1)
    dl->AddCircleFilled(pts[0], 2.0f, color);

  // Axis labels.
  char text[48];
  std::snprintf(text, sizeof text, "%.4g", hi);
  dl->AddText(ImVec2(x1 - ImGui::CalcTextSize(text).x - 3.0f, y0), IM_COL32(150, 150, 160, 255), text);
  std::snprintf(text, sizeof text, "%.4g", lo);
  dl->AddText(ImVec2(x1 - ImGui::CalcTextSize(text).x - 3.0f, y1 - ImGui::GetFontSize()),
              IM_COL32(150, 150, 160, 255), text);

  // Hover readout: the nearest sample, its value and a crosshair.
  if (mouse && mouse->x >= x0 && mouse->x < x1) {
    const uint64_t tick = area.tickAt(mouse->x);
    const size_t sample = nearestSample(s, tick);
    if (sample != SIZE_MAX) {
      const PlotColumn* column = nullptr;
      for (const PlotColumn& c : win.columns) {
        if (std::fabs(c.x - mouse->x) <= 0.5f) {
          column = &c;
          break;
        }
      }
      m_hover.kind = PlotHover::Kind::Plot;
      m_hover.index = plot;
      m_hover.lines = plotTooltipLines(model, s, sample, column, area.clockHz);
      dl->AddLine(ImVec2(mouse->x, y0), ImVec2(mouse->x, y1), IM_COL32(255, 255, 255, 70));
      const double v = s.value[sample];
      if (std::isfinite(v)) {
        const float sx = static_cast<float>(area.xOf(s.ts[sample]));
        dl->AddCircleFilled(ImVec2(sx, yOf(v)), 3.0f, IM_COL32(255, 255, 255, 255));
        std::snprintf(text, sizeof text, "%.5g", v);
        dl->AddText(ImVec2(mouse->x + 6.0f, y0 + 1.0f), IM_COL32(255, 255, 255, 255), text);
      }
    }
  }
  dl->PopClipRect();
}

}  // namespace tachyon::viewer::timeline
