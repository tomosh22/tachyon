// Frame-time graph strip of the threads timeline (WP-54): one bar per frame of a frame-mark series (rule 6 of
// the attribution rules), bar height = frame duration, a budget line (default 16.67 ms, per-series override),
// bars over budget highlighted. Click a bar to zoom the timeline to that frame; "Slowest" jumps to the
// slowest frame of the current filter (slowestFrames(filter, 1)).
//
// Data. frameSeries() and frameSpans() (the spans frames() reports; no per-frame sample scan, so a long
// capture shows its graph at once) run on the QueryExecutor through Document::analysis() and are reduced to
// FrameBars (start / end / number columns) on the worker; the jump to the slowest frame uses
// slowestFramesShared(). The bars of a series are contiguous in time, so both end and start are sorted and
// everything the UI thread does is a binary search.
//
// Bounded work. Frames narrower than a pixel merge per pixel and the bar shows the tallest of them, found by
// walking the frames of that pixel (at most the visible frames, once); wide frames are their own bar. So a
// strip never has more bars than pixels, and a 100 k frame trace draws as fast as a short one.
#pragma once

#include "analysis/frames.h"
#include "analysis/query.h"
#include "model/model.h"
#include "viewer/common/latest_query.h"
#include "viewer/panel.h"
#include "viewer/timeline/timeline_math.h"

#include <imgui.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer::timeline {

inline constexpr double kDefaultFrameBudgetMs = 16.67;

// The frames of one series as columns, in time order.
struct FrameBars {
  analysis::FrameSeries series;
  std::vector<uint64_t> start, end, number;
  uint64_t maxDuration = 0;

  // Thread-safe (const access).
  size_t size() const { return start.size(); }
  uint64_t duration(size_t i) const { return end[i] - start[i]; }
};

// What the frame query delivers.
struct FrameGraphData {
  std::vector<analysis::FrameSeries> series;  // all series; the default series first
  size_t selected = 0;                        // index into `series` of the one `bars` shows
  FrameBars bars;
};

// Thread-safe (pure). The columns of the frames of a series (analysis::frameSpans, the very spans frames()
// reports: the graph needs no per-frame sample scan).
FrameBars barsFrom(const std::vector<analysis::FrameSpan>& spans, const analysis::FrameSeries& series);

// Budgets in milliseconds: one default and per-series overrides.
class FrameBudgets {
 public:
  FrameBudgets() = default;
  explicit FrameBudgets(double defaultMs) : m_default(defaultMs > 0.0 ? defaultMs : kDefaultFrameBudgetMs) {}

  // Thread-safe (const access).
  double defaultMs() const { return m_default; }
  double msFor(const analysis::FrameSeries& series) const;
  bool overridden(const analysis::FrameSeries& series) const;
  // Not thread-safe. A non-positive value removes the override.
  void set(const analysis::FrameSeries& series, double ms);
  void setDefault(double ms) { m_default = ms > 0.0 ? ms : kDefaultFrameBudgetMs; }

 private:
  struct Entry {
    uint32_t thread, name;
    double ms;
  };
  double m_default = kDefaultFrameBudgetMs;
  std::vector<Entry> m_overrides;
};

// Thread-safe (pure). Ticks of a duration in milliseconds.
uint64_t msToTicks(double ms, double clockHz);

// A bar on screen: a frame, or the tallest of the sub-pixel frames of one pixel (`count` > 1).
struct FrameBarItem {
  float x0 = 0, x1 = 0;
  uint32_t first = 0;    // first frame the bar stands for
  uint32_t count = 1;    // frames the bar stands for
  uint32_t tallest = 0;  // the frame whose duration the bar shows
  uint64_t duration = 0;
  bool overBudget = false;
};

// Thread-safe (pure). The bars of the frames visible in `area`, in order. A frame at least one pixel wide is
// its own bar (x clipped to the plot area); narrower frames merge per pixel. `overBudget` is duration >
// budgetTicks (a zero budget highlights nothing). At most one bar per pixel plus the wide frames.
void layoutFrameBars(const FrameBars& bars, const PlotArea& area, uint64_t budgetTicks,
                     std::vector<FrameBarItem>& out);
// Thread-safe (pure). Value-axis maximum: the tallest visible bar, at least 1.25 times the budget so the
// budget line is always on the graph.
uint64_t yAxisMax(uint64_t maxVisibleDuration, uint64_t budgetTicks);
// Thread-safe (pure). Index of the bar containing screen x, or nullopt (bars are sorted by x).
std::optional<size_t> barAt(const std::vector<FrameBarItem>& items, float x);

// Thread-safe (pure). Tooltip lines of a bar.
std::vector<std::string> frameBarTooltipLines(const FrameBars& bars, const FrameBarItem& item, double clockHz,
                                              uint64_t budgetTicks, const std::string& seriesName);

// Where the timeline goes for a frame (what the click and the "slowest" jump request).
struct FrameJump {
  ThreadIdx thread;
  uint64_t t0 = 0, t1 = 0;
};
// Thread-safe (pure). The frame as a navigation target.
FrameJump jumpTargetOf(const analysis::FrameSummary& frame);
FrameJump jumpTargetOf(const FrameBars& bars, size_t frame);

// Thread-safe (pure). "Frame (Main)", "Frame (global)": the mark name and the thread it was marked on.
std::string seriesLabel(const model::TraceModel& model, const analysis::FrameSeries& series);

// A bar drawn in the last frame (tests).
struct DrawnFrameBar {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // screen rect of the bar
  uint32_t first = 0, count = 1, tallest = 0;
  bool overBudget = false;
};

class FrameGraphView {
 public:
  FrameGraphView() = default;
  FrameGraphView(const FrameGraphView&) = delete;
  FrameGraphView& operator=(const FrameGraphView&) = delete;

  // Main-thread only. Drops the data and cancels the queries in flight; `defaultBudgetMs` from the settings.
  void reset(double defaultBudgetMs = kDefaultFrameBudgetMs);
  // Main-thread only. Starts the frame query when the (document, series) pair changed and delivers finished
  // queries; a finished "slowest" query becomes a NavigationRequest on the bus.
  void update(const PanelContext& context);
  // Main-thread only. Test hook: installs data as if the query had delivered it.
  void inject(std::shared_ptr<const FrameGraphData> data);

  // Main-thread only. Draws the strip (controls line and bar canvas) at the ImGui cursor, `height` pixels for
  // the canvas, and handles hover / click.
  void draw(const PanelContext& context, const model::TraceModel& model, const PlotArea& area, float gutterX0,
            float height);

  // ---- actions (UI and tests); main-thread only ----
  // Chooses the series to show (index into data()->series); re-queries.
  void selectSeries(size_t index);
  // Asks for the slowest frame of the bus filter; the answer navigates the timeline.
  void jumpToSlowest(const PanelContext& context);
  // Navigates to a frame of the shown series.
  void gotoFrame(const PanelContext& context, size_t frame);

  // ---- state; main-thread only ----
  bool ready() const { return m_data != nullptr; }
  bool pending() const { return m_query.pending(); }
  bool jumpPending() const { return m_slowQuery.pending(); }
  const std::shared_ptr<const FrameGraphData>& data() const { return m_data; }
  const std::string& error() const { return m_error; }
  FrameBudgets& budgets() { return m_budgets; }
  const FrameBudgets& budgets() const { return m_budgets; }
  uint64_t issuedQueries() const { return m_query.issued(); }
  const std::vector<DrawnFrameBar>& lastBars() const { return m_drawn; }
  const std::vector<std::string>& lastTooltip() const { return m_tooltip; }
  // Screen rect of the canvas / the "Slowest" button in the last frame.
  struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool drawn = false;
  };
  const Rect& canvasRect() const { return m_canvas; }
  const Rect& slowestButton() const { return m_slowestButton; }
  uint32_t overBudgetBars() const { return m_overBudget; }
  // The value-axis maximum and the budget line y of the last frame.
  uint64_t lastAxisMax() const { return m_axisMax; }
  float lastBudgetY() const { return m_budgetY; }

 private:
  void issue(const PanelContext& context);

  common::LatestQuery<FrameGraphData> m_query;
  common::LatestQuery<std::vector<analysis::FrameSummary>> m_slowQuery;
  std::shared_ptr<const FrameGraphData> m_data;
  std::optional<analysis::FrameSeries> m_wanted;  // the chosen series (nullopt: the default)
  bool m_haveRequested = false;
  uint64_t m_requestedDocument = 0;
  std::optional<analysis::FrameSeries> m_requestedSeries;
  std::string m_error;
  FrameBudgets m_budgets;
  std::vector<FrameBarItem> m_items;
  std::vector<DrawnFrameBar> m_drawn;
  std::vector<std::string> m_tooltip;
  Rect m_canvas, m_slowestButton;
  uint32_t m_overBudget = 0;
  uint64_t m_axisMax = 0;
  float m_budgetY = 0;
  double m_clockHz = 1e9;
};

}  // namespace tachyon::viewer::timeline
