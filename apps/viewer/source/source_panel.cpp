#include "viewer/source/source_panel.h"

#include "viewer/common/imgui_util.h"
#include "viewer/common/query_filter.h"
#include "viewer/document.h"
#include "viewer/paths.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace tachyon::viewer {

using source::InstructionRow;
using source::LineTime;
using source::SourceView;
using source::ViewKind;
using symbols::FunctionKey;

namespace {

constexpr auto kThrottleWhileResolving = std::chrono::milliseconds(200);
constexpr size_t kSearchBufferSize = 256;
constexpr float kPitchPadding = 2.0f;

// Colours (ImGui packing 0xAABBGGRR). Running / ready match the timeline's state colours.
constexpr ImU32 kRunningColor = IM_COL32(70, 190, 90, 255);
constexpr ImU32 kReadyColor = IM_COL32(240, 170, 30, 255);
constexpr ImU32 kWaitingColor = IM_COL32(215, 70, 65, 255);
constexpr ImU32 kBarColor = IM_COL32(90, 150, 230, 255);

ImU32 heatColor(float heat) {
  return IM_COL32(255, 140, 20, static_cast<int>(heat * 120.0f));
}

}  // namespace

// Column geometry of the fixed (non-scrolling) part of a row, in characters.
struct SourcePanel::Layout {
  bool disassembly = false;
  // Source / Mixed: expander, line, bar, mini, pct, ms. Disassembly: addr, bytes, bar, pct, ms, line.
  int expander = 2, line = 7, addr = 11, bytes = 19, bar = 9, mini = 7, pct = 7, ms = 9, lineRef = 7;
  int fixedChars() const {
    return disassembly ? addr + bytes + bar + pct + ms + lineRef : expander + line + bar + mini + pct + ms;
  }
};

struct SourcePanel::RowContext {
  ImDrawList* dl = nullptr;
  const SourceView* view = nullptr;
  Layout layout;
  Mode mode = Mode::Source;
  float charW = 7.0f, pitch = 14.0f;
  float originX = 0;    // screen x of content x = 0 (scrolled)
  float fixedLeft = 0;  // screen x of the fixed columns' left edge (not scrolled)
  float fixedW = 0;     // width of the fixed columns
  float visRight = 0;   // right edge of the visible area
  size_t firstCol = 0, visCols = 0;
  float y0 = 0;  // top of the row being drawn
  uint64_t maxLineTicks = 0, maxInstrTicks = 0, instrDenominator = 1;
  ImU32 textColor = 0, dimColor = 0;
};

SourcePanel::SourcePanel(Host host)
    : m_host(std::move(host)), m_locator(std::make_shared<symbols::SourceLocator>()) {}

SourcePanel::~SourcePanel() = default;

std::unique_ptr<IPanel> makeSourcePanel(SourcePanel::Host host) {
  return std::make_unique<SourcePanel>(std::move(host));
}

// ---------------------------------------------------------------------------------------------------------
// Lifecycle and bus

void SourcePanel::resetDocumentState() {
  m_query.cancel();
  m_view.reset();
  m_rows.clear();
  m_lineRow.clear();
  m_expanded.clear();
  m_selectedRow = SIZE_MAX;
  m_scrollRow = -1;
  m_firstVisible = m_lastVisible = SIZE_MAX;
  m_matches.clear();
  m_haveRequested = false;
  m_requested = {};
  m_shown = {};
  m_message.clear();
  m_drawn.clear();
  m_haveDocument = false;
  m_pickerDebugPath.clear();
}

void SourcePanel::onDocumentChanged(const Document*) {
  resetDocumentState();  // the in-flight task is cancelled; a late result is dropped as stale
}

void SourcePanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                     SelectionField field) {
  if (field == SelectionField::PanelRequest && state.panelRequest.name == kName) {
    m_focusPending = true;
    ++m_focusRequests;
  }
  if (field == SelectionField::SelectedFunction || field == SelectionField::All ||
      field == SelectionField::TimeFilter)
    context.requestRedraw();
}

SourcePanel::Spec SourcePanel::currentSpec(const PanelContext& context) const {
  Spec spec;
  if (context.selection) {
    spec.key = context.selection->state().selectedFunction;
    spec.filter = context.selection->state().timeFilter;
    if (spec.filter) {
      spec.filter->subtreeRoot.reset();  // the function's source time is not limited to a call graph subtree
      spec.filter->function.reset();
    }
  }
  spec.documentGeneration = context.documentGeneration;
  spec.symbolGeneration = context.symbolGeneration;
  spec.sourceGeneration = m_sourceGeneration;
  return spec;
}

void SourcePanel::syncLocatorConfig(const PanelContext& context) {
  if (!context.settings) return;
  const Settings& s = *context.settings;
  if (m_haveConfig && s.sourcePathMappings == m_appliedRemaps && s.sourceRoots == m_appliedRoots) return;
  symbols::SourceLocatorConfig cfg;
  for (const SourcePathMapping& m : s.sourcePathMappings) cfg.remaps.push_back({m.from, m.to});
  for (const std::string& r : s.sourceRoots) cfg.searchRoots.push_back(pathFromUtf8(r));
  m_locator->configure(std::move(cfg));
  m_appliedRemaps = s.sourcePathMappings;
  m_appliedRoots = s.sourceRoots;
  m_haveConfig = true;
  ++m_sourceGeneration;
}

void SourcePanel::issue(const PanelContext& context, const Spec& spec) {
  analysis::AggregationOptions options;
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
  m_message.clear();
  source::BuildInputs inputs;
  inputs.filter = common::toQueryFilter(spec.filter);
  inputs.key = *spec.key;
  inputs.locator = m_locator;
  analysis::QueryExecutor& executor = *context.queryExecutor;
  using Value = std::shared_ptr<const SourceView>;
  m_query.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [inputs](const analysis::QueryContext& qc, analysis::Cancel cancel) -> Result<Value> {
          return source::buildSourceView(qc, inputs, cancel);
        });
  });
}

void SourcePanel::update(const PanelContext& context) {
  if (!context.document) {
    if (m_haveDocument || m_view) resetDocumentState();
    return;
  }
  if (!m_haveDocument || context.documentGeneration != m_documentGeneration) {
    resetDocumentState();
    m_haveDocument = true;
    m_documentGeneration = context.documentGeneration;
  }
  const model::TraceModel& model = context.document->model();
  m_clockHz = model.header.clock_frequency_hz ? static_cast<double>(model.header.clock_frequency_hz) : 1e9;
  syncLocatorConfig(context);
  if (!context.queryExecutor || !context.selection) return;

  if (auto result = m_query.poll()) {
    if (result->ok()) {
      m_shown = m_requested;
      m_message.clear();
      applyView(result->value());
    } else {
      m_message = "Source view failed: " + result->error().toString();
    }
  }

  const Spec spec = currentSpec(context);
  if (!spec.key) {
    // Nothing selected: forget the view.
    if (m_view || m_haveRequested) {
      m_query.cancel();
      m_view.reset();
      m_rows.clear();
      m_lineRow.clear();
      m_selectedRow = SIZE_MAX;
      m_matches.clear();
      m_haveRequested = false;
      m_requested = {};
      m_shown = {};
    }
    return;
  }
  if (!m_haveRequested || !(spec == m_requested)) {
    bool defer = false;
    if (m_throttle && m_haveRequested && context.symbols) {
      Spec same = spec;
      same.symbolGeneration = m_requested.symbolGeneration;
      const symbols::SymbolProgress progress = context.symbols->progress();
      if (same == m_requested && progress.done < progress.total &&
          Clock::now() - m_lastIssue < kThrottleWhileResolving)
        defer = true;  // a burst of module events: ask once it settles
    }
    if (defer) {
      context.requestRedraw();
    } else {
      issue(context, spec);
    }
  }
  if (m_query.pending()) context.requestRedraw();
}

// ---------------------------------------------------------------------------------------------------------
// View state

SourcePanel::Mode SourcePanel::effectiveMode() const {
  if (!m_view) return m_mode;
  if (m_view->kind != ViewKind::Source) return Mode::Disassembly;
  if (m_mode == Mode::Mixed && !m_view->haveInstructions) return Mode::Source;
  if (m_mode == Mode::Disassembly && !m_view->haveInstructions) return Mode::Source;
  return m_mode;
}

void SourcePanel::applyView(std::shared_ptr<const SourceView> view) {
  const bool sameFunction = m_view && m_view->key == view->key && m_view->localPath == view->localPath &&
                            m_view->lines.size() == view->lines.size() && m_view->kind == view->kind;
  m_view = std::move(view);
  ++m_delivered;
  if (!sameFunction) {
    m_expanded.assign(m_view->file ? static_cast<size_t>(m_view->file->lineCount()) + 1 : 0, 0);
    m_selectedRow = SIZE_MAX;
  }
  rebuildRows();
  rebuildMatches();
  if (!sameFunction && m_view->kind != ViewKind::Message) {
    // First show of a function: start at its hottest line, else at its first line.
    size_t target = SIZE_MAX;
    if (effectiveMode() == Mode::Disassembly) {
      target = m_view->hottestInstruction != UINT32_MAX ? m_view->hottestInstruction : 0;
      if (m_rows.empty()) target = SIZE_MAX;
    } else if (m_view->hottestLine != 0) {
      target = rowOfLine(m_view->hottestLine);
    } else if (m_view->firstLine != 0) {
      target = rowOfLine(m_view->firstLine);
    }
    if (target != SIZE_MAX) {
      m_selectedRow = target;
      scrollToRow(target);
    }
  } else if (m_selectedRow != SIZE_MAX && m_selectedRow >= m_rows.size()) {
    m_selectedRow = SIZE_MAX;
  }
}

void SourcePanel::injectView(std::shared_ptr<const SourceView> view) {
  applyView(std::move(view));
}

void SourcePanel::rebuildRows() {
  m_rows.clear();
  m_lineRow.clear();
  if (!m_view || m_view->kind == ViewKind::Message) return;
  const SourceView& v = *m_view;
  const Mode mode = effectiveMode();
  if (mode == Mode::Disassembly) {
    m_rows.reserve(v.instructions.size());
    for (size_t i = 0; i < v.instructions.size(); ++i) m_rows.push_back({0, static_cast<int32_t>(i)});
    return;
  }
  const uint32_t count = v.file->lineCount();
  m_rows.reserve(count);
  m_lineRow.assign(static_cast<size_t>(count) + 1, 0);
  for (uint32_t l = 1; l <= count; ++l) {
    m_lineRow[l] = static_cast<uint32_t>(m_rows.size());
    m_rows.push_back({l, -1});
    if (mode == Mode::Mixed && lineExpanded(l) && l + 1 < v.lineInstrBegin.size()) {
      for (uint32_t k = v.lineInstrBegin[l]; k < v.lineInstrBegin[l + 1]; ++k)
        m_rows.push_back({l, static_cast<int32_t>(v.lineInstrOrder[k])});
    }
  }
}

size_t SourcePanel::rowOfLine(uint32_t line) const {
  if (effectiveMode() == Mode::Disassembly || line >= m_lineRow.size() || line == 0) return SIZE_MAX;
  return m_lineRow[line];
}

void SourcePanel::scrollToRow(size_t row) {
  m_scrollRow = static_cast<int64_t>(row);
}

void SourcePanel::setMode(Mode mode) {
  if (mode == m_mode) return;
  // Keep the same source line (or instruction) selected across the switch.
  const Row old = m_selectedRow < m_rows.size() ? m_rows[m_selectedRow] : Row{};
  const bool hadSelection = m_selectedRow < m_rows.size();
  const Mode before = effectiveMode();
  m_mode = mode;
  rebuildRows();
  rebuildMatches();
  if (!hadSelection || !m_view) return;
  size_t row = SIZE_MAX;
  if (effectiveMode() == Mode::Disassembly) {
    if (old.instr >= 0)
      row = static_cast<size_t>(old.instr);
    else if (before != Mode::Disassembly && m_view->hasLine(old.line) &&
             !m_view->instructionsOfLine(old.line).empty())
      row = m_view->instructionsOfLine(old.line).front();
  } else {
    row = rowOfLine(old.line);
    if (row == SIZE_MAX && old.instr >= 0 && static_cast<size_t>(old.instr) < m_view->instructions.size())
      row = rowOfLine(m_view->instructions[static_cast<size_t>(old.instr)].line);
  }
  m_selectedRow = row;
  if (row != SIZE_MAX) scrollToRow(row);
}

void SourcePanel::toggleLine(uint32_t line) {
  if (!m_view || m_view->kind != ViewKind::Source || effectiveMode() != Mode::Mixed) return;
  if (line == 0 || line >= m_expanded.size() || m_view->instructionCountOfLine(line) == 0) return;
  const size_t selectedBefore = m_selectedRow;
  const Row sel = selectedBefore < m_rows.size() ? m_rows[selectedBefore] : Row{};
  m_expanded[line] ^= 1;
  rebuildRows();
  // Keep the selected row selected (collapsing hides the instruction rows below a line).
  if (selectedBefore < SIZE_MAX && sel.line != 0) {
    size_t row = rowOfLine(sel.line);
    if (row != SIZE_MAX && sel.instr >= 0 && m_expanded[sel.line] != 0) {
      for (size_t r = row; r < m_rows.size() && m_rows[r].line == sel.line; ++r)
        if (m_rows[r].instr == sel.instr) row = r;
    }
    m_selectedRow = row;
  }
}

void SourcePanel::expandAll() {
  if (!m_view || m_view->kind != ViewKind::Source || effectiveMode() != Mode::Mixed) return;
  for (uint32_t l = 1; l < m_expanded.size(); ++l)
    m_expanded[l] = m_view->instructionCountOfLine(l) != 0 ? 1 : 0;
  rebuildRows();
  m_selectedRow = SIZE_MAX;
}

void SourcePanel::collapseAll() {
  std::fill(m_expanded.begin(), m_expanded.end(), uint8_t{0});
  const Row sel = m_selectedRow < m_rows.size() ? m_rows[m_selectedRow] : Row{};
  rebuildRows();
  m_selectedRow = sel.line != 0 ? rowOfLine(sel.line) : SIZE_MAX;
}

void SourcePanel::selectRow(size_t row) {
  m_selectedRow = row < m_rows.size() ? row : SIZE_MAX;
}

void SourcePanel::jumpToHottest() {
  if (!m_view || m_view->kind == ViewKind::Message) return;
  size_t row = SIZE_MAX;
  if (effectiveMode() == Mode::Disassembly) {
    if (m_view->hottestInstruction != UINT32_MAX) row = m_view->hottestInstruction;
  } else if (m_view->hottestLine != 0) {
    row = rowOfLine(m_view->hottestLine);
  }
  if (row == SIZE_MAX) return;
  m_selectedRow = row;
  scrollToRow(row);
}

void SourcePanel::reload() {
  m_locator->clearCache();
  ++m_sourceGeneration;
}

// ---------------------------------------------------------------------------------------------------------
// Search

void SourcePanel::openSearch() {
  m_searchOpen = true;
  m_searchFocus = true;
}

void SourcePanel::closeSearch() {
  m_searchOpen = false;
  m_searchFocus = false;
}

void SourcePanel::rebuildMatches() {
  m_matches.clear();
  if (m_searchText.empty() || !m_view || m_view->kind == ViewKind::Message) return;
  const SourceView& v = *m_view;
  if (effectiveMode() == Mode::Disassembly) {
    char addr[32];
    for (size_t i = 0; i < v.instructions.size() && m_matches.size() < kMaxSearchMatches; ++i) {
      std::snprintf(addr, sizeof addr, "%llx", static_cast<unsigned long long>(v.instructions[i].rva));
      if (source::findInsensitive(v.instructions[i].text, m_searchText) != std::string_view::npos ||
          source::findInsensitive(addr, m_searchText) != std::string_view::npos)
        m_matches.push_back(static_cast<uint32_t>(i));
    }
  } else if (v.file) {
    uint32_t l = 0;
    while (m_matches.size() < kMaxSearchMatches && (l = v.file->findNext(m_searchText, l + 1)) != 0)
      m_matches.push_back(l);
  }
}

void SourcePanel::jumpToMatch(size_t index) {
  if (index >= m_matches.size()) return;
  const uint32_t target = m_matches[index];
  const size_t row = effectiveMode() == Mode::Disassembly ? target : rowOfLine(target);
  if (row == SIZE_MAX || row >= m_rows.size()) return;
  m_selectedRow = row;
  scrollToRow(row);
}

void SourcePanel::setSearchText(const std::string& text) {
  if (text == m_searchText) return;
  m_searchText = text;
  rebuildMatches();
  if (m_matches.empty()) return;
  int64_t cur = -1;
  if (m_selectedRow < m_rows.size())
    cur = effectiveMode() == Mode::Disassembly ? m_rows[m_selectedRow].instr : m_rows[m_selectedRow].line;
  // First match at or after the selection; wraps to the first.
  auto it = std::lower_bound(m_matches.begin(), m_matches.end(), cur,
                             [](uint32_t m, int64_t c) { return static_cast<int64_t>(m) < c; });
  jumpToMatch(it == m_matches.end() ? 0 : static_cast<size_t>(it - m_matches.begin()));
}

void SourcePanel::searchNext() {
  if (m_matches.empty()) return;
  int64_t cur = -1;
  if (m_selectedRow < m_rows.size())
    cur = effectiveMode() == Mode::Disassembly ? m_rows[m_selectedRow].instr : m_rows[m_selectedRow].line;
  auto it = std::upper_bound(m_matches.begin(), m_matches.end(), cur,
                             [](int64_t c, uint32_t m) { return c < static_cast<int64_t>(m); });
  jumpToMatch(it == m_matches.end() ? 0 : static_cast<size_t>(it - m_matches.begin()));
}

void SourcePanel::searchPrevious() {
  if (m_matches.empty()) return;
  int64_t cur = -1;
  if (m_selectedRow < m_rows.size())
    cur = effectiveMode() == Mode::Disassembly ? m_rows[m_selectedRow].instr : m_rows[m_selectedRow].line;
  auto it = std::lower_bound(m_matches.begin(), m_matches.end(), cur,
                             [](uint32_t m, int64_t c) { return static_cast<int64_t>(m) < c; });
  jumpToMatch(it == m_matches.begin() ? m_matches.size() - 1
                                      : static_cast<size_t>(it - m_matches.begin()) - 1);
}

// ---------------------------------------------------------------------------------------------------------
// Locate file

void SourcePanel::beginLocate(const PanelContext& context) {
  if (!m_view) return;
  m_pickerDebugPath = m_view->debugPath;
  std::filesystem::path start;
  if (context.settings) {
    if (!context.settings->sourceRoots.empty())
      start = pathFromUtf8(context.settings->sourceRoots.back());
    else if (!context.settings->sourcePathMappings.empty())
      start = pathFromUtf8(context.settings->sourcePathMappings.back().to);
  }
  if (start.empty() && context.document) start = context.document->path().parent_path();
  m_picker.open(start);
}

void SourcePanel::locateFile(const std::filesystem::path& chosen, bool chosenIsDirectory) {
  const std::string debugPath =
      !m_pickerDebugPath.empty() ? m_pickerDebugPath : (m_view ? m_view->debugPath : "");
  const source::LocateOutcome outcome = source::locateOutcome(debugPath, chosen, chosenIsDirectory);
  if (outcome.rule) {
    if (m_host.addRemap) m_host.addRemap(*outcome.rule);
    if (m_host.toast)
      m_host.toast(ToastLevel::Info,
                   "Source path mapping added: " + outcome.rule->from + " -> " + outcome.rule->to);
  } else if (outcome.root) {
    if (m_host.addRoot) m_host.addRoot(*outcome.root);
    if (m_host.toast)
      m_host.toast(ToastLevel::Info, "Source search directory added: " + pathToUtf8(*outcome.root));
  }
  // Even when the settings did not change (the rule existed), look again.
  m_locator->clearCache();
  ++m_sourceGeneration;
}

// ---------------------------------------------------------------------------------------------------------
// Drawing

void SourcePanel::draw(const PanelContext& context, bool* open) {
  update(context);
  if (m_focusPending) {
    ImGui::SetNextWindowFocus();
    m_focusPending = false;
  }
  ImGui::SetNextWindowSize(ImVec2(900.0f, 520.0f), ImGuiCond_FirstUseEver);
  m_drawn.clear();
  m_locateButton = {};
  m_hadFocus = false;
  if (!ImGui::Begin(name(), open)) {
    ImGui::End();
    return;
  }
  m_hadFocus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && context.document != nullptr;
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

  // Keys of the focused panel: Ctrl+F opens the search, F3 / Shift+F3 and Esc work while it is open.
  if (m_hadFocus) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) openSearch();
    if (m_searchOpen) {
      if (ImGui::IsKeyChordPressed(ImGuiKey_F3)) searchNext();
      if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F3)) searchPrevious();
      if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) closeSearch();
    }
  }

  if (!m_view) {
    if (m_query.pending()) {
      common::drawSpinner(ImGui::GetFontSize() * 0.5f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
      ImGui::SameLine();
      ImGui::TextUnformatted("Loading source...");
    } else if (!m_message.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", m_message.c_str());
    } else {
      ImGui::TextDisabled("Select a function in the Call Graph or the Function List to see its source.");
    }
    ImGui::End();
    return;
  }

  drawToolbar(context);
  drawHeaderText(context);
  if (m_view->kind != ViewKind::Message) drawCode(context);

  // The Locate file dialog (its result is applied once).
  if (auto chosen = m_picker.draw()) {
    std::error_code ec;
    locateFile(*chosen, std::filesystem::is_directory(*chosen, ec));
  }
  ImGui::End();
}

void SourcePanel::drawToolbar(const PanelContext& context) {
  const SourceView& v = *m_view;
  const float em = ImGui::GetFontSize();
  const bool havePlace = v.kind != ViewKind::Message;
  const Mode eff = effectiveMode();
  ImGui::BeginDisabled(!havePlace || v.kind != ViewKind::Source);
  if (ImGui::RadioButton("Source", eff == Mode::Source)) setMode(Mode::Source);
  ImGui::SameLine();
  ImGui::BeginDisabled(!v.haveInstructions);
  if (ImGui::RadioButton("Mixed", eff == Mode::Mixed)) setMode(Mode::Mixed);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!v.haveInstructions);
  if (ImGui::RadioButton("Disassembly", eff == Mode::Disassembly)) setMode(Mode::Disassembly);
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  if (havePlace && v.kind == ViewKind::Disassembly) {
    ImGui::SameLine();
    ImGui::TextDisabled("(disassembly only)");
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!havePlace);
  if (ImGui::Button("Jump to hottest")) jumpToHottest();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(eff == Mode::Disassembly ? "Scroll to the instruction with the most time"
                                               : "Scroll to the source line with the most time");
  ImGui::SameLine();
  if (ImGui::Button("Search")) {
    if (m_searchOpen)
      closeSearch();
    else
      openSearch();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Search in the file (Ctrl+F while this panel is focused)");
  if (eff == Mode::Mixed) {
    ImGui::SameLine();
    if (ImGui::Button("Expand all")) expandAll();
    ImGui::SameLine();
    if (ImGui::Button("Collapse all")) collapseAll();
  }
  ImGui::EndDisabled();
  if (havePlace && v.resolved) {
    ImGui::SameLine();
    if (ImGui::Button("Locate file...")) beginLocate(context);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    m_locateButton = {a.x, a.y, b.x, b.y};
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Pick the file on this machine that corresponds to\n%s", v.debugPath.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Reload")) reload();
  }
  if (m_query.pending()) {
    ImGui::SameLine();
    common::drawSpinner(em * 0.4f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
  }

  if (m_searchOpen) {
    std::array<char, kSearchBufferSize> buf{};
    std::snprintf(buf.data(), buf.size(), "%s", m_searchText.c_str());
    if (m_searchFocus) {
      ImGui::SetKeyboardFocusHere();
      m_searchFocus = false;
    }
    ImGui::SetNextItemWidth(em * 16.0f);
    const bool entered = ImGui::InputTextWithHint("##srcsearch", "Search", buf.data(), buf.size(),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    const bool active = ImGui::IsItemActive();
    if (std::string(buf.data()) != m_searchText) setSearchText(buf.data());
    if (entered) {
      if (ImGui::GetIO().KeyShift)
        searchPrevious();
      else
        searchNext();
      m_searchFocus = true;  // Enter leaves the field; keep typing / pressing Enter
    }
    (void)active;
    ImGui::SameLine();
    if (ImGui::Button("Prev")) searchPrevious();
    ImGui::SameLine();
    if (ImGui::Button("Next")) searchNext();
    ImGui::SameLine();
    if (!m_searchText.empty())
      ImGui::Text("%zu match%s%s", m_matches.size(), m_matches.size() == 1 ? "" : "es",
                  m_matches.size() >= kMaxSearchMatches ? "+" : "");
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) closeSearch();
  }
}

void SourcePanel::drawHeaderText(const PanelContext&) {
  const SourceView& v = *m_view;
  const ImVec4 warn(1.0f, 0.75f, 0.3f, 1.0f);
  if (v.kind == ViewKind::Message) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", v.message.c_str());
    return;
  }
  const std::string title = v.moduleName.empty() ? v.functionName : v.moduleName + "!" + v.functionName;
  ImGui::TextUnformatted(title.c_str());
  ImGui::SameLine();
  const double totalMs = common::ticksToMs(
      v.kind == ViewKind::Source ? v.total.ticks : v.instructionTicks + v.otherTicks, m_clockHz);
  ImGui::TextDisabled("  %.3f ms", totalMs);
  if (v.kind == ViewKind::Source) {
    ImGui::SameLine();
    ImGui::TextDisabled("  %s", v.localPath.c_str());
    if (v.localPath != v.debugPath && !v.debugPath.empty() && ImGui::IsItemHovered())
      ImGui::SetTooltip("Debug info path: %s", v.debugPath.c_str());
  } else {
    ImGui::TextColored(warn, "%s", v.sourceProblem.c_str());
    if (!v.haveInstructions && !v.instructionsProblem.empty())
      ImGui::TextColored(warn, "%s", v.instructionsProblem.c_str());
  }
  if (!v.note.empty()) ImGui::TextColored(warn, "%s", v.note.c_str());
  if (v.kind == ViewKind::Source) {
    if (v.pastEndTicks != 0)
      ImGui::TextColored(warn, "%.3f ms are on lines beyond the end of this file: it differs from the build.",
                         common::ticksToMs(v.pastEndTicks, m_clockHz));
    if (!v.haveInstructions && !v.instructionsProblem.empty())
      ImGui::TextDisabled("No assembly: %s", v.instructionsProblem.c_str());
  }
  if (v.unknownLineTicks != 0)
    ImGui::TextDisabled("%.3f ms have no line information.",
                        common::ticksToMs(v.unknownLineTicks, m_clockHz));
}

void SourcePanel::drawTimeCells(const RowContext& c, float x, const LineTime& t, uint64_t denominator) const {
  if (t.ticks == 0) return;
  const Layout& L = c.layout;
  const float y0 = c.y0 + 2.0f, y1 = c.y0 + c.pitch - 2.0f;
  const double denom = denominator ? static_cast<double>(denominator) : 1.0;
  const float barW = static_cast<float>(L.bar - 1) * c.charW;
  const float w = std::max(1.0f, barW * static_cast<float>(static_cast<double>(t.ticks) / denom));
  c.dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + std::min(w, barW), y1), kBarColor);
  float cx = x + static_cast<float>(L.bar) * c.charW;
  if (!L.disassembly) {
    const float miniW = static_cast<float>(L.mini - 1) * c.charW;
    float mx = cx;
    const double tt = static_cast<double>(t.ticks);
    const std::array<std::pair<uint64_t, ImU32>, 3> parts{
        {{t.running, kRunningColor}, {t.ready, kReadyColor}, {t.waiting, kWaitingColor}}};
    for (const auto& [ticks, color] : parts) {
      if (ticks == 0) continue;
      const float pw = std::max(1.0f, miniW * static_cast<float>(static_cast<double>(ticks) / tt));
      c.dl->AddRectFilled(ImVec2(mx, y0), ImVec2(std::min(mx + pw, cx + miniW), y1), color);
      mx += pw;
    }
    cx += static_cast<float>(L.mini) * c.charW;
  }
  char buf[48];
  std::snprintf(buf, sizeof buf, "%5.1f%%", 100.0 * static_cast<double>(t.ticks) / denom);
  c.dl->AddText(ImVec2(cx, c.y0 + 1.0f), c.textColor, buf);
  cx += static_cast<float>(L.pct) * c.charW;
  std::snprintf(buf, sizeof buf, "%8.3f", common::ticksToMs(t.ticks, m_clockHz));
  c.dl->AddText(ImVec2(cx, c.y0 + 1.0f), c.textColor, buf);
}

void SourcePanel::drawRow(const RowContext& c, size_t rowIndex) {
  const SourceView& v = *c.view;
  const Row& row = m_rows[rowIndex];
  const Layout& L = c.layout;
  ImDrawList* dl = c.dl;
  const float y0 = c.y0, y1 = c.y0 + c.pitch;
  const bool isInstruction = row.instr >= 0;
  const InstructionRow* ins = isInstruction ? &v.instructions[static_cast<size_t>(row.instr)] : nullptr;
  LineTime time;
  uint64_t denominator = 1, maxTicks = 0;
  if (ins) {
    time = LineTime{ins->ticks, ins->ticks, 0, 0};
    denominator = c.instrDenominator;
    maxTicks = c.maxInstrTicks;
  } else {
    time = v.lines[row.line];
    denominator = v.total.ticks ? v.total.ticks : 1;
    maxTicks = c.maxLineTicks;
  }

  // Background: function extent tint, heat, selection.
  const ImVec2 bgMin(c.fixedLeft, y0), bgMax(c.visRight, y1);
  if (!ins && v.firstLine != 0 && row.line >= v.firstLine && row.line <= v.lastLine)
    dl->AddRectFilled(bgMin, bgMax, IM_COL32(128, 128, 170, 16));
  if (ins && c.mode == Mode::Mixed) dl->AddRectFilled(bgMin, bgMax, IM_COL32(100, 140, 200, 18));
  if (time.ticks != 0) dl->AddRectFilled(bgMin, bgMax, heatColor(source::heatOf(time.ticks, maxTicks)));
  if (rowIndex == m_selectedRow) dl->AddRectFilled(bgMin, bgMax, IM_COL32(90, 140, 255, 55));

  // Code column (clipped to the right of the fixed columns).
  const float codeX = c.originX + c.fixedW;
  dl->PushClipRect(ImVec2(c.fixedLeft + c.fixedW, y0), ImVec2(c.visRight, y1), true);
  char buf[160];
  if (!ins) {
    const std::string_view text = v.file->line(row.line);
    const std::string shown = source::expandForDisplay(text, c.firstCol + c.visCols + 2);
    if (!m_searchText.empty() && effectiveMode() != Mode::Disassembly) {
      size_t at = 0;
      while ((at = source::findInsensitive(text, m_searchText, at)) != std::string_view::npos) {
        const uint32_t col0 = source::displayColumn(text, at);
        const uint32_t col1 = source::displayColumn(text, at + m_searchText.size());
        const bool current = rowIndex == m_selectedRow;
        dl->AddRectFilled(ImVec2(codeX + static_cast<float>(col0) * c.charW, y0 + 1),
                          ImVec2(codeX + static_cast<float>(col1) * c.charW, y1 - 1),
                          current ? IM_COL32(255, 200, 0, 150) : IM_COL32(255, 220, 0, 80));
        at += std::max<size_t>(m_searchText.size(), 1);
        if (col0 > c.firstCol + c.visCols + 2) break;
      }
    }
    if (c.firstCol < shown.size())
      dl->AddText(ImVec2(codeX + static_cast<float>(c.firstCol) * c.charW, y0 + 1.0f), c.textColor,
                  shown.c_str() + c.firstCol, shown.c_str() + shown.size());
  } else {
    // "  0x1230  48 89 e5      mov rbp, rsp   <- after call"
    std::string bytes;
    for (size_t i = 0; i < ins->length && i < 8; ++i) {
      std::snprintf(buf, sizeof buf, "%02x ", ins->bytes[i]);
      bytes += buf;
    }
    if (ins->length > 8) bytes += "..";
    std::snprintf(buf, sizeof buf, "%s%08llx  %-25s  %s", c.mode == Mode::Mixed ? "    " : "",
                  static_cast<unsigned long long>(ins->rva), bytes.c_str(), ins->text.c_str());
    if (c.mode != Mode::Mixed) {
      // The disassembly keeps the address and bytes in the fixed columns; only the text scrolls.
      std::snprintf(buf, sizeof buf, "%s", ins->text.c_str());
    }
    std::string text = buf;
    if (ins->afterCall && ins->ticks != 0) text += "    ; after call";
    if (!ins->valid) text += "    ; undecodable";
    if (!m_searchText.empty() && c.mode == Mode::Disassembly) {
      size_t at = source::findInsensitive(ins->text, m_searchText);
      if (at != std::string_view::npos)
        dl->AddRectFilled(ImVec2(codeX + static_cast<float>(at) * c.charW, y0 + 1),
                          ImVec2(codeX + static_cast<float>(at + m_searchText.size()) * c.charW, y1 - 1),
                          rowIndex == m_selectedRow ? IM_COL32(255, 200, 0, 150) : IM_COL32(255, 220, 0, 80));
    }
    const std::string shown = source::expandForDisplay(text, c.firstCol + c.visCols + 2);
    if (c.firstCol < shown.size())
      dl->AddText(ImVec2(codeX + static_cast<float>(c.firstCol) * c.charW, y0 + 1.0f), c.textColor,
                  shown.c_str() + c.firstCol, shown.c_str() + shown.size());
  }
  dl->PopClipRect();

  // Fixed columns.
  float x = c.fixedLeft;
  const float cw = c.charW;
  if (c.mode == Mode::Disassembly) {
    std::snprintf(buf, sizeof buf, "%08llx", static_cast<unsigned long long>(ins->rva));
    dl->AddText(ImVec2(x, y0 + 1.0f), c.dimColor, buf);
    x += static_cast<float>(L.addr) * cw;
    std::string bytes;
    for (size_t i = 0; i < ins->length && i < 6; ++i) {
      std::snprintf(buf, sizeof buf, "%02x ", ins->bytes[i]);
      bytes += buf;
    }
    if (ins->length > 6) bytes += "..";
    dl->AddText(ImVec2(x, y0 + 1.0f), c.dimColor, bytes.c_str());
    x += static_cast<float>(L.bytes) * cw;
    drawTimeCells(c, x, time, denominator);
    x += static_cast<float>(L.bar + L.pct + L.ms) * cw;
    if (ins->line != 0) {
      std::snprintf(buf, sizeof buf, "L%u", ins->line);
      dl->AddText(ImVec2(x, y0 + 1.0f), c.dimColor, buf);
    }
  } else {
    // Expander cell.
    if (!ins && c.mode == Mode::Mixed && v.instructionCountOfLine(row.line) != 0)
      dl->AddText(ImVec2(x + cw * 0.3f, y0 + 1.0f), c.textColor, lineExpanded(row.line) ? "v" : ">");
    x += static_cast<float>(L.expander) * cw;
    if (!ins) {
      std::snprintf(buf, sizeof buf, "%6u", row.line);
      dl->AddText(ImVec2(x, y0 + 1.0f), c.dimColor, buf);
    } else if (ins->afterCall && ins->ticks != 0) {
      dl->AddText(ImVec2(x, y0 + 1.0f), c.dimColor, "  call>");
    }
    x += static_cast<float>(L.line) * cw;
    drawTimeCells(c, x, time, denominator);
  }
  // Separator between the fixed and the scrolling columns.
  dl->AddLine(ImVec2(c.fixedLeft + c.fixedW - 2.0f, y0), ImVec2(c.fixedLeft + c.fixedW - 2.0f, y1),
              IM_COL32(128, 128, 128, 60));
}

void SourcePanel::drawCode(const PanelContext& context) {
  const SourceView& v = *m_view;
  const bool stale = m_query.pending();
  if (stale) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);

  RowContext c;
  c.view = &v;
  c.mode = effectiveMode();
  c.layout.disassembly = c.mode == Mode::Disassembly;
  c.charW = ImGui::CalcTextSize("0").x;
  m_charW = c.charW;
  c.pitch = ImGui::GetTextLineHeight() + kPitchPadding;
  c.fixedW = static_cast<float>(c.layout.fixedChars()) * c.charW;
  c.textColor = ImGui::GetColorU32(ImGuiCol_Text);
  c.dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  if (v.hottestLine != 0 && v.hasLine(v.hottestLine)) c.maxLineTicks = v.lines[v.hottestLine].ticks;
  if (v.hottestInstruction != UINT32_MAX) c.maxInstrTicks = v.instructions[v.hottestInstruction].ticks;
  c.instrDenominator = std::max<uint64_t>(1, v.instructionTicks + v.otherTicks);

  // Column titles, aligned with the fixed columns of the child below.
  const ImVec2 headerPos = ImGui::GetCursorScreenPos();
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = c.charW;
    float x = headerPos.x;
    const ImU32 dim = c.dimColor;
    if (c.layout.disassembly) {
      dl->AddText(ImVec2(x, headerPos.y), dim, "Address");
      x += static_cast<float>(c.layout.addr) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "Bytes");
      x += static_cast<float>(c.layout.bytes) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "Time");
      x += static_cast<float>(c.layout.bar) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "     %");
      x += static_cast<float>(c.layout.pct) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "      ms");
      x += static_cast<float>(c.layout.ms) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "Line");
      dl->AddText(ImVec2(headerPos.x + c.fixedW, headerPos.y), dim, "Instruction");
    } else {
      x += static_cast<float>(c.layout.expander) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "  Line");
      x += static_cast<float>(c.layout.line) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "Time");
      x += static_cast<float>(c.layout.bar) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "R/Rd/W");
      x += static_cast<float>(c.layout.mini) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "     %");
      x += static_cast<float>(c.layout.pct) * cw;
      dl->AddText(ImVec2(x, headerPos.y), dim, "      ms");
      dl->AddText(ImVec2(headerPos.x + c.fixedW, headerPos.y), dim, "Source");
    }
    ImGui::Dummy(ImVec2(0.0f, c.pitch));
  }

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
  const bool visible = ImGui::BeginChild("##sourcecode", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                                         ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar(2);  // the child keeps the padding it started with; rows set their own spacing
  if (visible) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    const float scrollX = ImGui::GetScrollX();
    const float visibleW = ImGui::GetWindowWidth();
    const float visibleH = ImGui::GetWindowHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    c.dl = ImGui::GetWindowDrawList();
    c.originX = origin.x;
    c.fixedLeft = origin.x + scrollX;
    c.visRight = c.fixedLeft + visibleW;
    c.firstCol = scrollX > 0.0f ? static_cast<size_t>(scrollX / c.charW) : 0;
    c.visCols = static_cast<size_t>(visibleW / c.charW) + 2;

    // Content width: the widest line, or the widest instruction text.
    uint32_t widest = 0;
    if (c.mode == Mode::Disassembly) {
      widest = 80;
    } else {
      widest = v.file->maxColumns();
      if (c.mode == Mode::Mixed) widest = std::max<uint32_t>(widest, 90);
    }
    const float contentW = c.fixedW + static_cast<float>(widest + 4) * c.charW;

    if (m_scrollRow >= 0) {
      const float target = static_cast<float>(m_scrollRow) * c.pitch - (visibleH - c.pitch) * 0.5f;
      ImGui::SetScrollY(std::max(0.0f, target));
      context.requestRedraw();
      m_scrollRow = -1;
    }
    // Keyboard: arrows / page keys move the selection of the focused panel.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        !m_rows.empty()) {
      const int page = std::max(1, static_cast<int>(visibleH / c.pitch) - 1);
      int step = 0;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        step = 1;
      else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        step = -1;
      else if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
        step = page;
      else if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        step = -page;
      if (step != 0) {
        const int64_t cur = m_selectedRow < m_rows.size() ? static_cast<int64_t>(m_selectedRow) : -1;
        const int64_t next = std::clamp<int64_t>(cur + step, 0, static_cast<int64_t>(m_rows.size()) - 1);
        m_selectedRow = static_cast<size_t>(next);
        if (m_firstVisible != SIZE_MAX &&
            (m_selectedRow < m_firstVisible + 1 || m_selectedRow + 1 > m_lastVisible))
          scrollToRow(m_selectedRow);
      }
    }

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_rows.size()), c.pitch);
    size_t first = SIZE_MAX, last = SIZE_MAX;
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        c.y0 = p.y;
        drawRow(c, static_cast<size_t>(i));
        m_drawn.push_back({static_cast<size_t>(i), c.fixedLeft, p.y, c.visRight, p.y + c.pitch, c.fixedLeft,
                           c.fixedLeft + static_cast<float>(c.layout.expander) * c.charW});
        ImGui::Dummy(ImVec2(contentW, c.pitch));
        if (first == SIZE_MAX) first = static_cast<size_t>(i);
        last = static_cast<size_t>(i);
      }
    }
    m_firstVisible = first;
    m_lastVisible = last;
    ImGui::PopStyleVar();

    // Mouse: the expander toggles a line, anywhere else selects the row; the fixed columns show a tooltip.
    if (ImGui::IsWindowHovered()) {
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      for (const DrawnRow& d : m_drawn) {
        if (mouse.y < d.y0 || mouse.y >= d.y1) continue;
        const bool inFixed = mouse.x >= d.fixedX && mouse.x < d.fixedX + c.fixedW;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          const Row& row = m_rows[d.row];
          if (c.mode == Mode::Mixed && row.instr < 0 && mouse.x < d.expanderX1 &&
              v.instructionCountOfLine(row.line) != 0) {
            selectRow(d.row);
            toggleLine(row.line);
          } else {
            selectRow(d.row);
          }
        }
        if (inFixed &&
            mouse.x >= d.fixedX + (c.layout.disassembly
                                       ? 0.0f
                                       : static_cast<float>(c.layout.expander + c.layout.line) * c.charW)) {
          const Row& row = m_rows[d.row];
          if (row.instr >= 0) {
            const InstructionRow& in = v.instructions[static_cast<size_t>(row.instr)];
            if (in.ticks != 0) {
              ImGui::SetTooltip(
                  "%.3f ms (%.1f%% of the function's samples)%s", common::ticksToMs(in.ticks, m_clockHz),
                  100.0 * static_cast<double>(in.ticks) / static_cast<double>(c.instrDenominator),
                  in.afterCall ? "\nafter call: samples here are return addresses of the call above" : "");
            }
          } else if (v.hasLine(row.line) && v.lines[row.line].ticks != 0) {
            const LineTime t = v.lines[row.line];
            ImGui::SetTooltip("Line %u: %.3f ms (%.1f%%)\nrunning %.3f ms, ready %.3f ms, waiting %.3f ms",
                              row.line, common::ticksToMs(t.ticks, m_clockHz),
                              100.0 * static_cast<double>(t.ticks) /
                                  static_cast<double>(std::max<uint64_t>(1, v.total.ticks)),
                              common::ticksToMs(t.running, m_clockHz), common::ticksToMs(t.ready, m_clockHz),
                              common::ticksToMs(t.waiting, m_clockHz));
          }
        }
        break;
      }
    }
  }
  ImGui::EndChild();
  if (stale) ImGui::PopStyleVar();
}

}  // namespace tachyon::viewer
