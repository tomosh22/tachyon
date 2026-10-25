#include "viewer/callgraph/callgraph_panel.h"

#include "viewer/builtin_panels.h"
#include "viewer/callgraph/subtree_resolver.h"
#include "viewer/common/function_names.h"
#include "viewer/common/imgui_util.h"
#include "viewer/document.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tachyon::viewer {

namespace cg = callgraph;
using analysis::NodeAgg;
using common::drawSpinner;
using common::textRight;
using symbols::FunctionKey;

namespace {

constexpr auto kThrottleWhileResolving = std::chrono::milliseconds(200);
constexpr uint32_t kMaxIndentLevels =
    48;  // deeper rows share the last indent (a 100 k deep chain stays on screen)

bool isPseudoKey(const FunctionKey& k) {
  return k == analysis::kRootKey || k == analysis::kKernelKey;
}

ImU32 sliceColor(size_t i, bool other) {
  if (other) return IM_COL32(128, 128, 128, 255);
  return ImColor::HSV(static_cast<float>(i % 10) / 10.0f, 0.55f, 0.85f);
}

}  // namespace

CallGraphPanel::CallGraphPanel() {
  cg::collapseAll(m_expanded);
}

CallGraphPanel::~CallGraphPanel() = default;

// ---------------------------------------------------------------------------------------------------------
// Lifecycle and bus

void CallGraphPanel::resetDocumentState() {
  m_query.cancel();
  m_pendingSnapshot.reset();
  m_shownSnapshot.reset();
  m_tree.reset();
  m_rows.clear();
  m_pie.clear();
  m_names.clear();
  cg::collapseAll(m_expanded);
  m_selectedId.reset();
  m_selectedPath.clear();
  m_selectedKey.reset();
  m_scrollTo.reset();
  m_externalKey.reset();
  m_menuTarget.reset();
  m_openMenu = false;
  m_haveRequested = false;
  m_requested = {};
  m_shown = {};
  m_firstTree = true;
  m_truncated = false;
  m_message.clear();
  m_subtreeLabel.clear();
  m_subtreeLabelNode = CallstackIdx();
  m_drawnRows.clear();
  m_haveDocument = false;
}

void CallGraphPanel::onDocumentChanged(const Document*) {
  resetDocumentState();  // the in-flight query is cancelled; a late result is dropped as stale
}

void CallGraphPanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                        SelectionField field) {
  if (field == SelectionField::PanelRequest && state.panelRequest.name == kName) {
    m_focusPending = true;
    ++m_focusRequests;
  }
  if (field == SelectionField::SelectedFunction || field == SelectionField::All) {
    if (state.selectedFunction != m_selectedKey) {
      m_externalKey = state.selectedFunction;
      if (!state.selectedFunction) {
        m_selectedId.reset();
        m_selectedPath.clear();
        m_selectedKey.reset();
        rebuildPie();
      }
    }
    context.requestRedraw();
  }
  if (field == SelectionField::TimeFilter || field == SelectionField::All) context.requestRedraw();
}

CallGraphPanel::QuerySpec CallGraphPanel::currentSpec(const PanelContext& context) const {
  QuerySpec spec;
  spec.filter = cg::toQueryFilter(context.selection ? context.selection->state().timeFilter : std::nullopt);
  spec.expandInline = m_inline;
  spec.kernelFrames = m_kernel;
  spec.documentGeneration = context.documentGeneration;
  spec.symbolGeneration = context.symbolGeneration;
  return spec;
}

void CallGraphPanel::issue(const PanelContext& context, const QuerySpec& spec) {
  analysis::AggregationOptions options;
  options.expandInline = spec.expandInline;
  options.kernelFrames = spec.kernelFrames;
  const Document& doc = *context.document;
  std::shared_ptr<const AnalysisSnapshot> snapshot =
      doc.analysis().snapshot(options, spec.symbolGeneration, context.queryExecutor);
  if (!snapshot) {
    m_message = "Analysis is not available for this document.";
    return;
  }
  m_requested = spec;
  m_haveRequested = true;
  m_lastIssue = Clock::now();
  m_pendingSnapshot = snapshot;
  const model::TraceModel& model = doc.model();
  m_pendingClockHz =
      model.header.clock_frequency_hz ? static_cast<double>(model.header.clock_frequency_hz) : 1e9;
  analysis::QueryExecutor& executor = *context.queryExecutor;
  const analysis::Filter filter = spec.filter;
  using Value = std::shared_ptr<const NodeAgg>;
  m_query.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [filter](const analysis::QueryContext& qc, analysis::Cancel cancel) -> Result<Value> {
          return analysis::topDownTreeShared(qc, filter, cancel);
        });
  });
}

void CallGraphPanel::update(const PanelContext& context) {
  if (!context.document) {
    if (m_haveDocument || m_tree) resetDocumentState();
    return;
  }
  if (!m_haveDocument || context.documentGeneration != m_documentGeneration) {
    resetDocumentState();
    m_haveDocument = true;
    m_documentGeneration = context.documentGeneration;
  }
  if (!context.queryExecutor || !context.selection) return;

  if (auto result = m_query.poll()) {
    if (result->ok()) {
      const bool reset = m_firstTree || m_shown.filter.subtreeRoot != m_requested.filter.subtreeRoot ||
                         m_shown.expandInline != m_requested.expandInline ||
                         m_shown.kernelFrames != m_requested.kernelFrames ||
                         m_shown.symbolGeneration != m_requested.symbolGeneration;  // keys changed: ids too
      m_shown = m_requested;
      m_message.clear();
      applyTree(result->value(), m_pendingSnapshot, m_pendingClockHz, reset);
    } else {
      m_message = "Query failed: " + result->error().toString();
    }
    m_pendingSnapshot.reset();
  }

  const QuerySpec spec = currentSpec(context);
  if (!m_haveRequested || !(spec == m_requested)) {
    bool defer = false;
    if (m_throttle && m_haveRequested && context.symbols) {
      QuerySpec same = spec;
      same.symbolGeneration = m_requested.symbolGeneration;
      const symbols::SymbolProgress progress = context.symbols->progress();
      if (same == m_requested && progress.done < progress.total &&
          Clock::now() - m_lastIssue < kThrottleWhileResolving)
        defer = true;  // a burst of module events: re-query once it settles
    }
    if (defer) {
      context.requestRedraw();
    } else {
      issue(context, spec);
    }
  }
  if (m_query.pending()) context.requestRedraw();  // keep polling (and spinning) until the result arrives
  if (m_externalKey && m_tree) resolveExternalSelection();
}

// ---------------------------------------------------------------------------------------------------------
// Tree state

void CallGraphPanel::applyTree(std::shared_ptr<const NodeAgg> tree,
                               std::shared_ptr<const AnalysisSnapshot> snapshot, double clockHz,
                               bool resetExpansion) {
  m_tree = std::move(tree);
  m_shownSnapshot = std::move(snapshot);
  m_clockHz = clockHz;
  m_names.clear();
  ++m_delivered;
  if (resetExpansion || m_firstTree) {
    cg::collapseAll(m_expanded);
    if (m_tree) cg::expandHotPath(*m_tree, cg::kRootId, m_expanded);
    m_firstTree = false;
    m_scrollTo.reset();
  }
  // Keep the selection if the node still exists; otherwise look the function up again.
  if (m_selectedId && m_tree) {
    if (const auto at = cg::findByPath(*m_tree, m_selectedPath)) {
      m_selectedId = at->id;
    } else {
      m_selectedId.reset();
      if (m_selectedKey) m_externalKey = m_selectedKey;
    }
  }
  rebuildRows();
  rebuildPie();
}

void CallGraphPanel::injectTree(std::shared_ptr<const NodeAgg> tree,
                                std::shared_ptr<const AnalysisSnapshot> snapshot, double clockHz,
                                bool resetExpansion) {
  applyTree(std::move(tree), std::move(snapshot), clockHz, resetExpansion);
}

void CallGraphPanel::rebuildRows() {
  if (!m_tree) {
    m_rows.clear();
    m_truncated = false;
    return;
  }
  m_truncated = cg::flattenVisible(*m_tree, m_expanded, cg::kMaxVisibleRows, m_rows);
}

void CallGraphPanel::rebuildPie() {
  m_pie.clear();
  m_pieTitle.clear();
  if (!m_tree) return;
  const NodeAgg* node = m_tree.get();
  if (m_selectedId && !m_selectedPath.empty()) {
    if (const auto at = cg::findByPath(*m_tree, m_selectedPath)) node = at->node;
  }
  m_pie = cg::pieSlices(*node);
  m_pieTitle = nameOf(node->key).function;
}

size_t CallGraphPanel::rowOfId(cg::NodeId id) const {
  for (size_t i = 0; i < m_rows.size(); ++i)
    if (m_rows[i].id == id) return i;
  return SIZE_MAX;
}

void CallGraphPanel::selectLocated(const cg::Located& at) {
  for (const cg::NodeId a : at.ancestors) m_expanded.insert(a);
  m_selectedId = at.id;
  m_selectedPath = at.path;
  m_selectedKey = at.node->key;
  m_scrollTo = at.id;
  rebuildRows();
  rebuildPie();
}

void CallGraphPanel::resolveExternalSelection() {
  const FunctionKey key = *m_externalKey;
  m_externalKey.reset();
  if (isPseudoKey(key)) return;
  if (const auto at = cg::findFirstByKey(*m_tree, key)) {
    selectLocated(*at);
  } else {
    m_selectedId.reset();
    m_selectedPath.clear();
    m_selectedKey = key;  // remembered, so the same bus value is not searched for again
    rebuildPie();
  }
}

const CallGraphPanel::NameParts& CallGraphPanel::nameOf(const FunctionKey& key) {
  const auto it = m_names.find(key);
  if (it != m_names.end()) return it->second;
  NameParts parts = common::splitFunctionName(m_shownSnapshot ? &m_shownSnapshot->context() : nullptr, key);
  return m_names.emplace(key, std::move(parts)).first->second;
}

// ---------------------------------------------------------------------------------------------------------
// Actions

void CallGraphPanel::expandHotPath() {
  if (!m_tree) return;
  const NodeAgg* from = m_tree.get();
  cg::NodeId id = cg::kRootId;
  m_expanded.insert(cg::kRootId);
  if (m_selectedId) {
    if (const auto at = cg::findByPath(*m_tree, m_selectedPath)) {
      for (const cg::NodeId a : at->ancestors) m_expanded.insert(a);
      from = at->node;
      id = at->id;
    }
  }
  const cg::HotPathEnd end = cg::expandHotPath(*from, id, m_expanded);
  m_scrollTo = end.id;
  rebuildRows();
}

bool CallGraphPanel::expandAllRows() {
  if (!m_tree) return true;
  const bool cut = cg::expandAll(*m_tree, m_expanded, cg::kMaxVisibleRows);
  rebuildRows();
  return !cut && !m_truncated;
}

void CallGraphPanel::collapseAllRows() {
  cg::collapseAll(m_expanded);
  rebuildRows();
}

void CallGraphPanel::toggleExpanded(size_t row) {
  if (row >= m_rows.size() || !m_rows[row].hasChildren) return;
  const cg::NodeId id = m_rows[row].id;
  if (m_expanded.count(id)) {
    m_expanded.erase(id);
  } else {
    m_expanded.insert(id);
  }
  rebuildRows();
}

std::optional<CallGraphPanel::ActionTarget> CallGraphPanel::targetOfRow(size_t row) const {
  if (row >= m_rows.size()) return std::nullopt;
  ActionTarget t;
  t.path = cg::pathOfRow(m_rows, row);
  t.key = m_rows[row].node->key;
  t.isRoot = m_rows[row].parentRow == UINT32_MAX;
  // const_cast: the name cache is a pure memo.
  t.fullName = const_cast<CallGraphPanel*>(this)->nameOf(t.key).full;
  return t;
}

void CallGraphPanel::selectRow(const PanelContext& context, size_t row) {
  if (row >= m_rows.size()) return;
  m_selectedId = m_rows[row].id;
  m_selectedPath = cg::pathOfRow(m_rows, row);
  const FunctionKey key = m_rows[row].node->key;
  const bool root = m_rows[row].parentRow == UINT32_MAX;
  rebuildPie();
  if (root || isPseudoKey(key)) {
    m_selectedKey.reset();
    return;
  }
  m_selectedKey = key;  // before publishing: our own onSelectionChanged must see "nothing new"
  if (context.selection) context.selection->setSelectedFunction(key);
}

bool CallGraphPanel::filterToSubtree(const PanelContext& context, const ActionTarget& target) {
  if (target.isRoot || !context.document || !context.selection || !m_shownSnapshot) return false;
  const std::optional<ViewFilter> current = context.selection->state().timeFilter;
  const CallstackIdx start = current && current->subtreeRoot ? *current->subtreeRoot : CallstackIdx();
  const std::shared_ptr<const NodeChildIndex> children = context.document->analysis().childIndex();
  const std::optional<CallstackIdx> node =
      cg::resolveNodePath(m_shownSnapshot->context(), *children, start, target.path);
  if (!node) {
    m_message = "Cannot map this call graph node back to a callstack node (symbols changed?).";
    return false;
  }
  ViewFilter filter;
  if (current) {
    filter = *current;
  } else {
    const model::TraceModel& m = context.document->model();
    filter.t0 = m.header.start_ts;
    filter.t1 = std::max(m.header.end_ts, m.header.start_ts) + 1;  // half-open: keeps a sample at end_ts
  }
  filter.subtreeRoot = *node;
  m_subtreeLabel = target.fullName;
  m_subtreeLabelNode = *node;
  m_message.clear();
  context.selection->setTimeFilter(filter);
  return true;
}

void CallGraphPanel::clearSubtreeFilter(const PanelContext& context) {
  if (!context.selection || !context.document) return;
  const std::optional<ViewFilter>& current = context.selection->state().timeFilter;
  if (!current || !current->subtreeRoot) return;
  ViewFilter filter = *current;
  filter.subtreeRoot.reset();
  const model::TraceModel& m = context.document->model();
  const bool wholeRange = filter.t0 <= m.header.start_ts && filter.t1 > m.header.end_ts;
  if (wholeRange && !filter.threads && !filter.function) {
    context.selection->setTimeFilter(std::nullopt);
  } else {
    context.selection->setTimeFilter(filter);
  }
  m_subtreeLabel.clear();
  m_subtreeLabelNode = CallstackIdx();
}

void CallGraphPanel::showInFunctionList(const PanelContext& context, const ActionTarget& target) {
  if (!context.selection || target.isRoot) return;
  if (!isPseudoKey(target.key)) {
    m_selectedKey = target.key;
    context.selection->setSelectedFunction(target.key);
  }
  context.selection->requestPanel(kFunctionListPanel);
}

void CallGraphPanel::showSource(const PanelContext& context, const ActionTarget& target) {
  if (!context.selection || target.isRoot) return;
  if (!isPseudoKey(target.key)) {
    m_selectedKey = target.key;
    context.selection->setSelectedFunction(target.key);
  }
  context.selection->requestPanel(kSourcePanel);
}

void CallGraphPanel::copyName(const ActionTarget& target) {
  ImGui::SetClipboardText(target.fullName.c_str());
}

void CallGraphPanel::setInlineExpansion(bool on) {
  m_inline = on;
}

void CallGraphPanel::setKernelFrames(analysis::KernelFrames mode) {
  m_kernel = mode;
}

// ---------------------------------------------------------------------------------------------------------
// Drawing

void CallGraphPanel::draw(const PanelContext& context, bool* open) {
  update(context);
  if (m_focusPending) {
    ImGui::SetNextWindowFocus();
    m_focusPending = false;
  }
  ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
  m_drawnRows.clear();
  if (!ImGui::Begin(name(), open)) {
    ImGui::End();
    return;
  }
  if (!context.document || !context.selection) {
    ImGui::TextDisabled("No trace open.");
    ImGui::End();
    return;
  }
  if (!context.queryExecutor) {
    ImGui::TextDisabled("Analysis is not available.");
    ImGui::End();
    return;
  }
  drawToolbar(context);
  const float pieHeight = std::clamp(ImGui::GetContentRegionAvail().y * 0.3f, ImGui::GetFontSize() * 6.0f,
                                     ImGui::GetFontSize() * 12.0f);
  const float tableHeight =
      std::max(ImGui::GetContentRegionAvail().y - pieHeight - ImGui::GetStyle().ItemSpacing.y,
               ImGui::GetFontSize() * 4.0f);
  if (m_tree) {
    drawTable(context, tableHeight);
  } else if (m_query.pending()) {
    drawSpinner(ImGui::GetFontSize() * 0.5f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    ImGui::SameLine();
    ImGui::TextUnformatted("Building call graph...");
    ImGui::Dummy(ImVec2(0, tableHeight - ImGui::GetFontSize() * 2.0f));
  } else {
    ImGui::TextDisabled("%s", m_message.empty() ? "No call graph." : m_message.c_str());
    ImGui::Dummy(ImVec2(0, tableHeight - ImGui::GetFontSize() * 2.0f));
  }
  drawPie(context);
  drawContextMenu(context);
  ImGui::End();
}

void CallGraphPanel::drawToolbar(const PanelContext& context) {
  if (ImGui::Button("Expand hot path")) expandHotPath();
  ImGui::SameLine();
  if (ImGui::Button("Expand all")) expandAllRows();
  ImGui::SameLine();
  if (ImGui::Button("Collapse all")) collapseAllRows();
  ImGui::SameLine();
  bool inl = m_inline;
  if (ImGui::Checkbox("Inline frames", &inl)) setInlineExpansion(inl);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
  static constexpr const char* kKernelNames[] = {"Kernel: show", "Kernel: group", "Kernel: hide"};
  int kernel = static_cast<int>(m_kernel);
  if (ImGui::Combo("##kernel", &kernel, kKernelNames, 3))
    setKernelFrames(static_cast<analysis::KernelFrames>(kernel));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
  static constexpr const char* kPercentNames[] = {"% of root", "% of parent"};
  int percent = m_percentMode == cg::PercentMode::OfRoot ? 0 : 1;
  if (ImGui::Combo("##percent", &percent, kPercentNames, 2))
    setPercentMode(percent == 0 ? cg::PercentMode::OfRoot : cg::PercentMode::OfParent);
  if (m_query.pending()) {
    ImGui::SameLine();
    drawSpinner(ImGui::GetFontSize() * 0.4f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
  }

  // Second line: totals, the subtree filter chip, messages.
  if (m_tree) {
    ImGui::Text("%.3f ms  %u samples  %zu rows%s", cg::ticksToMs(m_tree->inclusiveTicks, m_clockHz),
                m_tree->sampleCount, m_rows.size(), m_truncated ? " (row cap reached)" : "");
  }
  const SelectionState& state = context.selection->state();
  if (state.timeFilter && state.timeFilter->subtreeRoot) {
    ImGui::SameLine();
    const std::string label = !m_subtreeLabel.empty() && m_subtreeLabelNode == *state.timeFilter->subtreeRoot
                                  ? m_subtreeLabel
                                  : "node #" + std::to_string(state.timeFilter->subtreeRoot->get());
    ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), "  Subtree: %s", label.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x##clearsubtree")) clearSubtreeFilter(context);
  }
  if (!m_message.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "  %s", m_message.c_str());
  }
}

void CallGraphPanel::drawTable(const PanelContext& context, float height) {
  const bool stale = m_query.pending();
  if (stale) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Hideable |
                                ImGuiTableFlags_SizingFixedFit;
  const float em = ImGui::GetFontSize();
  int toggleRow = -1;
  if (ImGui::BeginTable("##callgraph", 7, flags, ImVec2(0.0f, height))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide,
                            5.0f);
    ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, em * 8.0f);
    ImGui::TableSetupColumn("Inclusive", ImGuiTableColumnFlags_WidthFixed, em * 6.5f);
    ImGui::TableSetupColumn("Incl %", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableSetupColumn("Exclusive", ImGuiTableColumnFlags_WidthFixed, em * 6.5f);
    ImGui::TableSetupColumn("Excl %", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableSetupColumn("Samples", ImGuiTableColumnFlags_WidthFixed, em * 5.0f);
    ImGui::TableHeadersRow();

    const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    if (m_scrollTo) {
      const size_t idx = rowOfId(*m_scrollTo);
      if (idx != SIZE_MAX) {
        ImGui::SetScrollY(std::max(0.0f, static_cast<float>(idx) * rowH - (height - rowH * 2.0f) * 0.5f));
        context.requestRedraw();
      }
      m_scrollTo.reset();
    }

    const NodeAgg& root = *m_tree;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_rows.size()), rowH);
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const cg::Row& r = m_rows[static_cast<size_t>(i)];
        const NodeAgg& n = *r.node;
        const NameParts& np = nameOf(n.key);
        const uint64_t parentTotal =
            r.parentRow == UINT32_MAX ? root.inclusiveTicks : m_rows[r.parentRow].node->inclusiveTicks;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(i);
        const bool selected = m_selectedId && *m_selectedId == r.id;
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##row", selected,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(0.0f, rowH - ImGui::GetStyle().CellPadding.y * 2.0f));
        const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        const float indent = static_cast<float>(std::min(r.depth, kMaxIndentLevels)) * em * 1.1f;
        const float arrowW = em * 1.1f;
        const float arrowRight = cell.x + indent + arrowW;
        m_drawnRows.push_back({static_cast<size_t>(i), rmin.x, rmin.y, rmax.x, rmax.y, arrowRight});
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
          if (r.hasChildren && ImGui::GetIO().MousePos.x < arrowRight) {
            toggleRow = i;
          } else {
            selectRow(context, static_cast<size_t>(i));
          }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
            !(r.hasChildren && ImGui::GetIO().MousePos.x < arrowRight)) {
          if (const auto t = targetOfRow(static_cast<size_t>(i))) showSource(context, *t);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
          selectRow(context, static_cast<size_t>(i));
          m_menuTarget = targetOfRow(static_cast<size_t>(i));
          m_openMenu = true;
        }
        // Arrow and name, drawn over the selectable.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 textCol = ImGui::GetColorU32(ImGuiCol_Text);
        if (r.hasChildren) {
          const float cx = cell.x + indent + arrowW * 0.5f, cy = (rmin.y + rmax.y) * 0.5f, s = em * 0.28f;
          if (r.expanded) {
            dl->AddTriangleFilled(ImVec2(cx - s, cy - s * 0.6f), ImVec2(cx + s, cy - s * 0.6f),
                                  ImVec2(cx, cy + s * 0.8f), textCol);
          } else {
            dl->AddTriangleFilled(ImVec2(cx - s * 0.6f, cy - s), ImVec2(cx - s * 0.6f, cy + s),
                                  ImVec2(cx + s * 0.8f, cy), textCol);
          }
        }
        ImGui::SetCursorScreenPos(ImVec2(arrowRight, cell.y));
        ImGui::TextUnformatted(np.function.c_str());
        if (np.inlined) {
          ImGui::SameLine(0.0f, 4.0f);
          ImGui::TextDisabled("[inline]");
        }
        ImGui::PopID();

        char buf[64];
        const double incl = cg::ticksToMs(n.inclusiveTicks, m_clockHz);
        const double excl = cg::ticksToMs(n.exclusiveTicks, m_clockHz);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(np.module.c_str());
        ImGui::TableSetColumnIndex(2);
        std::snprintf(buf, sizeof buf, "%.3f ms", incl);
        textRight(buf);
        ImGui::TableSetColumnIndex(3);
        std::snprintf(buf, sizeof buf, "%.1f%%",
                      cg::percentOf(n.inclusiveTicks, root.inclusiveTicks, parentTotal, m_percentMode));
        textRight(buf);
        ImGui::TableSetColumnIndex(4);
        std::snprintf(buf, sizeof buf, "%.3f ms", excl);
        textRight(buf);
        ImGui::TableSetColumnIndex(5);
        std::snprintf(buf, sizeof buf, "%.1f%%",
                      cg::percentOf(n.exclusiveTicks, root.inclusiveTicks, parentTotal, m_percentMode));
        textRight(buf);
        ImGui::TableSetColumnIndex(6);
        std::snprintf(buf, sizeof buf, "%u", n.sampleCount);
        textRight(buf);
      }
    }
    ImGui::EndTable();
  }
  if (stale) ImGui::PopStyleVar();
  if (toggleRow >= 0) toggleExpanded(static_cast<size_t>(toggleRow));
}

void CallGraphPanel::drawPie(const PanelContext&) {
  const float em = ImGui::GetFontSize();
  const float avail = std::max(ImGui::GetContentRegionAvail().y, em * 4.0f);
  const float radius = std::min(avail * 0.5f - 4.0f, em * 5.0f);
  if (!ImGui::BeginChild("##cgpie", ImVec2(0.0f, avail), ImGuiChildFlags_Borders)) {
    ImGui::EndChild();
    return;
  }
  if (m_pie.empty()) {
    ImGui::TextDisabled("%s", m_tree ? "The selected node has no children with time." : "");
    ImGui::EndChild();
    return;
  }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 centre(origin.x + radius + 4.0f, origin.y + radius + 2.0f);
  constexpr float kHalfPi = 1.5707963f;
  for (size_t i = 0; i < m_pie.size(); ++i) {
    const cg::PieSlice& s = m_pie[i];
    const ImU32 col = sliceColor(i, s.node == nullptr);
    if (s.fraction >= 0.9999) {
      dl->AddCircleFilled(centre, radius, col, 48);
      continue;
    }
    // Convex pieces of at most a quarter turn each.
    float a = s.angle0;
    while (a < s.angle1) {
      const float b = std::min(a + kHalfPi, s.angle1);
      const int segments = std::max(2, static_cast<int>((b - a) / kHalfPi * 12.0f));
      dl->PathClear();
      dl->PathLineTo(centre);
      dl->PathArcTo(centre, radius, a - kHalfPi, b - kHalfPi, segments);
      dl->PathFillConvex(col);
      a = b;
    }
  }
  // Legend.
  ImGui::SetCursorScreenPos(ImVec2(centre.x + radius + em, origin.y));
  ImGui::BeginGroup();
  ImGui::Text("Children of %s", m_pieTitle.c_str());
  for (size_t i = 0; i < m_pie.size(); ++i) {
    const cg::PieSlice& s = m_pie[i];
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + em * 0.8f, p.y + em * 0.8f), sliceColor(i, s.node == nullptr));
    ImGui::Dummy(ImVec2(em, em * 0.8f));
    ImGui::SameLine();
    const std::string label =
        s.node ? nameOf(s.node->key).function : "other (" + std::to_string(s.otherCount) + ")";
    ImGui::Text("%5.1f%%  %s", s.fraction * 100.0, label.c_str());
  }
  ImGui::EndGroup();
  ImGui::EndChild();
}

void CallGraphPanel::drawContextMenu(const PanelContext& context) {
  if (m_openMenu) {
    ImGui::OpenPopup("##cgmenu");
    m_openMenu = false;
  }
  if (!ImGui::BeginPopup("##cgmenu")) {
    return;
  }
  if (m_menuTarget) {
    const ActionTarget target = *m_menuTarget;  // the actions may re-query and invalidate nothing here
    ImGui::TextDisabled("%s", target.fullName.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Filter to subtree", nullptr, false, !target.isRoot))
      filterToSubtree(context, target);
    if (ImGui::MenuItem("Show in Function List", nullptr, false, !target.isRoot))
      showInFunctionList(context, target);
    if (ImGui::MenuItem("Show source", nullptr, false, !target.isRoot)) showSource(context, target);
    if (ImGui::MenuItem("Copy name")) copyName(target);
    ImGui::Separator();
    if (ImGui::MenuItem("Expand hot path from here")) expandHotPath();
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------

std::unique_ptr<IPanel> makeCallGraphPanel() {
  return std::make_unique<CallGraphPanel>();
}

}  // namespace tachyon::viewer
