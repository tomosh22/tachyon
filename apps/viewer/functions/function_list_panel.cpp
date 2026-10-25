#include "viewer/functions/function_list_panel.h"

#include "viewer/builtin_panels.h"
#include "viewer/common/function_names.h"
#include "viewer/common/imgui_util.h"
#include "viewer/common/query_filter.h"
#include "viewer/document.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>

namespace tachyon::viewer {

namespace fl = functions;
using common::drawSpinner;
using common::textRight;
using symbols::FunctionKey;

namespace {

constexpr auto kThrottleWhileResolving = std::chrono::milliseconds(200);
constexpr size_t kFilterBufferSize = 256;

// Table column of the first column that sorts by `column`.
int columnOf(fl::SortColumn column) {
  switch (column) {
    case fl::SortColumn::Function: return 0;
    case fl::SortColumn::Module: return 1;
    case fl::SortColumn::Inclusive: return 2;
    case fl::SortColumn::Exclusive: return 4;
    case fl::SortColumn::Instances: return 6;
  }
  return 4;
}

// ImGui column user ids: the sort column + 1 (0 would mean "no id").
ImGuiID userId(fl::SortColumn column) {
  return static_cast<ImGuiID>(column) + 1;
}

}  // namespace

FunctionListPanel::FunctionListPanel() = default;
FunctionListPanel::~FunctionListPanel() = default;

// ---------------------------------------------------------------------------------------------------------
// Lifecycle and bus

void FunctionListPanel::resetDocumentState() {
  m_query.cancel();
  m_table.reset();
  m_view.clear();
  m_groups.clear();
  m_items.clear();
  m_collapsed.clear();
  m_selectedKey.reset();
  m_selectedEntry.reset();
  m_externalKey.reset();
  m_scrollPending = false;
  m_menuEntry.reset();
  m_openMenu = false;
  m_haveRequested = false;
  m_requested = {};
  m_shown = {};
  m_message.clear();
  m_drawnRows.clear();
  m_haveDocument = false;
}

void FunctionListPanel::onDocumentChanged(const Document*) {
  resetDocumentState();  // the in-flight query is cancelled; a late result is dropped as stale
}

void FunctionListPanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                           SelectionField field) {
  if (field == SelectionField::PanelRequest && state.panelRequest.name == kName) {
    m_focusPending = true;
    ++m_focusRequests;
  }
  if (field == SelectionField::SelectedFunction || field == SelectionField::All) {
    if (state.selectedFunction != m_selectedKey) {
      m_externalKey = state.selectedFunction;
      if (!state.selectedFunction) {
        m_selectedKey.reset();
        m_selectedEntry.reset();
      }
    }
    context.requestRedraw();
  }
  if (field == SelectionField::TimeFilter || field == SelectionField::All) context.requestRedraw();
}

FunctionListPanel::QuerySpec FunctionListPanel::currentSpec(const PanelContext& context) const {
  QuerySpec spec;
  spec.filter = fl::makeFilter(context.selection ? context.selection->state().timeFilter : std::nullopt,
                               m_restrictSubtree);
  spec.expandInline = m_inline;
  spec.kernelFrames = m_kernel;
  spec.documentGeneration = context.documentGeneration;
  spec.symbolGeneration = context.symbolGeneration;
  return spec;
}

void FunctionListPanel::issue(const PanelContext& context, const QuerySpec& spec) {
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
  const model::TraceModel& model = doc.model();
  m_pendingClockHz =
      model.header.clock_frequency_hz ? static_cast<double>(model.header.clock_frequency_hz) : 1e9;
  const double clockHz = m_pendingClockHz;
  analysis::QueryExecutor& executor = *context.queryExecutor;
  const analysis::Filter filter = spec.filter;
  using Value = std::shared_ptr<const fl::Table>;
  m_query.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [filter, clockHz](const analysis::QueryContext& qc, analysis::Cancel cancel) -> Result<Value> {
          auto rows = analysis::bottomUpFunctionsShared(qc, filter, cancel);
          if (!rows.ok()) return rows.error();
          return fl::buildTable(
              **rows, [&qc](const FunctionKey& k) { return common::splitFunctionName(&qc, k); }, clockHz);
        });
  });
}

void FunctionListPanel::update(const PanelContext& context) {
  if (!context.document) {
    if (m_haveDocument || m_table) resetDocumentState();
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
      m_shown = m_requested;
      m_message.clear();
      applyTable(result->value());
    } else {
      m_message = "Query failed: " + result->error().toString();
    }
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
  if (m_externalKey && m_table) resolveExternalSelection();
}

// ---------------------------------------------------------------------------------------------------------
// Table state

void FunctionListPanel::applyTable(std::shared_ptr<const fl::Table> table) {
  m_table = std::move(table);
  ++m_delivered;
  // The same function again if the new table still has it; keys change when modules resolve.
  m_selectedEntry.reset();
  if (m_selectedKey && m_table) {
    const size_t at = fl::entryOfKey(*m_table, *m_selectedKey);
    if (at != SIZE_MAX) m_selectedEntry = static_cast<uint32_t>(at);
  }
  rebuildView();
}

void FunctionListPanel::injectTable(std::shared_ptr<const fl::Table> table) {
  applyTable(std::move(table));
}

void FunctionListPanel::rebuildView() {
  m_groups.clear();
  m_view.clear();
  if (m_table) {
    m_view = fl::buildView(*m_table, m_filterNorm, m_sort);
    if (m_grouped) m_groups = fl::groupByModule(*m_table, m_view, m_sort);
  }
  rebuildItems();
}

void FunctionListPanel::rebuildItems() {
  m_items = m_grouped ? fl::groupedItems(m_groups, m_collapsed) : fl::flatItems(m_view);
}

void FunctionListPanel::setFilterText(const std::string& text) {
  m_filterText = text;
  const std::string norm = fl::normalizeFilterText(text);
  if (norm == m_filterNorm) return;
  m_filterNorm = norm;
  rebuildView();
}

void FunctionListPanel::setSort(SortSpec sort) {
  if (sort == m_sort) return;
  m_sort = sort;
  m_pushSort = true;
  rebuildView();
}

void FunctionListPanel::setGroupByModule(bool on) {
  if (on == m_grouped) return;
  m_grouped = on;
  rebuildView();
}

void FunctionListPanel::toggleGroupCollapsed(size_t group) {
  if (group >= m_groups.size()) return;
  const std::string& module = m_groups[group].module;
  if (!m_collapsed.erase(module)) m_collapsed.insert(module);
  rebuildItems();
}

void FunctionListPanel::resolveExternalSelection() {
  const FunctionKey key = *m_externalKey;
  m_externalKey.reset();
  m_selectedKey = key;  // remembered, so the same bus value is not searched for again
  m_selectedEntry.reset();
  const size_t at = fl::entryOfKey(*m_table, key);
  if (at == SIZE_MAX) return;
  const uint32_t entry = static_cast<uint32_t>(at);
  m_selectedEntry = entry;
  // Make the row visible: the text filter may hide it, a collapsed module group may hold it.
  if (fl::itemOfEntry(m_items, entry) == SIZE_MAX) {
    if (std::find(m_view.begin(), m_view.end(), entry) == m_view.end()) {
      m_filterText.clear();
      m_filterNorm.clear();
      rebuildView();
    }
    if (m_grouped && fl::itemOfEntry(m_items, entry) == SIZE_MAX) {
      const std::string& module = m_table->entries[entry].module;
      m_collapsed.erase(module.empty() ? "(no module)" : module);
      rebuildItems();
    }
  }
  m_scrollPending = true;
}

// ---------------------------------------------------------------------------------------------------------
// Actions

void FunctionListPanel::selectEntry(const PanelContext& context, uint32_t entry) {
  if (!m_table || entry >= m_table->entries.size()) return;
  const FunctionKey key = m_table->entries[entry].key;
  m_selectedEntry = entry;
  m_selectedKey = key;  // before publishing: our own onSelectionChanged must see "nothing new"
  if (context.selection) context.selection->setSelectedFunction(key);
}

void FunctionListPanel::showInCallGraph(const PanelContext& context, uint32_t entry) {
  if (!context.selection || !m_table || entry >= m_table->entries.size()) return;
  selectEntry(context, entry);
  context.selection->requestPanel(kCallGraphPanel);
}

void FunctionListPanel::showSource(const PanelContext& context, uint32_t entry) {
  if (!context.selection || !m_table || entry >= m_table->entries.size()) return;
  selectEntry(context, entry);
  context.selection->requestPanel(kSourcePanel);
}

void FunctionListPanel::copyName(uint32_t entry) {
  if (!m_table || entry >= m_table->entries.size()) return;
  ImGui::SetClipboardText(m_table->entries[entry].full.c_str());
}

// ---------------------------------------------------------------------------------------------------------
// Drawing

void FunctionListPanel::draw(const PanelContext& context, bool* open) {
  update(context);
  if (m_focusPending) {
    ImGui::SetNextWindowFocus();
    m_focusPending = false;
  }
  ImGui::SetNextWindowSize(ImVec2(820.0f, 480.0f), ImGuiCond_FirstUseEver);
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
  const float height = std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 4.0f);
  if (m_table) {
    drawTable(context, height);
  } else if (m_query.pending()) {
    drawSpinner(ImGui::GetFontSize() * 0.5f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    ImGui::SameLine();
    ImGui::TextUnformatted("Building function list...");
  } else {
    ImGui::TextDisabled("%s", m_message.empty() ? "No functions." : m_message.c_str());
  }
  drawContextMenu(context);
  ImGui::End();
}

void FunctionListPanel::drawToolbar(const PanelContext& context) {
  const float em = ImGui::GetFontSize();
  char buf[kFilterBufferSize];
  std::snprintf(buf, sizeof buf, "%s", m_filterText.c_str());
  ImGui::SetNextItemWidth(em * 14.0f);
  if (ImGui::InputTextWithHint("##fnfilter", "Filter (function or module)", buf, sizeof buf))
    setFilterText(buf);
  ImGui::SameLine();
  bool restrict = m_restrictSubtree;
  if (ImGui::Checkbox("Call graph subtree", & restrict)) setRestrictToSubtree(restrict);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Restrict the list to the subtree selected in the Call Graph (\"Filter to subtree\").");
  ImGui::SameLine();
  bool grouped = m_grouped;
  if (ImGui::Checkbox("Group by module", &grouped)) setGroupByModule(grouped);
  ImGui::SameLine();
  bool inl = m_inline;
  if (ImGui::Checkbox("Inline frames", &inl)) setInlineExpansion(inl);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(em * 9.0f);
  static constexpr const char* kKernelNames[] = {"Kernel: show", "Kernel: group", "Kernel: hide"};
  int kernel = static_cast<int>(m_kernel);
  if (ImGui::Combo("##fnkernel", &kernel, kKernelNames, 3))
    setKernelFrames(static_cast<analysis::KernelFrames>(kernel));
  if (m_query.pending()) {
    ImGui::SameLine();
    drawSpinner(em * 0.4f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
  }

  if (m_table) {
    ImGui::Text("%zu of %zu functions  %.3f ms total", m_view.size(), m_table->entries.size(),
                common::ticksToMs(m_table->totalTicks, m_table->clockHz));
  }
  const SelectionState& state = context.selection->state();
  if (m_restrictSubtree && state.timeFilter && state.timeFilter->subtreeRoot) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), "  Subtree: node #%u",
                       state.timeFilter->subtreeRoot->get());
  }
  if (!m_message.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "  %s", m_message.c_str());
  }
}

void FunctionListPanel::drawTable(const PanelContext& context, float height) {
  const bool stale = m_query.pending();
  if (stale) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Hideable |
                                ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingFixedFit;
  const float em = ImGui::GetFontSize();
  const fl::Table& table = *m_table;
  const double clockHz = table.clockHz;
  std::optional<uint32_t> clicked, rightClicked, doubleClicked;
  std::optional<size_t> toggledGroup;
  if (ImGui::BeginTable("##functionlist", 7, flags, ImVec2(0.0f, height))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    const ImGuiTableColumnFlags num =
        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending;
    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide,
                            5.0f, userId(fl::SortColumn::Function));
    ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, em * 8.0f,
                            userId(fl::SortColumn::Module));
    ImGui::TableSetupColumn("Inclusive", num, em * 6.5f, userId(fl::SortColumn::Inclusive));
    ImGui::TableSetupColumn("Incl %", num, em * 4.5f, userId(fl::SortColumn::Inclusive));
    ImGui::TableSetupColumn("Exclusive", num | ImGuiTableColumnFlags_DefaultSort, em * 6.5f,
                            userId(fl::SortColumn::Exclusive));
    ImGui::TableSetupColumn("Excl %", num, em * 4.5f, userId(fl::SortColumn::Exclusive));
    ImGui::TableSetupColumn("Instances", num, em * 5.5f, userId(fl::SortColumn::Instances));
    ImGui::TableHeadersRow();

    if (m_pushSort) {
      ImGui::TableSetColumnSortDirection(
          columnOf(m_sort.column),
          m_sort.ascending ? ImGuiSortDirection_Ascending : ImGuiSortDirection_Descending, false);
      m_pushSort = false;
    } else if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
      if (specs->SpecsDirty) {
        if (specs->SpecsCount > 0) {
          const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
          const SortSpec next{static_cast<fl::SortColumn>(s.ColumnUserID - 1),
                              s.SortDirection == ImGuiSortDirection_Ascending};
          if (!(next == m_sort)) {
            m_sort = next;
            rebuildView();
          }
        }
        specs->SpecsDirty = false;
      }
    }

    const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    if (m_scrollPending) {
      const size_t idx = m_selectedEntry ? fl::itemOfEntry(m_items, *m_selectedEntry) : SIZE_MAX;
      if (idx != SIZE_MAX) {
        ImGui::SetScrollY(std::max(0.0f, static_cast<float>(idx) * rowH - (height - rowH * 2.0f) * 0.5f));
        context.requestRedraw();
      }
      m_scrollPending = false;
    }

    char buf[64];
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_items.size()), rowH);
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const fl::Item& item = m_items[static_cast<size_t>(i)];
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(i);
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        const float selectableH = rowH - ImGui::GetStyle().CellPadding.y * 2.0f;
        if (item.header) {
          const fl::Group& g = m_groups[item.group];
          const bool collapsed = m_collapsed.count(g.module) != 0;
          ImGui::Selectable("##group", false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, selectableH));
          const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
          m_drawnRows.push_back({static_cast<size_t>(i), rmin.x, rmin.y, rmax.x, rmax.y});
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) toggledGroup = item.group;
          ImGui::SetCursorScreenPos(cell);
          ImGui::Text("%s %s (%zu)", collapsed ? ">" : "v", g.module.c_str(), g.members.size());
          ImGui::PopID();
          ImGui::TableSetColumnIndex(2);
          std::snprintf(buf, sizeof buf, "%.3f ms", common::ticksToMs(g.inclusiveSum, clockHz));
          textRight(buf);
          if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Per-module inclusive is the sum of the function inclusives; it counts a sample once per\n"
                "function of the module on its stack and may exceed the real time spent in the module.\n"
                "The exclusive sum is exact.");
          ImGui::TableSetColumnIndex(3);
          std::snprintf(buf, sizeof buf, "%.1f%%", fl::percentOfTotal(g.inclusiveSum, table.totalTicks));
          textRight(buf);
          ImGui::TableSetColumnIndex(4);
          std::snprintf(buf, sizeof buf, "%.3f ms", common::ticksToMs(g.exclusiveSum, clockHz));
          textRight(buf);
          ImGui::TableSetColumnIndex(5);
          std::snprintf(buf, sizeof buf, "%.1f%%", fl::percentOfTotal(g.exclusiveSum, table.totalTicks));
          textRight(buf);
          ImGui::TableSetColumnIndex(6);
          std::snprintf(buf, sizeof buf, "%llu", static_cast<unsigned long long>(g.instancesSum));
          textRight(buf);
          continue;
        }
        const fl::Entry& e = table.entries[item.entry];
        const bool selected = m_selectedEntry && *m_selectedEntry == item.entry;
        ImGui::Selectable("##row", selected,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(0.0f, selectableH));
        const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        m_drawnRows.push_back({static_cast<size_t>(i), rmin.x, rmin.y, rmax.x, rmax.y});
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clicked = item.entry;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
          doubleClicked = item.entry;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) rightClicked = item.entry;
        ImGui::SetCursorScreenPos(ImVec2(cell.x + (m_grouped ? em : 0.0f), cell.y));
        ImGui::TextUnformatted(e.function.c_str());
        if (e.inlined) {
          ImGui::SameLine(0.0f, 4.0f);
          ImGui::TextDisabled("[inline]");
        }
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(e.module.c_str());
        ImGui::TableSetColumnIndex(2);
        std::snprintf(buf, sizeof buf, "%.3f ms", common::ticksToMs(e.inclusiveTicks, clockHz));
        textRight(buf);
        ImGui::TableSetColumnIndex(3);
        std::snprintf(buf, sizeof buf, "%.1f%%", fl::percentOfTotal(e.inclusiveTicks, table.totalTicks));
        textRight(buf);
        ImGui::TableSetColumnIndex(4);
        std::snprintf(buf, sizeof buf, "%.3f ms", common::ticksToMs(e.exclusiveTicks, clockHz));
        textRight(buf);
        ImGui::TableSetColumnIndex(5);
        std::snprintf(buf, sizeof buf, "%.1f%%", fl::percentOfTotal(e.exclusiveTicks, table.totalTicks));
        textRight(buf);
        ImGui::TableSetColumnIndex(6);
        std::snprintf(buf, sizeof buf, "%u", e.instances);
        textRight(buf);
      }
    }
    ImGui::EndTable();
  }
  if (stale) ImGui::PopStyleVar();
  if (toggledGroup) toggleGroupCollapsed(*toggledGroup);
  if (clicked) selectEntry(context, *clicked);
  if (doubleClicked) showSource(context, *doubleClicked);
  if (rightClicked) {
    selectEntry(context, *rightClicked);
    m_menuEntry = *rightClicked;
    m_openMenu = true;
  }
}

void FunctionListPanel::drawContextMenu(const PanelContext& context) {
  if (m_openMenu) {
    ImGui::OpenPopup("##fnmenu");
    m_openMenu = false;
  }
  if (!ImGui::BeginPopup("##fnmenu")) return;
  if (m_menuEntry && m_table && *m_menuEntry < m_table->entries.size()) {
    const uint32_t entry = *m_menuEntry;
    ImGui::TextDisabled("%s", m_table->entries[entry].full.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Call Graph")) showInCallGraph(context, entry);
    if (ImGui::MenuItem("Show source")) showSource(context, entry);
    if (ImGui::MenuItem("Copy name")) copyName(entry);
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------

std::unique_ptr<IPanel> makeFunctionListPanel() {
  return std::make_unique<FunctionListPanel>();
}

}  // namespace tachyon::viewer
