// TimelineWidget (WP-42): the ImGui widget behind the "Threads Timeline" panel. It draws the overview strip,
// the time ruler with frame marks and one row per thread (collapsed: state band + sample strip; expanded:
// flame chart), and handles pan / zoom / drag-select / keyboard navigation. All geometry and colour decisions
// live in the pure timeline_*.h headers; this class only glues them to ImGui, the LOD queries and the
// SelectionBus.
//
// Caching rule (WP-40 review): nothing derived from the document survives a document change. Everything
// derived lives in DocState, which is replaced whenever PanelContext::documentGeneration (or the document
// pointer) changes.
#pragma once

#include "viewer/panel.h"
#include "viewer/timeline/timeline_colors.h"
#include "viewer/timeline/timeline_data.h"
#include "viewer/timeline/timeline_math.h"
#include "viewer/timeline/timeline_rows.h"

#include <imgui.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer::timeline {

struct TimelineOptions {
  SortKey sortKey = SortKey::RunningTime;
  bool reverseSort = false;
  FrameColorMode colorMode = FrameColorMode::ByModule;
  // Inline frames in the flame chart; needs the symbol service (WP-20), so it only records the preference.
  bool showInlineFrames = false;
  bool showFrameMarks = true;
  // Width of one query column in pixels (1 = a query column per pixel). Bigger is cheaper to draw.
  uint32_t columnPixels = 2;
  float gutterWidth = 150.0f;
  // Cap on the flame depths shown for an expanded row.
  uint32_t maxFlameDepths = 40;
  // Test hook: build the LOD synchronously on the first draw instead of on the JobQueue.
  bool buildInline = false;
  // Test hook: do not start the LOD build (every row stays pending) until this is cleared.
  bool deferLodBuild = false;
};

// Counters of the last drawn frame (tests and the perf budget).
struct TimelineFrameStats {
  uint32_t rowsTotal = 0, rowsDrawn = 0, rowsPending = 0;
  uint32_t bandQueries = 0, stripQueries = 0, flameQueries = 0;
  uint32_t rects = 0;
  uint32_t flameSpans = 0;
  uint32_t frameMarksDrawn = 0;
};

// Where the widget put things on screen in the last frame (tests aim the mouse with it).
struct TimelineScreenGeometry {
  float plotX0 = 0, plotWidth = 0;
  float overviewY0 = 0, overviewY1 = 0;
  float headerY0 = 0, headerY1 = 0;  // ruler + frame-mark lane
  float rowsY0 = 0, rowsY1 = 0;      // visible part of the rows child window
  float rowsContentY0 = 0;           // screen y of content-space 0 (scroll applied)
  float gutterX0 = 0;
};

class TimelineWidget {
 public:
  TimelineWidget();
  ~TimelineWidget();
  TimelineWidget(const TimelineWidget&) = delete;
  TimelineWidget& operator=(const TimelineWidget&) = delete;

  // Main-thread only. Draws into the current ImGui window (the caller did Begin()).
  void draw(const PanelContext& context);
  // Main-thread only.
  void onSelectionChanged(const PanelContext& context, const SelectionState& state, SelectionField field);
  // Main-thread only. Drops everything derived from the previous document.
  void onDocumentChanged(const Document* document);

  // Main-thread only.
  TimelineOptions& options() { return m_options; }
  const TimelineOptions& options() const { return m_options; }

  // ---- state access (tests and commands); main-thread only ----
  bool hasDocument() const { return m_doc != nullptr; }
  const TimeLimits& limits() const;
  TimeView view() const;
  void setView(TimeView view);
  void fit();
  const std::optional<TickRange>& selectionRange() const { return m_selection; }
  void setSelectionRange(std::optional<TickRange> range) { m_selection = range; }
  // Applies the selection as the SelectionBus time filter (the F key). Returns false without a selection.
  bool applySelectionAsFilter(SelectionBus& bus);
  // Clears the selection and, if it is still the one applySelectionAsFilter() set, the time range of the bus
  // filter (the Esc key). Thread / function / subtree restrictions in the filter are kept (the range goes
  // back to the whole trace); a filter that another panel set is left alone.
  void clearSelection(SelectionBus* bus);
  // Rows in display order (empty before the document is known).
  const std::vector<ThreadIdx>& rowOrder() const;
  void setExpanded(ThreadIdx thread, bool expanded);
  void setPinned(ThreadIdx thread, bool pinned);
  void setHidden(ThreadIdx thread, bool hidden);
  RowFlags flagsOf(ThreadIdx thread) const;
  // True once the LOD build finished (or failed).
  bool lodFinished() const;
  size_t lodReadyThreads() const;
  const TimelineFrameStats& frameStats() const { return m_stats; }
  // Text lines of the tooltip drawn in the last frame (empty if none).
  const std::vector<std::string>& lastTooltip() const { return m_tooltip; }
  // Frame numbers whose label was drawn in the last frame.
  const std::vector<uint64_t>& lastFrameLabels() const { return m_frameLabels; }
  const TimelineScreenGeometry& screenGeometry() const { return m_geometry; }
  // Block until the background build is done, pumping nothing (only valid with buildInline or after the job
  // completed); test helper.
  const std::string& lodError() const;

 private:
  struct DocState;
  struct RowCache;
  struct Hit;

  void startDocument(const PanelContext& context);
  void startBuild(const PanelContext& context);
  void resetDocument();
  void drawToolbar(const PanelContext& context);
  void drawStatusLine(const PanelContext& context);
  void drawOverview(const PanelContext& context);
  void drawHeader(const PanelContext& context);
  void drawRows(const PanelContext& context);
  void drawRowContents(ImDrawList* dl, const PanelContext& context, size_t layoutIndex, float rowTopScreen,
                       RowCache& cache);
  void drawFlame(ImDrawList* dl, const PanelContext& context, const RowLayout& row, float rowTopScreen,
                 const RowCache& cache);
  void refreshRow(const PanelContext& context, const RowLayout& row, RowCache& cache);
  void updateOrder();
  void updateFlameDepths();
  void handleViewInput(const PanelContext& context, bool header, bool inGutter);
  void handleKeys(const PanelContext& context);
  void zoomAround(double cursorFrac, double factor);
  Hit hitTest(const PanelContext& context, float mouseX, float mouseY) const;
  void drawTooltip(const PanelContext& context, const Hit& hit);
  void publishHover(const PanelContext& context, const Hit& hit);
  void applyClick(const PanelContext& context, const Hit& hit);
  void drawRangeOverlays(ImDrawList* dl, const PanelContext& context, float y0, float y1);
  uint32_t flameDepthFor(ThreadIdx thread) const;

  TimelineOptions m_options;
  TimelineFrameStats m_stats;
  TimelineScreenGeometry m_geometry;
  RowGeometry m_rowGeometry;

  std::unique_ptr<DocState> m_doc;
  uint64_t m_generation = 0;
  const Document* m_documentPtr = nullptr;

  // Interaction state (not derived from the document).
  std::optional<TickRange> m_selection;
  std::optional<TickRange> m_appliedRange;  // the range applySelectionAsFilter() put on the bus
  FramePosition m_selectedPosition = FramePosition::Leaf;
  CallstackIdx m_selectedNode;  // frame behind the last click (re-keyed when symbols resolve)
  std::vector<std::string> m_tooltip;
  std::vector<uint64_t> m_frameLabels;
  struct Drag {
    enum class Mode : uint8_t { None, Pan, Select, Overview } mode = Mode::None;
    float startX = 0;
    bool moved = false;
    bool fromGutter = false;
    bool fromHeader = false;
  } m_drag;
  bool m_hoverPublished = false;
  bool m_anyHovered = false;
  ThreadIdx m_contextThread;
  float m_pendingScrollY = -1.0f;
};

}  // namespace tachyon::viewer::timeline
