#include "viewer/stats/function_stats_panel.h"

#include "viewer/builtin_panels.h"
#include "viewer/common/function_names.h"
#include "viewer/common/imgui_util.h"
#include "viewer/common/query_filter.h"
#include "viewer/document.h"
#include "viewer/functions/function_list_model.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tachyon::viewer {

namespace st = stats;
using symbols::FunctionKey;

namespace {

constexpr auto kThrottleWhileResolving = std::chrono::milliseconds(200);
constexpr size_t kQueryBufferSize = 256;

// Up / Down in the search box move the result cursor. Registering for the history callback makes ImGui keep
// those keys for the text field instead of moving the navigation focus away from it.
// CallbackAlways pushes text that was set programmatically into the active editor (ImGui ignores the caller's
// buffer while the field is being edited).
int searchCallback(ImGuiInputTextCallbackData* data) {
  auto* panel = static_cast<FunctionStatsPanel*>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    panel->moveCursor(data->EventKey == ImGuiKey_UpArrow ? -1 : 1);
  } else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
    if (const std::optional<std::string> text = panel->takeEditorText()) {
      data->DeleteChars(0, data->BufTextLen);
      data->InsertChars(0, text->c_str());
    }
  }
  return 0;
}

const char* kindLabel(st::EntryKind kind) {
  return kind == st::EntryKind::Scope ? "scope" : "func";
}

ImVec4 kindColor(st::EntryKind kind) {
  return kind == st::EntryKind::Scope ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(0.55f, 0.75f, 1.0f, 1.0f);
}

}  // namespace

FunctionStatsPanel::FunctionStatsPanel() = default;
FunctionStatsPanel::~FunctionStatsPanel() = default;

// ---------------------------------------------------------------------------------------------------------
// Lifecycle and bus

void FunctionStatsPanel::resetDocumentState() {
  m_indexQuery.cancel();
  m_statsQuery.cancel();
  m_index.reset();
  m_hits.clear();
  m_cursor = -1;
  m_selected.reset();
  m_result.reset();
  m_current.reset();
  m_publishedHighlight = false;
  m_haveRequestedIndex = false;
  m_haveRequestedStats = false;
  m_requestedStats = {};
  m_shownStats = {};
  m_requestedIndexSymbols = m_shownIndexSymbols = 0;
  m_message.clear();
  m_histogram = {};
  m_boxPlot = {};
  m_drawnRows.clear();
  m_haveDocument = false;
}

void FunctionStatsPanel::onDocumentChanged(const Document*) {
  resetDocumentState();
}

void FunctionStatsPanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                            SelectionField field) {
  if (field == SelectionField::PanelRequest && state.panelRequest.name == kName) {
    m_focusWindow = true;
    m_focusSearch = true;
    ++m_focusRequests;
    context.requestRedraw();
  }
  if (field == SelectionField::All) {
    m_current.reset();
    m_publishedHighlight = false;
    context.requestRedraw();
  }
  if (field == SelectionField::TimeFilter) context.requestRedraw();
}

bool FunctionStatsPanel::symbolBurst(const PanelContext& context) const {
  if (!context.symbols) return false;
  const symbols::SymbolProgress progress = context.symbols->progress();
  return progress.done < progress.total && Clock::now() - m_lastIssue < kThrottleWhileResolving;
}

FunctionStatsPanel::StatsSpec FunctionStatsPanel::statsSpecFor(const PanelContext& context,
                                                               const st::SearchEntry& entry) const {
  StatsSpec spec;
  spec.key = entry.key;
  spec.filter =
      common::toQueryFilter(context.selection ? context.selection->state().timeFilter : std::nullopt);
  spec.filter.subtreeRoot.reset();  // the Call Graph subtree does not restrict function statistics
  spec.options = functions::effectiveOptions(false, analysis::KernelFrames::Show, entry.key);
  spec.documentGeneration = context.documentGeneration;
  spec.symbolGeneration = context.symbolGeneration;
  return spec;
}

void FunctionStatsPanel::issueIndex(const PanelContext& context) {
  const Document& doc = *context.document;
  const analysis::AggregationOptions options;
  std::shared_ptr<const AnalysisSnapshot> snapshot =
      doc.analysis().snapshot(options, context.symbolGeneration, context.queryExecutor);
  if (!snapshot) {
    m_message = "Analysis is not available for this document.";
    return;
  }
  m_requestedIndexSymbols = context.symbolGeneration;
  m_haveRequestedIndex = true;
  m_lastIssue = Clock::now();
  const model::TraceModel& model = doc.model();
  m_clockHz = model.header.clock_frequency_hz ? static_cast<double>(model.header.clock_frequency_hz) : 1e9;
  m_sampleInterval = std::max<uint64_t>(model.sampleIntervalTicks, 1);
  analysis::QueryExecutor& executor = *context.queryExecutor;
  using Value = std::shared_ptr<const st::SearchIndex>;
  m_indexQuery.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [](const analysis::QueryContext& qc, analysis::Cancel cancel) -> Result<Value> {
          auto rows = analysis::bottomUpFunctionsShared(qc, analysis::Filter{}, cancel);
          if (!rows.ok()) return rows.error();
          const auto scopeIds = st::collectScopeIds(qc.model());
          return st::buildSearchIndex(
              **rows, [&qc](const FunctionKey& k) { return common::splitFunctionName(&qc, k); }, scopeIds,
              qc.model());
        });
  });
}

void FunctionStatsPanel::issueStats(const PanelContext& context, const StatsSpec& spec) {
  const Document& doc = *context.document;
  std::shared_ptr<const AnalysisSnapshot> snapshot =
      doc.analysis().snapshot(spec.options, spec.symbolGeneration, context.queryExecutor);
  if (!snapshot) {
    m_message = "Analysis is not available for this document.";
    return;
  }
  // The result on screen belongs to another function: do not show it under the new name, and take its
  // highlight off the timeline.
  if (m_result && m_result->key != spec.key) {
    m_result.reset();
    m_current.reset();
    if (m_publishedHighlight && context.selection)
      context.selection->setInstanceHighlight(std::nullopt, nullptr);
    m_publishedHighlight = false;
  }
  m_requestedStats = spec;
  m_haveRequestedStats = true;
  m_lastIssue = Clock::now();
  analysis::QueryExecutor& executor = *context.queryExecutor;
  const FunctionKey key = spec.key;
  const analysis::Filter filter = spec.filter;
  using Value = std::shared_ptr<const StatsResult>;
  m_statsQuery.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [key, filter](const analysis::QueryContext& qc, analysis::Cancel cancel) -> Result<Value> {
          auto stats = analysis::functionStatsShared(qc, filter, key, cancel);
          if (!stats.ok()) return stats.error();
          auto instances = analysis::functionInstances(qc, filter, key, cancel);
          if (!instances.ok()) return instances.error();
          auto result = std::make_shared<StatsResult>();
          result->key = key;
          result->stats = *stats;
          result->instances = *instances;
          return Value(std::move(result));
        });
  });
}

void FunctionStatsPanel::update(const PanelContext& context) {
  if (!context.document) {
    if (m_haveDocument || m_index) resetDocumentState();
    return;
  }
  if (!m_haveDocument || context.documentGeneration != m_documentGeneration) {
    resetDocumentState();
    m_haveDocument = true;
    m_documentGeneration = context.documentGeneration;
  }
  if (!context.queryExecutor || !context.selection) return;

  if (auto result = m_indexQuery.poll()) {
    if (result->ok()) {
      m_shownIndexSymbols = m_requestedIndexSymbols;
      m_message.clear();
      applyIndex(context, result->value());
    } else {
      m_message = "Query failed: " + result->error().toString();
    }
  }
  if (auto result = m_statsQuery.poll()) {
    if (result->ok()) {
      applyResult(context, result->value());
    } else {
      m_message = "Query failed: " + result->error().toString();
    }
  }

  if (!m_haveRequestedIndex || m_requestedIndexSymbols != context.symbolGeneration) {
    if (m_throttle && m_haveRequestedIndex && symbolBurst(context)) {
      context.requestRedraw();  // a burst of module events: re-query once it settles
    } else {
      issueIndex(context);
    }
  }
  if (m_selected) {
    const StatsSpec spec = statsSpecFor(context, *m_selected);
    if (!m_haveRequestedStats || !(spec == m_requestedStats)) {
      StatsSpec same = spec;
      same.symbolGeneration = m_requestedStats.symbolGeneration;
      if (m_throttle && m_haveRequestedStats && same == m_requestedStats && symbolBurst(context)) {
        context.requestRedraw();
      } else {
        issueStats(context, spec);
      }
    }
  }
  if (m_indexQuery.pending() || m_statsQuery.pending()) context.requestRedraw();
}

void FunctionStatsPanel::applyIndex(const PanelContext& context,
                                    std::shared_ptr<const st::SearchIndex> index) {
  m_index = std::move(index);
  runSearch();
  if (!m_selected || !m_index) return;
  // The selected entry again in the new index: keys change when modules resolve, so fall back to the name.
  const st::SearchEntry* found = m_index->find(m_selected->key);
  if (!found) {
    for (const st::SearchEntry& e : m_index->entries) {
      if (e.kind == m_selected->kind && e.name == m_selected->name) {
        found = &e;
        break;
      }
    }
  }
  if (!found) return;
  const FunctionKey oldKey = m_selected->key;
  m_selected = *found;
  if (found->key != oldKey && context.selection &&
      context.selection->state().selectedFunction == std::optional<FunctionKey>(oldKey))
    context.selection->setSelectedFunction(found->key);
}

void FunctionStatsPanel::applyResult(const PanelContext& context, std::shared_ptr<const StatsResult> result) {
  if (!result || !m_selected || result->key != m_selected->key) return;  // not the selection any more
  m_result = std::move(result);
  m_shownStats = m_requestedStats;
  ++m_deliveredResults;
  m_current.reset();
  if (context.selection) {
    context.selection->setInstanceHighlight(m_result->key, m_result->instances);
    m_publishedHighlight = true;
  }
}

// ---------------------------------------------------------------------------------------------------------
// Search and selection

void FunctionStatsPanel::runSearch() {
  const auto t0 = Clock::now();
  m_hits.clear();
  if (m_index) m_hits = st::searchEntries(*m_index, m_queryText);
  m_searchMicros = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
  m_cursor = m_hits.empty() ? -1 : 0;
  m_scrollToCursor = true;
}

void FunctionStatsPanel::applyQueryText(const std::string& text) {
  if (text == m_queryText) return;
  m_queryText = text;
  runSearch();
}

void FunctionStatsPanel::setQueryText(const std::string& text) {
  if (text == m_queryText) return;
  applyQueryText(text);
  m_editorText = text;  // the search field shows it too
}

std::optional<std::string> FunctionStatsPanel::takeEditorText() {
  std::optional<std::string> text = std::move(m_editorText);
  m_editorText.reset();
  return text;
}

void FunctionStatsPanel::moveCursor(int delta) {
  const int next = st::stepSelection(m_cursor, delta, static_cast<int>(m_hits.size()));
  if (next == m_cursor) return;
  m_cursor = next;
  m_scrollToCursor = true;
}

void FunctionStatsPanel::chooseResult(const PanelContext& context, size_t row) {
  if (row >= m_hits.size()) return;
  m_cursor = static_cast<int>(row);
  selectEntry(context, m_hits[row].entry);
}

void FunctionStatsPanel::chooseCursor(const PanelContext& context) {
  if (m_cursor >= 0) chooseResult(context, static_cast<size_t>(m_cursor));
}

void FunctionStatsPanel::useEntry(const PanelContext& context, st::SearchEntry entry, bool publishSelection) {
  const bool sameKey = m_selected && m_selected->key == entry.key;
  m_selected = std::move(entry);
  if (!sameKey) m_current.reset();
  if (publishSelection && context.selection) context.selection->setSelectedFunction(m_selected->key);
  context.requestRedraw();
}

void FunctionStatsPanel::selectEntry(const PanelContext& context, uint32_t entry) {
  if (!m_index || entry >= m_index->entries.size()) return;
  useEntry(context, m_index->entries[entry], true);
}

bool FunctionStatsPanel::populateFromSelection(const PanelContext& context) {
  if (!context.document || !context.selection) return false;
  const std::optional<FunctionKey> key = context.selection->state().selectedFunction;
  if (!key) {
    m_message = "Nothing is selected in the Call Graph, Function List, Butterfly or timeline.";
    return false;
  }
  m_message.clear();
  st::SearchEntry entry;
  if (const st::SearchEntry* found = m_index ? m_index->find(*key) : nullptr) {
    entry = *found;
  } else {
    entry.key = *key;
    entry.kind = key->isScope() ? st::EntryKind::Scope : st::EntryKind::Function;
    if (key->isScope()) {
      entry.name =
          std::string(context.document->model().strings.get(StringIdx(static_cast<uint32_t>(key->value))));
    } else {
      const analysis::AggregationOptions options =
          functions::effectiveOptions(false, analysis::KernelFrames::Show, key);
      auto snapshot = context.queryExecutor ? context.document->analysis().snapshot(
                                                  options, context.symbolGeneration, context.queryExecutor)
                                            : nullptr;
      const common::NameParts parts =
          common::splitFunctionName(snapshot ? &snapshot->context() : nullptr, *key);
      entry.name = parts.full;
      entry.module = parts.module;
    }
  }
  useEntry(context, entry, false);
  // Show the entry in the result list: the search box takes its name.
  setQueryText(entry.name);
  for (size_t i = 0; i < m_hits.size(); ++i) {
    if (m_index && m_index->entries[m_hits[i].entry].key == entry.key) {
      m_cursor = static_cast<int>(i);
      break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------
// Instances

bool FunctionStatsPanel::goToInstance(const PanelContext& context, uint32_t index) {
  if (!m_result || !m_result->instances || index >= m_result->instances->size() || !context.selection ||
      !context.document)
    return false;
  const analysis::Instance& inst = (*m_result->instances)[index];
  m_current = index;
  SelectionBus& bus = *context.selection;
  bus.setHighlightCurrent(index);
  bus.setSelectedThread(inst.thread);
  const st::TickWindow w = st::navigationWindow(inst, m_sampleInterval);
  bus.requestNavigation(inst.thread, w.t0, w.t1);
  context.requestRedraw();
  return true;
}

bool FunctionStatsPanel::nextInstance(const PanelContext& context) {
  if (!m_result || !m_result->instances) return false;
  const auto next = st::stepInstance(m_result->instances->size(), m_current, true);
  return next && goToInstance(context, *next);
}

bool FunctionStatsPanel::previousInstance(const PanelContext& context) {
  if (!m_result || !m_result->instances) return false;
  const auto next = st::stepInstance(m_result->instances->size(), m_current, false);
  return next && goToInstance(context, *next);
}

// ---------------------------------------------------------------------------------------------------------
// Drawing

void FunctionStatsPanel::draw(const PanelContext& context, bool* open) {
  update(context);
  // F3 / Shift+F3 step through the instances from anywhere while the panel is open.
  if (m_result && m_result->instances && !m_result->instances->empty() && context.selection &&
      ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
    if (ImGui::GetIO().KeyShift) {
      previousInstance(context);
    } else {
      nextInstance(context);
    }
  }
  if (m_focusWindow) {
    ImGui::SetNextWindowFocus();
    m_focusWindow = false;
  }
  ImGui::SetNextWindowSize(ImVec2(620.0f, 640.0f), ImGuiCond_FirstUseEver);
  m_drawnRows.clear();
  m_prevButton = m_nextButton = m_populateButton = {};
  m_histogram = {};
  m_boxPlot = {};
  m_searchActive = false;
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
  drawSearch(context);
  const float em = ImGui::GetFontSize();
  const float resultsHeight = std::max(em * 9.0f, ImGui::GetContentRegionAvail().y * 0.30f);
  drawResults(context, resultsHeight);
  ImGui::Separator();
  drawStats(context);
  ImGui::End();
}

void FunctionStatsPanel::drawSearch(const PanelContext& context) {
  const float em = ImGui::GetFontSize();
  const float buttonWidth =
      ImGui::CalcTextSize("Populate from selection").x + ImGui::GetStyle().FramePadding.x * 2;
  char buf[kQueryBufferSize];
  std::snprintf(buf, sizeof buf, "%s", m_queryText.c_str());
  if (m_focusSearch) {
    ImGui::SetKeyboardFocusHere();
    m_focusSearch = false;
  }
  ImGui::SetNextItemWidth(
      std::max(em * 8.0f, ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x));
  const bool enter =
      ImGui::InputTextWithHint("##statssearch", "Search functions and scopes (fuzzy)", buf, sizeof buf,
                               ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll |
                                   ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackAlways,
                               searchCallback, this);
  const bool active = ImGui::IsItemActive();
  m_searchActive = active;
  if (!active) m_editorText.reset();  // nothing to push into a field that is not being edited
  if (m_queryText != buf) applyQueryText(buf);
  if (active) {
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) moveCursor(10);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) moveCursor(-10);
  }
  if (enter) {
    chooseCursor(context);
    m_focusSearch = true;  // keep the keyboard in the box so Up / Down / Enter go on working
  }
  ImGui::SameLine();
  if (ImGui::Button("Populate from selection")) populateFromSelection(context);
  const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
  m_populateButton = {bmin.x, bmin.y, bmax.x, bmax.y, true};
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Take the function selected in the Call Graph, Function List, Butterfly or timeline.");

  if (m_indexQuery.pending() && !m_index) {
    common::drawSpinner(em * 0.4f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    ImGui::SameLine();
    ImGui::TextUnformatted("Indexing functions and scopes...");
  } else if (m_index) {
    ImGui::TextDisabled("%zu results of %zu names  (%.2f ms)", m_hits.size(), m_index->entries.size(),
                        m_searchMicros / 1000.0);
    if (m_indexQuery.pending()) {
      ImGui::SameLine();
      common::drawSpinner(em * 0.3f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    }
  }
  if (!m_message.empty()) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", m_message.c_str());
}

void FunctionStatsPanel::drawResults(const PanelContext& context, float height) {
  std::optional<size_t> clicked;
  if (ImGui::BeginChild("##statsresults", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
    const float em = ImGui::GetFontSize();
    if (ImGui::BeginTable("##statsresulttable", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
      ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, em * 3.5f);
      ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 5.0f);
      ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, em * 8.0f);
      ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
      const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(m_hits.size()), rowH);
      if (m_scrollToCursor && m_cursor >= 0) clipper.IncludeItemByIndex(m_cursor);
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          const st::SearchEntry& e = m_index->entries[m_hits[static_cast<size_t>(i)].entry];
          ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
          ImGui::TableSetColumnIndex(0);
          ImGui::PushID(i);
          const ImVec2 cell = ImGui::GetCursorScreenPos();
          const bool isCursor = i == m_cursor;
          ImGui::Selectable("##hit", isCursor, ImGuiSelectableFlags_SpanAllColumns,
                            ImVec2(0.0f, rowH - ImGui::GetStyle().CellPadding.y * 2.0f));
          const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
          m_drawnRows.push_back({static_cast<size_t>(i), rmin.x, rmin.y, rmax.x, rmax.y});
          if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clicked = static_cast<size_t>(i);
          if (isCursor && m_scrollToCursor) ImGui::SetScrollHereY();
          ImGui::PopID();
          ImGui::SetCursorScreenPos(cell);
          ImGui::TextColored(kindColor(e.kind), "%s", kindLabel(e.kind));
          ImGui::TableSetColumnIndex(1);
          const bool chosen = m_selected && m_selected->key == e.key;
          if (chosen) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s", e.name.c_str());
          } else {
            ImGui::TextUnformatted(e.name.c_str());
          }
          ImGui::TableSetColumnIndex(2);
          ImGui::TextUnformatted(e.module.c_str());
          ImGui::TableSetColumnIndex(3);
          if (e.countKnown) {
            char cbuf[48];
            std::snprintf(cbuf, sizeof cbuf, "%u %s", e.count,
                          e.kind == st::EntryKind::Scope ? "calls" : "samples");
            common::textRight(cbuf);
          }
        }
      }
      m_scrollToCursor = false;
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();
  if (clicked) chooseResult(context, *clicked);
}

void FunctionStatsPanel::drawStats(const PanelContext& context) {
  const float em = ImGui::GetFontSize();
  if (!m_selected) {
    ImGui::TextDisabled("Type to search, Up / Down + Enter to choose. Ctrl+F focuses the search box.");
    return;
  }
  ImGui::TextColored(kindColor(m_selected->kind), "%s", kindLabel(m_selected->kind));
  ImGui::SameLine();
  ImGui::TextUnformatted(m_selected->name.c_str());
  if (m_statsQuery.pending()) {
    ImGui::SameLine();
    common::drawSpinner(em * 0.4f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
  }
  if (!m_result || !m_result->stats) {
    if (!m_statsQuery.pending()) ImGui::TextDisabled("No statistics yet.");
    return;
  }
  const analysis::Stats& s = *m_result->stats;
  if (s.count == 0) {
    ImGui::TextDisabled("No instances in the selected time range.");
    return;
  }
  const bool scope = m_selected->kind == st::EntryKind::Scope;
  const double hz = m_clockHz;
  if (ImGui::BeginTable("##statsnumbers", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame)) {
    static constexpr const char* kHeads[] = {"Count", "Min", "p25", "Median", "p75", "Max", "Mean", "Total"};
    for (const char* h : kHeads) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::Text("%u", s.count);
    const double values[7] = {
        s.min, s.p25, s.median, s.p75, s.max, s.mean, static_cast<double>(s.totalTicks)};
    for (int i = 0; i < 7; ++i) {
      ImGui::TableSetColumnIndex(i + 1);
      ImGui::TextUnformatted(st::formatDuration(values[i], hz).c_str());
    }
    ImGui::EndTable();
  }
  if (scope && (s.unterminated || s.orphanEnds)) {
    ImGui::TextDisabled("%u unterminated (end at trace end), %u orphan End events ignored", s.unterminated,
                        s.orphanEnds);
  } else if (!scope) {
    ImGui::TextDisabled("Instances are approximated from samples (runs of consecutive samples).");
  }
  ImGui::Spacing();
  drawHistogram(hz);
  ImGui::Spacing();
  drawBoxPlot(hz);
  ImGui::Spacing();
  drawInstanceNav(context);
}

void FunctionStatsPanel::drawHistogram(double clockHz) {
  const analysis::Stats& s = *m_result->stats;
  const float em = ImGui::GetFontSize();
  const float width = std::max(ImGui::GetContentRegionAvail().x, em * 10.0f);
  const float plotH = em * 6.0f, labelH = em * 1.4f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##statshist", ImVec2(width, plotH + labelH));
  const bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const st::PlotRect area{p.x, p.y, p.x + width, p.y + plotH};
  m_histogram = st::layoutHistogram(s, area);
  dl->AddRectFilled(ImVec2(area.x0, area.y0), ImVec2(area.x1, area.y1), IM_COL32(24, 24, 28, 255));
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const st::HistogramBar* hot = nullptr;
  for (const st::HistogramBar& bar : m_histogram.bars) {
    const bool over = hovered && mouse.x >= bar.x0 && mouse.x <= bar.x1;
    if (over) hot = &bar;
    if (bar.count == 0) continue;
    dl->AddRectFilled(ImVec2(bar.x0, bar.y0), ImVec2(bar.x1, bar.y1),
                      over ? IM_COL32(255, 200, 90, 255) : IM_COL32(90, 150, 230, 255));
  }
  // Axis labels: the lower edge of the first bucket, then every few buckets as far as the text fits.
  if (!m_histogram.bars.empty()) {
    const float slot = area.width() / static_cast<float>(m_histogram.bars.size());
    const float textW = ImGui::CalcTextSize("000.00 \xC2\xB5s").x + 6.0f;
    const size_t step = std::max<size_t>(1, static_cast<size_t>(std::ceil(textW / std::max(slot, 1.0f))));
    for (size_t i = 0; i < m_histogram.bars.size(); i += step) {
      const st::HistogramBar& bar = m_histogram.bars[i];
      const std::string label = st::formatDuration(st::bucketLowerTicks(bar.bucket), clockHz);
      dl->AddText(ImVec2(bar.x0, area.y1 + 2.0f), IM_COL32(170, 170, 175, 255), label.c_str());
    }
    const std::string maxLabel = std::to_string(m_histogram.maxCount);
    dl->AddText(ImVec2(area.x0 + 3.0f, area.y0 + 1.0f), IM_COL32(170, 170, 175, 255), maxLabel.c_str());
  }
  if (hot) {
    const double lo = st::bucketLowerTicks(hot->bucket);
    const double hi =
        hot->bucket >= 64 ? lo * 2.0 : static_cast<double>(analysis::statsBucketUpper(hot->bucket));
    ImGui::SetTooltip("%s - %s: %u instance%s", st::formatDuration(lo, clockHz).c_str(),
                      st::formatDuration(hi, clockHz).c_str(), hot->count, hot->count == 1 ? "" : "s");
  }
}

void FunctionStatsPanel::drawBoxPlot(double clockHz) {
  const analysis::Stats& s = *m_result->stats;
  const float em = ImGui::GetFontSize();
  const float width = std::max(ImGui::GetContentRegionAvail().x, em * 10.0f) - em;
  const float plotH = em * 2.4f, labelH = em * 1.4f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##statsbox", ImVec2(width + em, plotH + labelH));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const st::PlotRect area{p.x + em * 0.5f, p.y, p.x + em * 0.5f + width, p.y + plotH};
  m_boxPlot = st::layoutBoxPlot(s, area);
  if (!m_boxPlot.valid) return;
  const st::BoxPlotLayout& b = m_boxPlot;
  const ImU32 line = IM_COL32(210, 210, 215, 255);
  dl->AddLine(ImVec2(b.xMin, b.yMid), ImVec2(b.xQ1, b.yMid), line, 1.5f);
  dl->AddLine(ImVec2(b.xQ3, b.yMid), ImVec2(b.xMax, b.yMid), line, 1.5f);
  dl->AddLine(ImVec2(b.xMin, b.yTop), ImVec2(b.xMin, b.yBottom), line, 1.5f);
  dl->AddLine(ImVec2(b.xMax, b.yTop), ImVec2(b.xMax, b.yBottom), line, 1.5f);
  dl->AddRectFilled(ImVec2(b.xQ1, b.yTop), ImVec2(std::max(b.xQ3, b.xQ1 + 1.0f), b.yBottom),
                    IM_COL32(90, 150, 230, 140));
  dl->AddRect(ImVec2(b.xQ1, b.yTop), ImVec2(std::max(b.xQ3, b.xQ1 + 1.0f), b.yBottom), line, 0.0f, 0, 1.5f);
  dl->AddLine(ImVec2(b.xMedian, b.yTop), ImVec2(b.xMedian, b.yBottom), IM_COL32(255, 200, 90, 255), 2.5f);
  dl->AddCircleFilled(ImVec2(b.xMean, b.yMid), 3.0f, IM_COL32(255, 120, 120, 255));
  const std::string lo = st::formatDuration(s.min, clockHz), hi = st::formatDuration(s.max, clockHz);
  dl->AddText(ImVec2(area.x0, area.y1 + 2.0f), IM_COL32(170, 170, 175, 255), lo.c_str());
  const float hiW = ImGui::CalcTextSize(hi.c_str()).x;
  dl->AddText(ImVec2(area.x1 - hiW, area.y1 + 2.0f), IM_COL32(170, 170, 175, 255), hi.c_str());
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "min %s  p25 %s  median %s  p75 %s  max %s  mean %s", lo.c_str(),
        st::formatDuration(s.p25, clockHz).c_str(), st::formatDuration(s.median, clockHz).c_str(),
        st::formatDuration(s.p75, clockHz).c_str(), hi.c_str(), st::formatDuration(s.mean, clockHz).c_str());
  }
}

void FunctionStatsPanel::drawInstanceNav(const PanelContext& context) {
  const size_t n = m_result->instances ? m_result->instances->size() : 0;
  if (n == 0) return;
  if (ImGui::Button("< Previous")) previousInstance(context);
  ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
  m_prevButton = {bmin.x, bmin.y, bmax.x, bmax.y, true};
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous instance (Shift+F3)");
  ImGui::SameLine();
  if (ImGui::Button("Next >")) nextInstance(context);
  bmin = ImGui::GetItemRectMin();
  bmax = ImGui::GetItemRectMax();
  m_nextButton = {bmin.x, bmin.y, bmax.x, bmax.y, true};
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next instance (F3)");
  ImGui::SameLine();
  if (m_current && *m_current < n) {
    const analysis::Instance& inst = (*m_result->instances)[*m_current];
    const model::TraceModel& model = context.document->model();
    const std::string thread =
        inst.thread.get() < model.threads.size() ? model.threads[inst.thread.get()].name : "";
    ImGui::Text(
        "Instance %u of %zu   thread %s   at %s   lasts %s", *m_current + 1, n,
        thread.empty() ? "(unnamed)" : thread.c_str(),
        st::formatDuration(static_cast<double>(inst.begin - model.header.start_ts), m_clockHz).c_str(),
        st::formatDuration(static_cast<double>(inst.end - inst.begin), m_clockHz).c_str());
  } else {
    ImGui::Text("%zu instances highlighted in the timeline", n);
  }
}

// ---------------------------------------------------------------------------------------------------------

std::unique_ptr<IPanel> makeFunctionStatsPanel() {
  return std::make_unique<FunctionStatsPanel>();
}

}  // namespace tachyon::viewer
