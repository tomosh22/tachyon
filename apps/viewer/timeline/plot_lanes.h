// Plot lanes and the markers lane of the threads timeline (WP-54): below the thread rows, a lane of Marker
// records (ticks with tooltips) and a collapsible group with one line graph per plot name.
//
// Data. A plot can hold tens of millions of samples, so a frame may never walk them. A background job builds
// a PlotIndex once per document: per series two levels of block minima / maxima (32 samples, then 32 blocks
// of those), over the model's own columns (copied only when a series is not sorted by time). A pixel column
// of a lane is then two binary searches (its sample range) and a min / max over that range in O(32 + 32 +
// blocks of the range), so the work of a lane is bounded by its plot width, and a one-sample spike is never
// lost: the line of a column spans its min to its max.
//
// Markers are thinned the same way: one tick per pixel (binary search to the first marker of the next pixel),
// with the number of markers it stands for.
#pragma once

#include "model/model.h"
#include "viewer/job_queue.h"
#include "viewer/panel.h"
#include "viewer/timeline/timeline_math.h"

#include <imgui.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer::timeline {

// ---- index
// --------------------------------------------------------------------------------------------------

inline constexpr size_t kPlotBlock0 = 32;  // samples per level-0 block
inline constexpr size_t kPlotBlock1 = 32;  // level-0 blocks per level-1 block

struct PlotSeriesIndex {
  StringIdx name;
  const uint64_t* ts = nullptr;  // sorted ascending; the model's column, or ownedTs
  const double* value = nullptr;
  size_t n = 0;
  std::vector<uint64_t> ownedTs;  // only when the model's column was not sorted
  std::vector<double> ownedValue;
  std::vector<double> min0, max0, min1, max1;  // block extremes of the finite values
  double minValue = 0, maxValue = 0;           // over the whole series (valid when hasFinite)
  bool hasFinite = false;
};

struct PlotIndex {
  std::shared_ptr<const model::TraceModel> model;  // keeps the columns the series point at alive
  std::vector<PlotSeriesIndex> series;             // like model->plots
  std::vector<uint32_t> markerOrder;               // marker indices sorted by ts; empty when the model's are
  uint64_t sampleCount = 0;
};

// Thread-safe (pure). Builds the index of the model's plots and markers. `cancel` (may be null) is polled; a
// cancelled build returns nullptr. `progress` (may be empty) gets values in [0, 1].
std::shared_ptr<PlotIndex> buildPlotIndex(std::shared_ptr<const model::TraceModel> model,
                                          const std::atomic<bool>* cancel,
                                          const std::function<void(float)>& progress = {});

// Thread-safe (pure). Min and max of the finite values of samples [i0, i1) of a series; false when there is
// none.
bool plotRangeMinMax(const PlotSeriesIndex& s, size_t i0, size_t i1, double& mn, double& mx);

// One pixel column of a lane.
struct PlotColumn {
  float x = 0;  // screen x of the column's centre
  uint32_t count = 0;
  double min = 0, max = 0, first = 0, last = 0;  // over the samples of the column
};

struct PlotWindow {
  std::vector<PlotColumn> columns;  // only columns that hold samples, in x order
  bool hasBefore = false, hasAfter = false;
  double beforeValue = 0, afterValue = 0;  // the neighbouring samples outside the view (they join the line)
  float beforeX = 0, afterX = 0;
  bool any = false;  // some finite value in view (or a neighbour)
  double minValue = 0, maxValue = 0;
  uint64_t samples = 0;  // samples inside the view
};

// Thread-safe (pure). The columns of a series for `area`, each `columnPixels` wide: sample ranges by binary
// search, extremes from the index. Work is O(columns * log n).
void queryPlotColumns(const PlotSeriesIndex& s, const PlotArea& area, float columnPixels, PlotWindow& out);

// Thread-safe (pure). The sample of `s` nearest in time to `tick`, or SIZE_MAX for an empty series.
size_t nearestSample(const PlotSeriesIndex& s, uint64_t tick);

// ---- markers
// --------------------------------------------------------------------------------------------------

// Markers in time order (the model's vector, or through the index's order).
struct MarkerView {
  const std::vector<model::Marker>* markers = nullptr;
  const std::vector<uint32_t>* order = nullptr;  // empty: the model's order is sorted

  size_t size() const { return markers ? markers->size() : 0; }
  const model::Marker& at(size_t k) const { return (*markers)[order && !order->empty() ? (*order)[k] : k]; }
};

struct MarkerItem {
  float x = 0;         // pixel centre
  uint32_t first = 0;  // position (in time order) of the first marker of the tick
  uint32_t count = 1;  // markers in the pixel
};

// Thread-safe (pure). One item per pixel that holds markers, in order, at most `maxItems`; false when the cap
// cut the list short.
bool layoutMarkers(const MarkerView& markers, const PlotArea& area, size_t maxItems,
                   std::vector<MarkerItem>& out);
// Thread-safe (pure). The item within `tolerancePx` of screen x (the nearest), or nullopt.
std::optional<size_t> markerItemAt(const std::vector<MarkerItem>& items, float x, float tolerancePx);
// Thread-safe (pure). Tooltip lines of a marker item.
std::vector<std::string> markerTooltipLines(const model::TraceModel& model, const MarkerView& markers,
                                            const MarkerItem& item, double clockHz);
// Thread-safe (pure). Tooltip lines for the sample nearest the cursor of a plot.
std::vector<std::string> plotTooltipLines(const model::TraceModel& model, const PlotSeriesIndex& s,
                                          size_t sample, const PlotColumn* column, double clockHz);

// ---- layout of the section
// ----------------------------------------------------------------------------------

struct GlobalGeometry {
  float markerLaneHeight = 20.0f;
  float plotHeaderHeight = 16.0f;
  float plotLaneHeight = 56.0f;
  float gap = 2.0f;
  float sectionGap = 6.0f;  // above the section
};

struct GlobalLane {
  enum class Kind : uint8_t { Markers, PlotsHeader, Plot };
  Kind kind = Kind::Markers;
  uint32_t index = 0;        // the plot (Plot)
  float y0 = 0, height = 0;  // relative to the section top
};

struct GlobalLayout {
  std::vector<GlobalLane> lanes;
  float height = 0;  // 0 without lanes
};

// Thread-safe (pure). Markers lane (if any markers), the plots header and, unless collapsed, one lane per
// plot.
GlobalLayout layoutGlobalLanes(bool hasMarkers, size_t plotCount, bool plotsCollapsed,
                               const GlobalGeometry& geom);
// Thread-safe (pure). The lane containing section-relative y, or nullptr.
const GlobalLane* globalLaneAt(const GlobalLayout& layout, float y);

// ---- the view
// -----------------------------------------------------------------------------------------------

struct PlotFrameStats {
  uint32_t plotLanes = 0, plotColumns = 0, markerTicks = 0, markersDrawn = 0, samplesInView = 0;
};

// What the cursor is over in the section (the tooltip and the readout).
struct PlotHover {
  enum class Kind : uint8_t { None, Marker, Plot, PlotsHeader };
  Kind kind = Kind::None;
  uint32_t index = 0;  // the plot
  std::vector<std::string> lines;
};

class PlotLanesView {
 public:
  PlotLanesView() = default;
  PlotLanesView(const PlotLanesView&) = delete;
  PlotLanesView& operator=(const PlotLanesView&) = delete;

  // Main-thread only. Cancels the build job and drops the index.
  void reset();
  // Main-thread only. Starts the index build for `model` (on `context.jobs`, or inline without a job queue or
  // with `buildInline`).
  void start(const PanelContext& context, std::shared_ptr<const model::TraceModel> model, bool buildInline);
  // Main-thread only. Test hook: installs a built index.
  void inject(std::shared_ptr<const PlotIndex> index);

  // Main-thread only.
  bool hasContent() const { return m_hasMarkers || m_plotCount > 0; }
  bool ready() const;
  bool collapsed() const { return m_plotsCollapsed; }
  void setCollapsed(bool collapsed) { m_plotsCollapsed = collapsed; }
  size_t plotCount() const { return m_plotCount; }
  bool hasMarkers() const { return m_hasMarkers; }
  const PlotIndex* index() const;
  // The section layout (no lanes while hidden).
  const GlobalLayout& layout(const GlobalGeometry& geom);
  void setVisible(bool visible) { m_visible = visible; }
  bool visible() const { return m_visible; }

  // Main-thread only. Draws the section whose top is at screen y `top`. `mouse` is the cursor when it is over
  // the section's canvas, else null; it feeds the hover readout and `hover()`.
  void draw(ImDrawList* dl, const model::TraceModel& model, const PlotArea& area, float top, float gutterX0,
            const GlobalGeometry& geom, const ImVec2* mouse);
  const PlotHover& hover() const { return m_hover; }
  const PlotFrameStats& frameStats() const { return m_stats; }
  // Rects of the lanes drawn in the last frame (tests aim the mouse with them): {y0, y1} per GlobalLane.
  struct DrawnLane {
    GlobalLane lane;
    float y0 = 0, y1 = 0;
  };
  const std::vector<DrawnLane>& drawnLanes() const { return m_drawnLanes; }
  const std::vector<MarkerItem>& lastMarkerItems() const { return m_markerItems; }
  // Value range shown by a plot lane in the last frame (tests).
  std::optional<std::pair<double, double>> lastRange(uint32_t plot) const;

 private:
  struct Shared;
  void drawMarkers(ImDrawList* dl, const model::TraceModel& model, const PlotArea& area, float y0, float y1,
                   float gutterX0, const ImVec2* mouse);
  void drawPlot(ImDrawList* dl, const model::TraceModel& model, const PlotArea& area, uint32_t plot, float y0,
                float y1, float gutterX0, const ImVec2* mouse);

  std::shared_ptr<Shared> m_shared;
  std::shared_ptr<const PlotIndex> m_injected;
  JobQueue* m_jobs = nullptr;
  JobQueue::JobId m_job = 0;
  bool m_hasMarkers = false;
  size_t m_plotCount = 0;
  bool m_plotsCollapsed = false;
  bool m_visible = true;
  GlobalLayout m_layout;
  GlobalGeometry m_layoutGeom;
  bool m_layoutValid = false;
  size_t m_layoutPlots = 0;
  bool m_layoutCollapsed = false;
  bool m_layoutMarkers = false;
  PlotHover m_hover;
  PlotFrameStats m_stats;
  std::vector<DrawnLane> m_drawnLanes;
  std::vector<MarkerItem> m_markerItems;
  std::vector<std::pair<double, double>> m_ranges;
  // The last column query of each plot lane and what it was made for: a frame with the same view reuses it.
  struct WindowCache {
    bool valid = false;
    float x0 = 0, width = 0;
    double start = 0, span = 0;
    uint64_t traceStart = 0;
    PlotWindow window;
  };
  std::vector<WindowCache> m_windows;  // by plot
};

}  // namespace tachyon::viewer::timeline
