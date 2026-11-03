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

#include "viewer/interaction/arrow_geometry.h"
#include "viewer/interaction/interaction_model.h"
#include "viewer/panel.h"
#include "viewer/stats/instance_index.h"
#include "viewer/timeline/frame_graph.h"
#include "viewer/timeline/plot_lanes.h"
#include "viewer/timeline/scope_lanes.h"
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
  // WP-64: byte ceiling of the whole trace used to budget the LOD levels up front (LodOptions::maxBytes = the
  // ceiling minus the model); 0 = Settings::memoryCeilingPercent of the physical RAM.
  uint64_t memoryCeilingBytes = 0;
  // Test hook: do not start the LOD build (every row stays pending) until this is cleared.
  bool deferLodBuild = false;
  // WP-54: what rows show below the state band. Effective only for traces with scope events (others are
  // always Sampled); a row's own mode (RowFlags::mode) wins over this global one.
  RowMode rowMode = RowMode::Both;
  // WP-54: the frame-time strip above the rows and the markers / plot lanes below them (each only when the
  // trace has frame marks / markers or plots).
  bool showFrameGraph = true;
  bool showPlots = true;
  float frameGraphHeight = 44.0f;  // canvas height at the base font size
  // Test hook (WP-54): record the scope items drawn in each frame (ScopeLanesView::drawnItems()).
  bool recordDrawnItems = false;
};

// Counters of the last drawn frame (tests and the perf budget).
struct TimelineFrameStats {
  uint32_t rowsTotal = 0, rowsDrawn = 0, rowsPending = 0;
  uint32_t bandQueries = 0, stripQueries = 0, flameQueries = 0;
  uint32_t rects = 0;
  uint32_t flameSpans = 0;
  uint32_t frameMarksDrawn = 0;
  uint32_t highlightRects = 0;  // WP-47: instance highlight rects drawn
  // WP-54.
  uint32_t scopeRows = 0, scopeLines = 0, scopeItems = 0, scopeLabels = 0, scopeRowsPending = 0;
  uint32_t frameBars = 0;
  uint32_t plotLanes = 0, plotColumns = 0, markerTicks = 0;
};

// One wake-up arrow drawn in the last frame (WP-46; tests).
struct WakeArrowInfo {
  interaction::WaitEpisode episode;
  interaction::ArrowGeometry geometry;
  bool selected = false;   // the persistent arrow of SelectionState::selectedWait (else the hover arrow)
  std::string wakerLabel;  // text next to the edge marker
};

// One rect of the instance highlight drawn in the last frame (WP-47; tests).
struct HighlightRectInfo {
  ThreadIdx thread;
  uint32_t instance = 0;  // index into the highlighted instance list (the first one of a run)
  uint32_t count = 1;     // instances the rect stands for (> 1: a run of sub-pixel instances)
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool current = false;  // the instance last navigated to
};

// Where the widget put things on screen in the last frame (tests aim the mouse with it).
struct TimelineScreenGeometry {
  float plotX0 = 0, plotWidth = 0;
  float overviewY0 = 0, overviewY1 = 0;
  float headerY0 = 0, headerY1 = 0;  // ruler + frame-mark lane
  float rowsY0 = 0, rowsY1 = 0;      // visible part of the rows child window
  float rowsContentY0 = 0;           // screen y of content-space 0 (scroll applied)
  float gutterX0 = 0;
  float frameGraphY0 = 0, frameGraphY1 = 0;  // WP-54: the frame-time canvas (0 / 0 when not drawn)
  float sectionY0 = 0;                       // WP-54: screen y of the markers / plot section's top
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
  // Wake-up arrows drawn in the last frame: the hovered wait's and the selected wait's (WP-46).
  const std::vector<WakeArrowInfo>& lastArrows() const { return m_arrows; }
  // WP-47: the rects of the instance highlight (SelectionState::instanceHighlight) drawn in the last frame,
  // one per visible instance on a visible row.
  const std::vector<HighlightRectInfo>& lastHighlightRects() const { return m_highlightRects; }
  // Screen y of the centre of the state band of `thread`'s row in the last frame (nullopt: no such row).
  std::optional<float> bandCenterY(ThreadIdx thread) const;

  // ---- WP-54: instrumentation lanes, frame graph, plots ----
  // The mode a row shows now: its own, else the global one; Sampled for traces without scope events.
  RowMode effectiveRowMode(ThreadIdx thread) const;
  // Sets one row's mode (nullopt: follow the global mode).
  void setRowMode(ThreadIdx thread, std::optional<RowMode> mode);
  // Sets the global mode and lets every row follow it.
  void setGlobalRowMode(RowMode mode);
  // Screen y of the centre of scope line `line` of `thread`'s row in the last frame (nullopt: no such line).
  std::optional<float> scopeLineCenterY(ThreadIdx thread, uint32_t line) const;
  // Screen y of the centre of a lane of the markers / plot section in the last frame.
  std::optional<float> sectionLaneCenterY(GlobalLane::Kind kind, uint32_t index = 0) const;
  // The row layout of the last frame for `thread` (nullptr: not shown).
  const RowLayout* rowLayoutOf(ThreadIdx thread) const;
  ScopeLanesView& scopeLanes();
  const ScopeLanesView& scopeLanes() const;
  FrameGraphView& frameGraph();
  const FrameGraphView& frameGraph() const;
  PlotLanesView& plotLanes();
  const PlotLanesView& plotLanes() const;
  // The scope or section item the tooltip of the last frame described (tests).
  const std::optional<FunctionKey>& lastHoveredScope() const { return m_hoveredScope; }
  // ---- WP-64: memory ceiling ----
  // Main-thread only. Bytes held by the timeline index (0 until it exists).
  size_t lodBytes() const;
  // Main-thread only, between frames. Frees at least `bytes` of timeline index (sample mipmap levels, finest
  // first); returns the bytes freed (0 when the index is still building or nothing is left to free).
  size_t trimLod(size_t bytes);
  // Main-thread only. What the memory ceiling took from the index so far (zeros without an index).
  analysis::LodTrim lodTrim() const;
  uint32_t lodDegradedBaseLevels() const;
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
  void applyNavigation();
  void drawWakeArrows(ImDrawList* dl, const PanelContext& context, const Hit& hit);
  void drawWakeArrow(ImDrawList* dl, const PanelContext& context, const interaction::WaitEpisode& episode,
                     bool selected);
  void drawInstanceHighlights(ImDrawList* dl, const PanelContext& context, const RowRange& visible,
                              float originY);
  void drawRangeOverlays(ImDrawList* dl, const PanelContext& context, float y0, float y1);
  void drawFrameGraphStrip(const PanelContext& context);
  void drawSection(ImDrawList* dl, const PanelContext& context, float originY, const ImVec2* mouse);
  void updateInstrumentation(const PanelContext& context);
  PlotArea plotArea() const;
  RowMode rowModeOf(ThreadIdx thread, bool hasScopes) const;
  uint32_t flameDepthFor(ThreadIdx thread) const;

  TimelineOptions m_options;
  TimelineFrameStats m_stats;
  TimelineScreenGeometry m_geometry;
  RowGeometry m_rowGeometry;

  std::unique_ptr<DocState> m_doc;
  uint64_t m_generation = 0;
  const Document* m_documentPtr = nullptr;
  // WP-64: when a streaming preview is replaced by the complete document of the same stream, the user's place
  // (view, row flags, scroll) carries over.
  uint64_t m_streamId = 0;
  struct Carry {
    TimeView view;
    bool wasFit = false;  // the view showed the whole preview: show the whole trace now
    std::vector<RowFlags> flags;
    float rowsScrollY = 0;
  };
  std::optional<Carry> m_carry;

  // Interaction state (not derived from the document).
  std::optional<TickRange> m_selection;
  std::optional<TickRange> m_appliedRange;         // the range applySelectionAsFilter() put on the bus
  std::optional<FunctionKey> m_publishedFunction;  // the key this widget last put on the bus for a click
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
  std::vector<WakeArrowInfo> m_arrows;
  std::optional<NavigationRequest> m_pendingNav;  // WP-46: applied at the start of the next draw
  uint64_t m_lastNavSerial = 0;
  ThreadIdx m_scrollToThread;  // row to bring into view once the layout is known
  // WP-47: per-thread index of the highlighted instance list, rebuilt when the bus generation changes.
  std::unique_ptr<stats::InstanceIndex> m_highlightIndex;
  uint64_t m_highlightGeneration = 0;
  std::vector<HighlightRectInfo> m_highlightRects;
  std::optional<FunctionKey> m_hoveredScope;  // WP-54
  GlobalGeometry m_globalGeometry;
};

}  // namespace tachyon::viewer::timeline
