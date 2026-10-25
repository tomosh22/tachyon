#include "viewer/timeline/timeline_widget.h"

#include "analysis/thread_states.h"
#include "analysis/wait_reasons.h"
#include "viewer/document.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace tachyon::viewer::timeline {

namespace {

constexpr size_t kNoRow = std::numeric_limits<size_t>::max();
constexpr uint32_t kDefaultFlameDepths = 8;
constexpr float kBaseUi = 13.0f;  // font size the base geometry numbers were chosen for

const TimeLimits kEmptyLimits{};

ImU32 toIm(Color32 c) {
  return static_cast<ImU32>(c);
}

// Appends many filled rectangles to a draw list with one PrimReserve per chunk instead of one per rectangle.
// Nothing else may be added to the draw list while a batch is alive.
class RectBatch {
 public:
  RectBatch(ImDrawList* dl, size_t upperBound) : m_dl(dl), m_remaining(upperBound) { refill(); }
  ~RectBatch() { release(); }
  RectBatch(const RectBatch&) = delete;
  RectBatch& operator=(const RectBatch&) = delete;

  void add(float x0, float y0, float x1, float y1, ImU32 color) {
    if (m_used == m_reserved) {
      release();
      refill();
    }
    m_dl->PrimRect(ImVec2(x0, y0), ImVec2(x1, y1), color);
    ++m_used;
  }

 private:
  static constexpr size_t kChunk = 4096;  // 16384 vertices: far below the 16-bit index limit
  void refill() {
    m_reserved = std::min(m_remaining, kChunk);
    m_used = 0;
    if (m_reserved > 0) m_dl->PrimReserve(static_cast<int>(m_reserved * 6), static_cast<int>(m_reserved * 4));
  }
  void release() {
    if (m_reserved > m_used) {
      m_dl->PrimUnreserve(static_cast<int>((m_reserved - m_used) * 6),
                          static_cast<int>((m_reserved - m_used) * 4));
    }
    m_remaining -= std::min(m_remaining, m_used);
    m_reserved = m_used = 0;
  }

  ImDrawList* m_dl;
  size_t m_remaining;
  size_t m_reserved = 0, m_used = 0;
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------
// Per-document state. Replaced wholesale when the document (generation) changes.

struct TimelineWidget::RowCache {
  bool valid = false;
  bool pending = false;
  uint64_t t0 = 0, t1 = 0;
  uint32_t columns = 0;
  uint32_t depths = 0;  // flame depths queried (0 = none)
  analysis::StateBandResult band;
  analysis::SampleStripResult strip;
  analysis::TimelineResult flame;
};

struct TimelineWidget::DocState {
  std::shared_ptr<LodShared> shared;
  const model::TraceModel* model = nullptr;  // owned by `shared`
  JobQueue* jobs = nullptr;
  JobQueue::JobId job = 0;
  bool buildStarted = false;

  TimeLimits limits;
  TimeView view;
  double clockHz = 1e9;
  uint32_t platform = 0;

  std::vector<FrameTick> frameTicks;
  std::vector<RowFlags> flags;
  std::vector<ThreadIdx> order;
  bool orderDirty = true;
  bool orderUsedSummaries = false;
  std::vector<uint32_t> flameDepth;  // by thread, 0 = unknown yet
  std::vector<RowLayout> layout;
  std::vector<RowCache> rows;  // by thread

  // Overview strip contents: average fraction of Running over the ready threads.
  std::vector<float> overview;
  uint32_t overviewColumns = 0;
  size_t overviewReady = std::numeric_limits<size_t>::max();

  float rowsScrollY = 0;
  float rowsViewHeight = 0;
  float contentHeight = 0;
};

struct TimelineWidget::Hit {
  bool inPlot = false;
  bool inGutter = false;
  size_t layoutIndex = kNoRow;
  ThreadIdx thread;
  RowSection section = RowSection::None;
  uint32_t depth = 0;
  uint64_t ts = 0;
  CallstackIdx stack;  // representative callstack node under the cursor
  const analysis::TimelineSpan* span = nullptr;
  bool pending = false;
  float share = 0;
};

TimelineWidget::TimelineWidget() = default;
TimelineWidget::~TimelineWidget() = default;

// ---------------------------------------------------------------------------------------------------------
// State access

const TimeLimits& TimelineWidget::limits() const {
  return m_doc ? m_doc->limits : kEmptyLimits;
}

TimeView TimelineWidget::view() const {
  return m_doc ? m_doc->view : TimeView{};
}

void TimelineWidget::setView(TimeView view) {
  if (m_doc) m_doc->view = clampView(view, m_doc->limits);
}

void TimelineWidget::fit() {
  if (m_doc) m_doc->view = fitView(m_doc->limits);
}

const std::vector<ThreadIdx>& TimelineWidget::rowOrder() const {
  static const std::vector<ThreadIdx> kEmpty;
  return m_doc ? m_doc->order : kEmpty;
}

RowFlags TimelineWidget::flagsOf(ThreadIdx thread) const {
  if (!m_doc || thread.get() >= m_doc->flags.size()) return {};
  return m_doc->flags[thread.get()];
}

void TimelineWidget::setExpanded(ThreadIdx thread, bool expanded) {
  if (m_doc && thread.get() < m_doc->flags.size()) m_doc->flags[thread.get()].expanded = expanded;
}

void TimelineWidget::setPinned(ThreadIdx thread, bool pinned) {
  if (m_doc && thread.get() < m_doc->flags.size()) {
    m_doc->flags[thread.get()].pinned = pinned;
    m_doc->orderDirty = true;
  }
}

void TimelineWidget::setHidden(ThreadIdx thread, bool hidden) {
  if (m_doc && thread.get() < m_doc->flags.size()) {
    m_doc->flags[thread.get()].hidden = hidden;
    m_doc->orderDirty = true;
  }
}

bool TimelineWidget::lodFinished() const {
  if (!m_doc || !m_doc->shared) return false;
  const analysis::TimelineLod* lod = m_doc->shared->lodIfPrepared();
  if (m_doc->shared->failed.load(std::memory_order_acquire)) return true;
  return lod && lod->readyThreadCount() == lod->threadCount();
}

size_t TimelineWidget::lodReadyThreads() const {
  if (!m_doc || !m_doc->shared) return 0;
  const analysis::TimelineLod* lod = m_doc->shared->lodIfPrepared();
  return lod ? lod->readyThreadCount() : 0;
}

const std::string& TimelineWidget::lodError() const {
  static const std::string kNone;
  return m_doc && m_doc->shared ? m_doc->shared->error : kNone;
}

bool TimelineWidget::applySelectionAsFilter(SelectionBus& bus) {
  if (!m_selection) return false;
  ViewFilter filter;
  if (bus.state().timeFilter) filter = *bus.state().timeFilter;  // keep thread / function restrictions
  filter.t0 = m_selection->t0;
  filter.t1 = m_selection->t1;
  bus.setTimeFilter(filter);
  return true;
}

void TimelineWidget::clearSelection(SelectionBus* bus) {
  m_selection.reset();
  if (bus && bus->state().timeFilter) bus->setTimeFilter(std::nullopt);
}

// ---------------------------------------------------------------------------------------------------------
// Document lifecycle

void TimelineWidget::onDocumentChanged(const Document* document) {
  if (!document) resetDocument();
  // A non-null document is picked up by the generation check in draw().
}

void TimelineWidget::resetDocument() {
  if (m_doc && m_doc->jobs && m_doc->job != 0) m_doc->jobs->cancel(m_doc->job);
  m_doc.reset();
  m_documentPtr = nullptr;
  m_selection.reset();
  m_drag = {};
  m_hoverPublished = false;
}

void TimelineWidget::startDocument(const PanelContext& context) {
  resetDocument();
  m_generation = context.documentGeneration;
  m_documentPtr = context.document;
  auto doc = std::make_unique<DocState>();
  doc->shared = std::make_shared<LodShared>(context.document->sharedModel());
  const model::TraceModel& model = *doc->shared->model;
  doc->model = &model;
  doc->limits.traceStart = model.header.start_ts;
  doc->limits.traceEnd = std::max(model.header.end_ts, model.header.start_ts + 1);
  doc->limits.minSpan = 16.0;
  doc->view = fitView(doc->limits);
  doc->clockHz = model.header.clock_frequency_hz ? static_cast<double>(model.header.clock_frequency_hz) : 1e9;
  doc->platform = model.header.platform;
  doc->frameTicks = buildFrameTicks(model);
  doc->flags.assign(model.threads.size(), RowFlags{});
  doc->flameDepth.assign(model.threads.size(), 0);
  doc->rows.resize(model.threads.size());

  m_doc = std::move(doc);
}

void TimelineWidget::startBuild(const PanelContext& context) {
  DocState& d = *m_doc;
  d.buildStarted = true;
  const analysis::LodOptions options;
  if (m_options.buildInline || !context.jobs) {
    buildLodShared(*d.shared, options, nullptr, {});
    d.shared->finished.store(true, std::memory_order_release);
  } else {
    d.jobs = context.jobs;
    const PanelContext redraw = context;  // only the redraw hook is used from the completion
    d.job = startLodBuild(*context.jobs, d.shared, options, [redraw] { redraw.requestRedraw(); });
  }
}

void TimelineWidget::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                        SelectionField field) {
  (void)state;
  if (field == SelectionField::All) {
    m_selection.reset();  // a new document or an explicit reset
  }
  if (field != SelectionField::Hovered) context.requestRedraw();
}

// ---------------------------------------------------------------------------------------------------------
// Per-frame preparation

void TimelineWidget::updateOrder() {
  DocState& d = *m_doc;
  const auto* summaries = d.shared->summariesIfReady();
  if (summaries && !d.orderUsedSummaries) d.orderDirty = true;
  if (!d.orderDirty) return;
  d.orderDirty = false;
  d.orderUsedSummaries = summaries != nullptr;

  const model::TraceModel& model = *d.model;
  std::vector<ThreadSortInfo> infos(model.threads.size());
  for (size_t t = 0; t < infos.size(); ++t) {
    ThreadSortInfo& i = infos[t];
    i.thread = ThreadIdx(static_cast<uint32_t>(t));
    i.name = model.threads[t].name;
    i.tid = model.threads[t].tid;
    i.sampleCount = t < model.samplesByThread.size() ? model.samplesByThread[t].ts.size() : 0;
    i.runningTicks = summaries && t < summaries->size() ? (*summaries)[t].runningTicks : 0;
  }
  d.order = orderRows(infos, d.flags, m_options.sortKey, m_options.reverseSort);
}

void TimelineWidget::updateFlameDepths() {
  DocState& d = *m_doc;
  const analysis::TimelineLod* lod = d.shared->lodIfPrepared();
  if (!lod || lod->levels().empty()) return;
  const size_t top = lod->levels().size() - 1;
  for (const ThreadIdx t : d.order) {
    if (d.flameDepth[t.get()] != 0 || !lod->threadReady(t)) continue;
    analysis::LodBucket bucket;
    uint32_t depth = kDefaultFlameDepths;
    if (lod->bucket(t, top, 0, bucket)) depth = std::max<uint32_t>(bucket.maxDepth, 1);
    d.flameDepth[t.get()] = std::min(depth, m_options.maxFlameDepths);
  }
}

uint32_t TimelineWidget::flameDepthFor(ThreadIdx thread) const {
  const uint32_t known = thread.get() < m_doc->flameDepth.size() ? m_doc->flameDepth[thread.get()] : 0;
  return known ? known : kDefaultFlameDepths;
}

void TimelineWidget::refreshRow(const PanelContext& context, const RowLayout& row, RowCache& cache) {
  (void)context;
  DocState& d = *m_doc;
  const analysis::TimelineLod* lod = d.shared->lodIfPrepared();
  const TickRange window = queryWindow(d.view, d.limits);
  const uint32_t columns = std::clamp<uint32_t>(
      static_cast<uint32_t>(m_geometry.plotWidth / static_cast<float>(std::max(m_options.columnPixels, 1u))),
      1u, analysis::kMaxPixelWidth);
  const uint32_t depths = row.expanded ? row.flameDepths : 0;

  const bool sameKey = cache.valid && cache.t0 == window.t0 && cache.t1 == window.t1 &&
                       cache.columns == columns && cache.depths == depths;
  if (sameKey) {
    // A pending result cannot change until the thread becomes ready.
    if (!cache.pending || !lod || !lod->threadReady(row.thread)) return;
  }
  cache.valid = true;
  cache.t0 = window.t0;
  cache.t1 = window.t1;
  cache.columns = columns;
  cache.depths = depths;
  if (!lod) {
    cache.pending = true;
    cache.band.columns.clear();
    cache.strip.columns.clear();
    cache.flame.spans.clear();
    return;
  }
  analysis::StateBandQuery bq;
  bq.thread = row.thread;
  bq.t0 = window.t0;
  bq.t1 = window.t1;
  bq.pixelWidth = columns;
  if (!lod->stateBands(bq, cache.band).ok()) cache.band.columns.clear();
  ++m_stats.bandQueries;
  analysis::SampleStripQuery sq;
  sq.thread = row.thread;
  sq.t0 = window.t0;
  sq.t1 = window.t1;
  sq.pixelWidth = columns;
  if (!lod->sampleStrip(sq, cache.strip).ok()) cache.strip.columns.clear();
  ++m_stats.stripQueries;
  cache.pending = cache.band.pending || cache.strip.pending;
  if (depths > 0) {
    analysis::TimelineQuery tq;
    tq.thread = row.thread;
    tq.depthBegin = 1;
    tq.depthEnd = 1 + depths;
    tq.t0 = window.t0;
    tq.t1 = window.t1;
    tq.pixelWidth = columns;
    if (!lod->timeline(tq, cache.flame).ok()) cache.flame.spans.clear();
    ++m_stats.flameQueries;
    cache.pending = cache.pending || cache.flame.pending;
  } else {
    cache.flame.spans.clear();
  }
}

// ---------------------------------------------------------------------------------------------------------
// Drawing

void TimelineWidget::draw(const PanelContext& context) {
  m_stats = {};
  if (!context.document) {
    if (m_doc) resetDocument();
    ImGui::TextDisabled("No trace open.");
    return;
  }
  if (!m_doc || context.documentGeneration != m_generation || context.document != m_documentPtr) {
    startDocument(context);
  }
  const float ui = std::max(0.5f, ImGui::GetFontSize() / kBaseUi);
  m_rowGeometry = RowGeometry{};
  m_rowGeometry.bandHeight *= ui;
  m_rowGeometry.stripHeight *= ui;
  m_rowGeometry.rowPadding *= ui;
  m_rowGeometry.flameDepthHeight *= ui;
  m_rowGeometry.flameGap *= ui;
  m_rowGeometry.rowSpacing *= ui;

  if (!m_doc->buildStarted && !m_options.deferLodBuild) startBuild(context);
  updateOrder();
  updateFlameDepths();
  m_anyHovered = false;

  drawToolbar(context);
  drawStatusLine(context);

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 base = ImGui::GetCursorScreenPos();
  const float gutter = m_options.gutterWidth * ui;
  m_geometry.gutterX0 = base.x;
  m_geometry.plotX0 = base.x + gutter;
  m_geometry.plotWidth = std::max(40.0f, avail.x - gutter - ImGui::GetStyle().ScrollbarSize);

  drawOverview(context);
  drawHeader(context);
  drawRows(context);
  handleKeys(context);
}

void TimelineWidget::drawToolbar(const PanelContext& context) {
  DocState& d = *m_doc;
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
  if (ImGui::BeginCombo("Sort", sortKeyName(m_options.sortKey))) {
    for (const SortKey k :
         {SortKey::RunningTime, SortKey::SampleCount, SortKey::Name, SortKey::ThreadId, SortKey::Natural}) {
      if (ImGui::Selectable(sortKeyName(k), k == m_options.sortKey)) {
        m_options.sortKey = k;
        d.orderDirty = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Reverse", &m_options.reverseSort)) d.orderDirty = true;
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.5f);
  const char* modeNames[] = {"Module", "Function"};
  int mode = static_cast<int>(m_options.colorMode);
  if (ImGui::Combo("Color by", &mode, modeNames, 2)) {
    m_options.colorMode = static_cast<FrameColorMode>(mode);
    for (RowCache& r : d.rows) r.valid = false;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(true);  // needs inline frame data from the symbol service (WP-20)
  ImGui::Checkbox("Inline frames", &m_options.showInlineFrames);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Inline frames need symbols (not available yet).");
  }
  ImGui::SameLine();
  if (ImGui::Button("Fit")) fit();
  ImGui::SameLine();
  if (ImGui::Button("Expand all")) {
    for (RowFlags& f : d.flags) f.expanded = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Collapse all")) {
    for (RowFlags& f : d.flags) f.expanded = false;
  }
  size_t hidden = 0;
  for (const RowFlags& f : d.flags) hidden += f.hidden ? 1 : 0;
  ImGui::SameLine();
  char label[48];
  std::snprintf(label, sizeof label, "Hidden (%zu)###hidden", hidden);
  if (ImGui::Button(label)) ImGui::OpenPopup("##hiddenthreads");
  if (ImGui::BeginPopup("##hiddenthreads")) {
    if (hidden == 0) ImGui::TextDisabled("No hidden threads");
    for (size_t t = 0; t < d.flags.size(); ++t) {
      if (!d.flags[t].hidden) continue;
      const std::string& name = d.model->threads[t].name;
      char item[160];
      std::snprintf(item, sizeof item, "Show %s (%u)##%zu", name.empty() ? "thread" : name.c_str(),
                    d.model->threads[t].tid, t);
      if (ImGui::MenuItem(item)) {
        d.flags[t].hidden = false;
        d.orderDirty = true;
      }
    }
    if (hidden > 1 && ImGui::MenuItem("Show all")) {
      for (RowFlags& f : d.flags) f.hidden = false;
      d.orderDirty = true;
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Wheel: zoom at cursor   Shift+wheel: pan   Ctrl+wheel: scroll rows\n"
        "Drag: pan   Middle drag: pan   Shift+drag or drag on the ruler: select a range\n"
        "F: apply the selection as time filter   Esc: clear selection and filter\n"
        "Arrows / A D: pan   W S / + -: zoom   Home: fit   Z: zoom to selection\n"
        "Up/Down: previous/next thread   E: expand   P: pin   PgUp/PgDn: scroll\n"
        "Double-click a thread name or right-click for pin / hide / expand");
  }
  (void)context;
}

void TimelineWidget::drawStatusLine(const PanelContext& context) {
  DocState& d = *m_doc;
  const analysis::TimelineLod* lod = d.shared->lodIfPrepared();
  if (d.shared->failed.load(std::memory_order_acquire)) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Timeline index failed: %s", d.shared->error.c_str());
  } else if (!lod || lod->readyThreadCount() < lod->threadCount()) {
    const size_t ready = lod ? lod->readyThreadCount() : 0;
    const size_t total = d.model->threads.size();
    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Building timeline index: %zu / %zu threads (%.0f%%)",
                       ready, total, lod ? static_cast<double>(lod->progress()) * 100.0 : 0.0);
    context.requestRedraw();
  } else {
    const TickRange window = queryWindow(d.view, d.limits);
    ImGui::Text("View %s  (%s)", formatDurationTicks(window.t1 - window.t0, d.clockHz).c_str(),
                formatTimeNs(d.view.start * 1e9 / d.clockHz, 1.0).c_str());
  }
  const SelectionState* sel = context.selection ? &context.selection->state() : nullptr;
  if (m_selection) {
    ImGui::SameLine();
    ImGui::Text("  Selection: %s  [F] apply  [Esc] clear",
                formatDurationTicks(m_selection->t1 - m_selection->t0, d.clockHz).c_str());
  }
  if (sel && sel->timeFilter) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), "  Filter: %s",
                       formatDurationTicks(sel->timeFilter->t1 - sel->timeFilter->t0, d.clockHz).c_str());
  }
  // Frame rate of the whole viewer (the acceptance criterion is 60 fps), right aligned.
  char fps[64];
  const float framerate = ImGui::GetIO().Framerate;
  std::snprintf(fps, sizeof fps, "%.0f fps  %.1f ms", static_cast<double>(framerate),
                1000.0 / std::max(static_cast<double>(framerate), 1.0));
  const float fpsWidth = ImGui::CalcTextSize(fps).x;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - fpsWidth));
  ImGui::TextDisabled("%s", fps);
}

void TimelineWidget::drawRangeOverlays(ImDrawList* dl, const PanelContext& context, float y0, float y1) {
  DocState& d = *m_doc;
  const float x0 = m_geometry.plotX0, w = m_geometry.plotWidth;
  const auto rect = [&](const TickRange& r, ImU32 fill, ImU32 edge) {
    const double a = relTickToX(d.view, toRelative(d.limits, r.t0), x0, w);
    const double b = relTickToX(d.view, toRelative(d.limits, r.t1), x0, w);
    if (b < x0 || a > x0 + w) return;
    const float xa = static_cast<float>(std::max(a, static_cast<double>(x0)));
    const float xb = static_cast<float>(std::min(std::max(b, a + 1.0), static_cast<double>(x0 + w)));
    dl->AddRectFilled(ImVec2(xa, y0), ImVec2(xb, y1), fill);
    if (a >= x0) dl->AddLine(ImVec2(xa, y0), ImVec2(xa, y1), edge);
    if (b <= x0 + w) dl->AddLine(ImVec2(xb, y0), ImVec2(xb, y1), edge);
  };
  if (context.selection && context.selection->state().timeFilter) {
    const ViewFilter& f = *context.selection->state().timeFilter;
    rect({f.t0, f.t1}, IM_COL32(80, 140, 255, 36), IM_COL32(80, 140, 255, 200));
  }
  if (m_selection) rect(*m_selection, IM_COL32(255, 255, 255, 44), IM_COL32(255, 255, 255, 220));
}

void TimelineWidget::drawOverview(const PanelContext& context) {
  DocState& d = *m_doc;
  const float ui = std::max(0.5f, ImGui::GetFontSize() / kBaseUi);
  const float height = 26.0f * ui;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float fullWidth = m_geometry.plotX0 - m_geometry.gutterX0 + m_geometry.plotWidth;
  ImGui::InvisibleButton("##overview", ImVec2(fullWidth, height));
  m_geometry.overviewY0 = p0.y;
  m_geometry.overviewY1 = p0.y + height;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float x0 = m_geometry.plotX0, w = m_geometry.plotWidth;

  dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x0 + w, p0.y + height), IM_COL32(24, 24, 28, 255));
  dl->AddText(ImVec2(p0.x + 4.0f, p0.y + (height - ImGui::GetFontSize()) * 0.5f),
              IM_COL32(170, 170, 175, 255), "Overview");

  // Contents: average Running fraction of the ready threads, recomputed when the width or the number of ready
  // threads changes.
  const analysis::TimelineLod* lod = d.shared->lodIfPrepared();
  const uint32_t cols = std::max<uint32_t>(1, static_cast<uint32_t>(w / 2.0f));
  const size_t ready = lod ? lod->readyThreadCount() : 0;
  if (lod && (d.overviewColumns != cols || d.overviewReady != ready)) {
    d.overviewColumns = cols;
    d.overviewReady = ready;
    d.overview.assign(cols, 0.0f);
    size_t counted = 0;
    analysis::StateBandResult band;
    for (size_t t = 0; t < d.model->threads.size(); ++t) {
      const ThreadIdx th(static_cast<uint32_t>(t));
      if (!lod->threadReady(th)) continue;
      analysis::StateBandQuery q;
      q.thread = th;
      q.t0 = d.limits.traceStart;
      q.t1 = std::max(d.limits.traceEnd, d.limits.traceStart + 1);
      q.pixelWidth = cols;
      if (!lod->stateBands(q, band).ok() || band.pending) continue;
      for (uint32_t c = 0; c < cols && c < band.columns.size(); ++c) d.overview[c] += band.columns[c].running;
      ++counted;
    }
    if (counted > 0) {
      for (float& v : d.overview) v /= static_cast<float>(counted);
    }
  }
  if (!d.overview.empty() && d.overviewColumns == cols) {
    const float colW = w / static_cast<float>(cols);
    for (uint32_t c = 0; c < cols; ++c) {
      const float v = std::min(d.overview[c], 1.0f);
      if (v <= 0.0f) continue;
      const float h = std::max(1.0f, v * (height - 4.0f));
      dl->AddRectFilled(ImVec2(x0 + c * colW, p0.y + height - 2.0f - h),
                        ImVec2(x0 + (c + 1) * colW + 0.5f, p0.y + height - 2.0f), IM_COL32(70, 160, 90, 255));
    }
  }
  // Frame marks are too dense to show here; selection / filter ranges and the viewport indicator are not.
  const auto rangeRect = [&](const TickRange& r, ImU32 fill) {
    const float a = x0 + static_cast<float>(toRelative(d.limits, r.t0) / d.limits.total() * w);
    const float b = x0 + static_cast<float>(toRelative(d.limits, r.t1) / d.limits.total() * w);
    dl->AddRectFilled(ImVec2(a, p0.y), ImVec2(std::max(b, a + 1.0f), p0.y + height), fill);
  };
  if (context.selection && context.selection->state().timeFilter) {
    const ViewFilter& f = *context.selection->state().timeFilter;
    rangeRect({f.t0, f.t1}, IM_COL32(80, 140, 255, 70));
  }
  if (m_selection) rangeRect(*m_selection, IM_COL32(255, 255, 255, 70));
  const OverviewRect vp = overviewViewport(d.view, d.limits, x0, w);
  dl->AddRectFilled(ImVec2(vp.x0, p0.y), ImVec2(vp.x1, p0.y + height), IM_COL32(255, 255, 255, 28));
  dl->AddRect(ImVec2(vp.x0, p0.y), ImVec2(vp.x1, p0.y + height), IM_COL32(255, 255, 255, 200));

  const ImGuiIO& io = ImGui::GetIO();
  if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    d.view = overviewDragTo(d.view, d.limits, io.MousePos.x, x0, w);
  }
  if (ImGui::IsItemHovered()) m_anyHovered = true;
}

void TimelineWidget::drawHeader(const PanelContext& context) {
  DocState& d = *m_doc;
  const float ui = std::max(0.5f, ImGui::GetFontSize() / kBaseUi);
  const float rulerH = std::max(18.0f * ui, ImGui::GetFontSize() + 4.0f);
  const float frameH = std::max(14.0f * ui, ImGui::GetFontSize() + 2.0f);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float fullWidth = m_geometry.plotX0 - m_geometry.gutterX0 + m_geometry.plotWidth;
  ImGui::InvisibleButton("##header", ImVec2(fullWidth, rulerH + frameH),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  m_geometry.headerY0 = p0.y;
  m_geometry.headerY1 = p0.y + rulerH + frameH;
  handleViewInput(context, true, false);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float x0 = m_geometry.plotX0, w = m_geometry.plotWidth;
  const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
  dl->PushClipRect(ImVec2(x0, p0.y), ImVec2(x0 + w, p0.y + rulerH + frameH), true);
  dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x0 + w, p0.y + rulerH), IM_COL32(32, 32, 38, 255));
  dl->AddRectFilled(ImVec2(x0, p0.y + rulerH), ImVec2(x0 + w, p0.y + rulerH + frameH),
                    IM_COL32(26, 26, 31, 255));

  // Ruler
  const std::vector<RulerTick> ticks = buildRuler(d.view, d.clockHz, w, 90.0 * ui);
  for (const RulerTick& t : ticks) {
    const float x = static_cast<float>(relTickToX(d.view, t.relTick, x0, w));
    const float len = t.major ? rulerH * 0.65f : rulerH * 0.3f;
    dl->AddLine(ImVec2(x, p0.y + rulerH - len), ImVec2(x, p0.y + rulerH), IM_COL32(170, 170, 175, 255));
    if (t.major) dl->AddText(ImVec2(x + 3.0f, p0.y + 1.0f), textCol, t.label.c_str());
  }

  // Frame marks: vertical ticks with frame numbers, thinned to at least 3 px apart (labels to fit the text).
  if (m_options.showFrameMarks && !d.frameTicks.empty()) {
    const TickRange window = queryWindow(d.view, d.limits);
    const double pxPerTick = w / d.view.span;
    size_t i = firstFrameTickAtOrAfter(d.frameTicks, window.t0);
    if (i > 0) --i;  // the tick just before the window may have its label inside it
    float lastX = -1e9f, lastLabelEnd = -1e9f;
    for (; i < d.frameTicks.size() && d.frameTicks[i].ts <= window.t1; ++i) {
      const double rel = toRelative(d.limits, d.frameTicks[i].ts);
      const float x = static_cast<float>(relTickToX(d.view, rel, x0, w));
      if (x < lastX + 3.0f) {
        // Skip ahead to the first tick at least 3 px later instead of visiting every tick.
        const uint64_t nextTs = d.frameTicks[i].ts + static_cast<uint64_t>(std::ceil(3.0 / pxPerTick));
        const size_t j = firstFrameTickAtOrAfter(d.frameTicks, nextTs);
        if (j > i + 1) i = j - 1;
        continue;
      }
      lastX = x;
      ++m_stats.frameMarksDrawn;
      dl->AddLine(ImVec2(x, p0.y + rulerH + 2.0f), ImVec2(x, p0.y + rulerH + frameH),
                  IM_COL32(230, 190, 90, 255));
      if (x > lastLabelEnd + 4.0f) {
        char text[24];
        std::snprintf(text, sizeof text, "%llu", static_cast<unsigned long long>(d.frameTicks[i].number));
        const float tw = ImGui::CalcTextSize(text).x;
        if (x + 2.0f + tw < x0 + w) {
          dl->AddText(ImVec2(x + 2.0f, p0.y + rulerH), IM_COL32(230, 190, 90, 255), text);
          lastLabelEnd = x + 2.0f + tw;
        }
      }
    }
  }
  drawRangeOverlays(dl, context, p0.y, p0.y + rulerH + frameH);
  dl->PopClipRect();
  dl->AddLine(ImVec2(m_geometry.gutterX0, p0.y + rulerH + frameH), ImVec2(x0 + w, p0.y + rulerH + frameH),
              IM_COL32(70, 70, 76, 255));
}

void TimelineWidget::drawRows(const PanelContext& context) {
  DocState& d = *m_doc;
  const RowGeometry& geom = m_rowGeometry;
  std::vector<uint32_t> depths =
      d.flameDepth;  // unknown (0) depths use the default until the thread is ready
  for (uint32_t& v : depths) {
    if (v == 0) v = kDefaultFlameDepths;
  }
  d.layout = layoutRows(d.order, d.flags, depths, geom);
  d.contentHeight = contentHeight(d.layout, geom);

  ImGui::BeginChild("##rows", ImVec2(0, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                        ImGuiWindowFlags_NoMove);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float fullWidth = m_geometry.plotX0 - m_geometry.gutterX0 + m_geometry.plotWidth;
  const float viewH = ImGui::GetWindowHeight();
  ImGui::InvisibleButton("##canvas", ImVec2(fullWidth, std::max(d.contentHeight, 1.0f)),
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                             ImGuiButtonFlags_MouseButtonRight);
  ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
  const bool canvasHovered = ImGui::IsItemHovered();
  if (m_pendingScrollY >= 0.0f) {
    ImGui::SetScrollY(m_pendingScrollY);
    m_pendingScrollY = -1.0f;
  }
  const float scrollY = ImGui::GetScrollY();
  d.rowsScrollY = scrollY;
  d.rowsViewHeight = viewH;
  const ImGuiIO& io = ImGui::GetIO();
  m_geometry.rowsContentY0 = origin.y;
  m_geometry.rowsY0 = origin.y + scrollY;
  m_geometry.rowsY1 = origin.y + scrollY + viewH;

  const bool inGutter = io.MousePos.x < m_geometry.plotX0;
  handleViewInput(context, false, inGutter);

  // Right click: row context menu.
  if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const Hit hit = hitTest(context, io.MousePos.x, io.MousePos.y);
    if (hit.thread.valid()) {
      m_contextThread = hit.thread;
      ImGui::OpenPopup("##rowmenu");
    }
  }
  if (ImGui::BeginPopup("##rowmenu")) {
    if (m_contextThread.valid() && m_contextThread.get() < d.flags.size()) {
      RowFlags& f = d.flags[m_contextThread.get()];
      const std::string& name = d.model->threads[m_contextThread.get()].name;
      ImGui::TextDisabled("%s", name.empty() ? "(unnamed thread)" : name.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem(f.expanded ? "Collapse" : "Expand")) f.expanded = !f.expanded;
      if (ImGui::MenuItem(f.pinned ? "Unpin" : "Pin to top")) {
        f.pinned = !f.pinned;
        d.orderDirty = true;
      }
      if (ImGui::MenuItem("Hide")) {
        f.hidden = true;
        d.orderDirty = true;
      }
      if (ImGui::MenuItem("Select") && context.selection)
        context.selection->setSelectedThread(m_contextThread);
    }
    ImGui::EndPopup();
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const RowRange vis = visibleRows(d.layout, scrollY, viewH);
  m_stats.rowsTotal = static_cast<uint32_t>(d.layout.size());
  const ThreadIdx selectedThread =
      context.selection ? context.selection->state().selectedThread : ThreadIdx();
  for (size_t i = vis.first; i < vis.last; ++i) {
    const RowLayout& row = d.layout[i];
    RowCache& cache = d.rows[row.thread.get()];
    refreshRow(context, row, cache);
    const float top = origin.y + row.y0;
    const bool selected = row.thread == selectedThread;
    dl->AddRectFilled(ImVec2(m_geometry.gutterX0, top),
                      ImVec2(m_geometry.plotX0 + m_geometry.plotWidth, top + row.height),
                      selected ? IM_COL32(50, 62, 90, 255)
                               : ((i & 1) ? IM_COL32(30, 30, 35, 255) : IM_COL32(35, 35, 41, 255)));
    drawRowContents(dl, context, i, top, cache);
    ++m_stats.rowsDrawn;
    if (cache.pending) ++m_stats.rowsPending;
  }
  drawRangeOverlays(dl, context, m_geometry.rowsY0, m_geometry.rowsY1);

  // Cursor hairline and tooltip.
  if (canvasHovered && !inGutter && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    const float x = io.MousePos.x;
    dl->AddLine(ImVec2(x, m_geometry.rowsY0), ImVec2(x, m_geometry.rowsY1), IM_COL32(255, 255, 255, 60));
  }
  const Hit hit = canvasHovered ? hitTest(context, io.MousePos.x, io.MousePos.y) : Hit{};
  if (canvasHovered) m_anyHovered = true;
  if (canvasHovered && m_drag.mode == Drag::Mode::None && !ImGui::IsPopupOpen("##rowmenu"))
    drawTooltip(context, hit);
  publishHover(context, hit);
  ImGui::EndChild();
}
namespace {

std::optional<FunctionKey> functionKeyOf(const model::TraceModel& m, CallstackIdx node) {
  if (!node.valid() || node.get() >= m.nodes.size()) return std::nullopt;
  const FrameIdx fi = m.nodes[node.get()].frame;
  if (!fi.valid() || fi.get() >= m.frames.size()) return std::nullopt;
  // Without symbols (WP-20) the function start is unknown; the frame address stands in for it.
  const model::Frame& f = m.frames[fi.get()];
  FunctionKey key;
  key.module = f.module;
  key.functionStartRvaOrString = f.addr_or_rva;
  return key;
}

void drawPendingLook(ImDrawList* dl, float x0, float y0, float x1, float y1) {
  dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(44, 44, 52, 255));
  dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
  const float h = y1 - y0;
  const float phase = std::fmod(static_cast<float>(ImGui::GetTime()) * 20.0f, 16.0f);
  for (float x = x0 - h + phase; x < x1; x += 16.0f) {
    dl->AddLine(ImVec2(x, y1), ImVec2(x + h, y0), IM_COL32(70, 70, 82, 255));
  }
  dl->PopClipRect();
}

}  // namespace

void TimelineWidget::drawRowContents(ImDrawList* dl, const PanelContext& context, size_t layoutIndex,
                                     float rowTop, RowCache& cache) {
  DocState& d = *m_doc;
  const RowGeometry& geom = m_rowGeometry;
  const RowLayout& row = d.layout[layoutIndex];
  const model::TraceModel& model = *d.model;
  const float x0 = m_geometry.plotX0, w = m_geometry.plotWidth;
  const float bandY0 = rowTop + bandTop(geom), bandY1 = bandY0 + geom.bandHeight;
  const float stripY0 = rowTop + stripTop(geom), stripY1 = stripY0 + geom.stripHeight;

  // Gutter label: expand arrow, pin marker, name and thread id.
  {
    const model::Thread& th = model.threads[row.thread.get()];
    const RowFlags flags = d.flags[row.thread.get()];
    char label[200];
    std::snprintf(label, sizeof label, "%s%s%s (%u)", row.expanded ? "v " : "> ", flags.pinned ? "* " : "",
                  th.name.empty() ? "thread" : th.name.c_str(), th.tid);
    const ImVec4 clip(m_geometry.gutterX0, rowTop, x0 - 4.0f, rowTop + row.height);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(m_geometry.gutterX0 + 4.0f, rowTop + geom.rowPadding),
                ImGui::GetColorU32(ImGuiCol_Text), label, nullptr, 0.0f, &clip);
  }

  dl->PushClipRect(ImVec2(x0, rowTop), ImVec2(x0 + w, rowTop + row.height), true);
  if (cache.pending || cache.band.columns.empty()) {
    drawPendingLook(dl, x0, bandY0, x0 + w, bandY1);
    drawPendingLook(dl, x0, stripY0, x0 + w, stripY1);
  } else {
    // State band: merge neighbouring columns of equal colour into one rectangle.
    {
      const uint32_t cols = static_cast<uint32_t>(cache.band.columns.size());
      const float colW = w / static_cast<float>(cols);
      RectBatch batch(dl, cols);
      uint32_t runStart = 0;
      Color32 runColor = bandColumnColor(cache.band.columns[0]);
      for (uint32_t c = 1; c <= cols; ++c) {
        const Color32 col = c < cols ? bandColumnColor(cache.band.columns[c]) : 0;
        if (c == cols || col != runColor) {
          batch.add(x0 + static_cast<float>(runStart) * colW, bandY0, x0 + static_cast<float>(c) * colW,
                    bandY1, toIm(runColor));
          ++m_stats.rects;
          runStart = c;
          runColor = col;
        }
      }
    }
    // Sample strip.
    const uint32_t scols = static_cast<uint32_t>(cache.strip.columns.size());
    if (scols > 0) {
      const float sw = w / static_cast<float>(scols);
      RectBatch batch(dl, scols);
      uint32_t start = 0;
      Color32 cur = stripColumnColor(model, cache.strip.columns[0], m_options.colorMode);
      for (uint32_t c = 1; c <= scols; ++c) {
        const Color32 col =
            c < scols ? stripColumnColor(model, cache.strip.columns[c], m_options.colorMode) : 0;
        if (c == scols || col != cur) {
          if (colorA(cur) != 0) {
            batch.add(x0 + static_cast<float>(start) * sw, stripY0, x0 + static_cast<float>(c) * sw, stripY1,
                      toIm(cur));
            ++m_stats.rects;
          }
          start = c;
          cur = col;
        }
      }
    }
  }
  if (row.expanded) drawFlame(dl, context, row, rowTop, cache);
  dl->PopClipRect();
}

void TimelineWidget::drawFlame(ImDrawList* dl, const PanelContext& context, const RowLayout& row,
                               float rowTop, const RowCache& cache) {
  DocState& d = *m_doc;
  const RowGeometry& geom = m_rowGeometry;
  const model::TraceModel& model = *d.model;
  const float x0 = m_geometry.plotX0, w = m_geometry.plotWidth;
  if (cache.pending) {
    const float top = rowTop + flameTop(geom);
    drawPendingLook(dl, x0, top, x0 + w, top + static_cast<float>(row.flameDepths) * geom.flameDepthHeight);
    return;
  }
  std::optional<FunctionKey> selected;
  if (context.selection) selected = context.selection->state().selectedFunction;
  const float fontSize = ImGui::GetFontSize();
  ImFont* font = ImGui::GetFont();
  // Pass 1: all rectangles in one batch. Spans that need decoration (labels, selection outline) are noted.
  struct Decor {
    float xa, ya, xb, yb;
    CallstackIdx node;
    Color32 color;
    bool outline, label;
  };
  std::vector<Decor> decor;
  {
    RectBatch batch(dl, cache.flame.spans.size());
    for (const analysis::TimelineSpan& s : cache.flame.spans) {
      const double a = relTickToX(d.view, toRelative(d.limits, s.t0), x0, w);
      const double b = relTickToX(d.view, toRelative(d.limits, s.t1), x0, w);
      const float xa = static_cast<float>(std::max(a, static_cast<double>(x0)));
      float xb = static_cast<float>(std::min(b, static_cast<double>(x0 + w)));
      if (xb <= xa) xb = xa + 1.0f;
      const float ya = rowTop + flameDepthTop(geom, s.depth);
      const float yb = ya + geom.flameDepthHeight - 1.0f;
      const bool merged = !s.node.valid();
      const Color32 color = merged ? kOtherWaitColor : callstackColor(model, s.node, m_options.colorMode);
      batch.add(xa, ya, xb, yb, toIm(merged ? scaleBrightness(color, 0.6f) : color));
      ++m_stats.rects;
      ++m_stats.flameSpans;
      const bool outline = selected && !merged && functionKeyOf(model, s.node) == selected;
      const bool label = !merged && xb - xa > fontSize * 4.0f;
      if (outline || label) decor.push_back({xa, ya, xb, yb, s.node, color, outline, label});
    }
  }
  // Pass 2: outlines and labels.
  uint32_t labels = 0;
  for (const Decor& dc : decor) {
    if (dc.outline) dl->AddRect(ImVec2(dc.xa, dc.ya), ImVec2(dc.xb, dc.yb), IM_COL32(255, 255, 255, 255));
    if (dc.label && labels < 48) {
      const std::string text = describeCallstackLeaf(model, dc.node);
      const ImVec4 clip(dc.xa, dc.ya, dc.xb, dc.yb);
      dl->AddText(font, fontSize, ImVec2(dc.xa + 2.0f, dc.ya), toIm(textColorFor(dc.color)), text.c_str(),
                  nullptr, 0.0f, &clip);
      ++labels;
    }
  }
}

// ---------------------------------------------------------------------------------------------------------
// Hit testing, hover, tooltip, click

TimelineWidget::Hit TimelineWidget::hitTest(const PanelContext& context, float mx, float my) const {
  (void)context;
  Hit h;
  if (!m_doc) return h;
  const DocState& d = *m_doc;
  const float cy = my - m_geometry.rowsContentY0;
  const size_t i = rowAt(d.layout, m_rowGeometry, cy);
  if (i == kNoRow) return h;
  const RowLayout& row = d.layout[i];
  if (cy >= row.y0 + row.height) return h;  // row spacing
  h.layoutIndex = i;
  h.thread = row.thread;
  h.inGutter = mx < m_geometry.plotX0;
  h.inPlot = mx >= m_geometry.plotX0 && mx < m_geometry.plotX0 + m_geometry.plotWidth;
  const RowHit rh = hitRowSection(row, m_rowGeometry, cy - row.y0);
  h.section = rh.section;
  h.depth = rh.depth;
  if (!h.inPlot) return h;
  h.ts = toAbsolute(d.limits, xToRelTick(d.view, mx, m_geometry.plotX0, m_geometry.plotWidth));
  const RowCache& cache = d.rows[row.thread.get()];
  h.pending = !cache.valid || cache.pending;
  if (!cache.valid || cache.pending) return h;
  const uint32_t col = columnForTick(cache.t0, cache.t1, cache.columns, h.ts);
  if (col < cache.strip.columns.size()) {
    h.stack = cache.strip.columns[col].stack;
    h.share = cache.strip.columns[col].share;
  }
  if (h.section == RowSection::Flame) {
    h.span = findSpanAt(cache.flame.spans, h.depth, h.ts);
    if (h.span && h.span->node.valid()) h.stack = h.span->node;
  }
  return h;
}

void TimelineWidget::drawTooltip(const PanelContext& context, const Hit& hit) {
  (void)context;
  if (!m_doc || hit.layoutIndex == kNoRow || !hit.thread.valid()) return;
  const DocState& d = *m_doc;
  const model::TraceModel& model = *d.model;
  const model::Thread& th = model.threads[hit.thread.get()];
  if (hit.inGutter) {
    ImGui::BeginTooltip();
    ImGui::Text("%s (tid %u)", th.name.empty() ? "(unnamed)" : th.name.c_str(), th.tid);
    const size_t samples = hit.thread.get() < model.samplesByThread.size()
                               ? model.samplesByThread[hit.thread.get()].ts.size()
                               : 0;
    ImGui::Text("%zu samples", samples);
    if (const auto* sums = d.shared->summariesIfReady()) {
      ImGui::Text("Running %s",
                  formatDurationTicks((*sums)[hit.thread.get()].runningTicks, d.clockHz).c_str());
    }
    ImGui::TextDisabled("Double-click: expand   Right-click: menu");
    ImGui::EndTooltip();
    return;
  }
  if (!hit.inPlot) return;
  const double nsPerPixel = d.view.span / std::max(1.0f, m_geometry.plotWidth) * 1e9 / d.clockHz;
  ImGui::BeginTooltip();
  ImGui::Text("%s (tid %u)", th.name.empty() ? "(unnamed)" : th.name.c_str(), th.tid);
  ImGui::Text(
      "Time: %s",
      formatTimeNs(toRelative(d.limits, hit.ts) * 1e9 / d.clockHz, std::max(nsPerPixel, 1.0)).c_str());
  if (const model::ThreadStateInterval* s = analysis::stateAt(model, hit.thread, hit.ts)) {
    switch (s->state) {
      case model::ThreadState::Running: ImGui::Text("State: Running (cpu %u)", s->cpu); break;
      case model::ThreadState::Ready: ImGui::Text("State: Ready (runnable, waiting for a cpu)"); break;
      case model::ThreadState::Waiting: {
        const std::string_view reason = analysis::waitReasonName(d.platform, s->wait_reason);
        const std::string_view cat =
            analysis::waitCategoryName(analysis::waitReasonCategory(d.platform, s->wait_reason));
        ImGui::Text("State: Waiting - %.*s (%.*s)", static_cast<int>(reason.size()), reason.data(),
                    static_cast<int>(cat.size()), cat.data());
        break;
      }
      case model::ThreadState::Unknown: ImGui::Text("State: Unknown"); break;
    }
    ImGui::Text("Interval: %s", formatDurationTicks(s->end - s->start, d.clockHz).c_str());
  }
  if (hit.pending) {
    ImGui::TextDisabled("Timeline index still building...");
  } else if (hit.span) {
    const std::string fn = hit.span->node.valid() ? describeCallstackLeaf(model, hit.span->node) : "(merged)";
    ImGui::Text("Depth %u: %s", hit.span->depth, fn.c_str());
    ImGui::Text("Span: %s", formatDurationTicks(hit.span->t1 - hit.span->t0, d.clockHz).c_str());
    if (hit.span->mergedCount > 1) ImGui::Text("%u samples merged", hit.span->mergedCount);
  } else if (hit.stack.valid()) {
    ImGui::Text("Top function: %s (%.0f%% of samples here)", describeCallstackLeaf(model, hit.stack).c_str(),
                static_cast<double>(hit.share) * 100.0);
  } else {
    ImGui::TextDisabled("No samples");
  }
  ImGui::EndTooltip();
}

void TimelineWidget::publishHover(const PanelContext& context, const Hit& hit) {
  if (!context.selection || !m_doc) return;
  if (hit.inPlot && hit.thread.valid()) {
    HoveredItem item;
    item.kind = hit.stack.valid() ? HoveredItem::Kind::Callstack : HoveredItem::Kind::Time;
    item.thread = hit.thread;
    item.callstack = hit.stack;
    item.function = functionKeyOf(*m_doc->model, hit.stack);
    item.ts = hit.ts;
    context.selection->setHovered(item);
    m_hoverPublished = true;
  } else if (m_hoverPublished) {
    context.selection->setHovered(HoveredItem{});
    m_hoverPublished = false;
  }
}

void TimelineWidget::applyClick(const PanelContext& context, const Hit& hit) {
  if (!context.selection || !m_doc || !hit.thread.valid()) return;
  context.selection->setSelectedThread(hit.thread);
  if (hit.inPlot && hit.stack.valid()) {
    context.selection->setSelectedFunction(functionKeyOf(*m_doc->model, hit.stack));
  }
}

// ---------------------------------------------------------------------------------------------------------
// Input

void TimelineWidget::zoomAround(double cursorFrac, double factor) {
  DocState& d = *m_doc;
  d.view = zoomAtCursor(d.view, d.limits, cursorFrac, factor);
}

// Called right after the InvisibleButton of the header or the rows canvas.
void TimelineWidget::handleViewInput(const PanelContext& context, bool header, bool inGutter) {
  DocState& d = *m_doc;
  const ImGuiIO& io = ImGui::GetIO();
  const float x0 = m_geometry.plotX0, w = m_geometry.plotWidth;
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) m_anyHovered = true;

  if (hovered && !inGutter) {
    const float wheelH = io.MouseWheelH + (io.KeyShift ? io.MouseWheel : 0.0f);
    if (wheelH != 0.0f) {
      d.view = panTicks(d.view, d.limits, -static_cast<double>(wheelH) * 0.1 * d.view.span);
    } else if (io.MouseWheel != 0.0f) {
      if (!header && io.KeyCtrl) {
        ImGui::SetScrollY(ImGui::GetScrollY() -
                          io.MouseWheel * 3.0f * (m_rowGeometry.bandHeight + m_rowGeometry.stripHeight));
      } else {
        const double frac = std::clamp(static_cast<double>((io.MousePos.x - x0) / w), 0.0, 1.0);
        zoomAround(frac, std::pow(1.25, static_cast<double>(io.MouseWheel)));
      }
    }
  }

  if (ImGui::IsItemActivated()) {
    m_drag = {};
    m_drag.startX = io.MousePos.x;
    m_drag.fromGutter = !header && inGutter;
    m_drag.fromHeader = header;
    if (!m_drag.fromGutter) {
      if (io.MouseDown[ImGuiMouseButton_Middle]) {
        m_drag.mode = Drag::Mode::Pan;
      } else if (io.MouseDown[ImGuiMouseButton_Left]) {
        m_drag.mode = (header || io.KeyShift) ? Drag::Mode::Select : Drag::Mode::Pan;
      }
    }
    if (m_drag.fromGutter && io.MouseDown[ImGuiMouseButton_Left] &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      const Hit hit = hitTest(context, io.MousePos.x, io.MousePos.y);
      if (hit.thread.valid()) setExpanded(hit.thread, !flagsOf(hit.thread).expanded);
    }
  }
  if (ImGui::IsItemActive()) {
    if (m_drag.mode == Drag::Mode::Pan) {
      if (io.MouseDelta.x != 0.0f) d.view = panPixels(d.view, d.limits, io.MouseDelta.x, w);
      if (std::fabs(io.MousePos.x - m_drag.startX) > 3.0f) m_drag.moved = true;
    } else if (m_drag.mode == Drag::Mode::Select) {
      const auto r = dragRange(d.view, d.limits, m_drag.startX, io.MousePos.x, x0, w);
      if (r) {
        m_selection = r;
        m_drag.moved = true;
      }
    }
  }
  if (ImGui::IsItemDeactivated()) {
    const bool click = !m_drag.moved;
    if (click && !header && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const Hit hit = hitTest(context, io.MousePos.x, io.MousePos.y);
      applyClick(context, hit);
    }
    m_drag = {};
  }
}

void TimelineWidget::handleKeys(const PanelContext& context) {
  if (!m_doc) return;
  DocState& d = *m_doc;
  const ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;
  if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !m_anyHovered) return;
  if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
  const auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  const double pan = d.view.span * 0.1 * (io.KeyShift ? 5.0 : 1.0);
  if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_A)) d.view = panTicks(d.view, d.limits, -pan);
  if (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_D)) d.view = panTicks(d.view, d.limits, pan);
  double zoomCenter = 0.5;
  if (m_anyHovered && io.MousePos.x >= m_geometry.plotX0 &&
      io.MousePos.x < m_geometry.plotX0 + m_geometry.plotWidth) {
    zoomCenter = (io.MousePos.x - m_geometry.plotX0) / m_geometry.plotWidth;
  }
  if (pressed(ImGuiKey_W) || pressed(ImGuiKey_Equal) || pressed(ImGuiKey_KeypadAdd))
    zoomAround(zoomCenter, 1.25);
  if (pressed(ImGuiKey_S) || pressed(ImGuiKey_Minus) || pressed(ImGuiKey_KeypadSubtract)) {
    zoomAround(zoomCenter, 0.8);
  }
  if (pressed(ImGuiKey_Home)) fit();
  if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
    if (m_selection) {
      d.view = viewFromRange(d.limits, *m_selection);
    } else if (context.selection && context.selection->state().timeFilter) {
      const ViewFilter& f = *context.selection->state().timeFilter;
      d.view = viewFromRange(d.limits, {f.t0, f.t1});
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_F, false) && context.selection) applySelectionAsFilter(*context.selection);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) clearSelection(context.selection);

  const float page = std::max(d.rowsViewHeight * 0.9f, 20.0f);
  const float maxScroll = std::max(d.contentHeight - d.rowsViewHeight, 0.0f);
  if (pressed(ImGuiKey_PageDown)) m_pendingScrollY = std::clamp(d.rowsScrollY + page, 0.0f, maxScroll);
  if (pressed(ImGuiKey_PageUp)) m_pendingScrollY = std::max(d.rowsScrollY - page, 0.0f);

  if (context.selection && !d.order.empty()) {
    const ThreadIdx sel = context.selection->state().selectedThread;
    const auto it = std::find(d.order.begin(), d.order.end(), sel);
    const long index = it == d.order.end() ? -1 : static_cast<long>(it - d.order.begin());
    long next = index;
    if (pressed(ImGuiKey_DownArrow)) next = std::min<long>(index + 1, static_cast<long>(d.order.size()) - 1);
    if (pressed(ImGuiKey_UpArrow)) next = std::max<long>(index < 0 ? 0 : index - 1, 0);
    if (next != index) {
      context.selection->setSelectedThread(d.order[static_cast<size_t>(next)]);
      if (static_cast<size_t>(next) < d.layout.size()) {
        const RowLayout& r = d.layout[static_cast<size_t>(next)];
        if (r.y0 < d.rowsScrollY) m_pendingScrollY = r.y0;
        if (r.y0 + r.height > d.rowsScrollY + d.rowsViewHeight) {
          m_pendingScrollY = r.y0 + r.height - d.rowsViewHeight;
        }
      }
    }
    if (sel.valid() && ImGui::IsKeyPressed(ImGuiKey_E, false)) setExpanded(sel, !flagsOf(sel).expanded);
    if (sel.valid() && ImGui::IsKeyPressed(ImGuiKey_P, false)) setPinned(sel, !flagsOf(sel).pinned);
  }
}

}  // namespace tachyon::viewer::timeline
