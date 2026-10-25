#include "viewer/functions/butterfly_panel.h"

#include "viewer/builtin_panels.h"
#include "viewer/common/function_names.h"
#include "viewer/common/imgui_util.h"
#include "viewer/common/query_filter.h"
#include "viewer/document.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace tachyon::viewer {

namespace fl = functions;
using common::drawSpinner;
using common::textRight;
using symbols::FunctionKey;

namespace {

constexpr auto kThrottleWhileResolving = std::chrono::milliseconds(200);

}  // namespace

ButterflyPanel::ButterflyPanel() = default;
ButterflyPanel::~ButterflyPanel() = default;

// ---------------------------------------------------------------------------------------------------------
// Lifecycle and bus

void ButterflyPanel::resetDocumentState() {
  m_query.cancel();
  m_data.reset();
  m_items.clear();
  m_menuLine.reset();
  m_openMenu = false;
  m_haveRequested = false;
  m_requested = {};
  m_shown = {};
  m_message.clear();
  m_drawnRows.clear();
  m_haveDocument = false;
}

void ButterflyPanel::onDocumentChanged(const Document*) {
  resetDocumentState();
}

void ButterflyPanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                        SelectionField field) {
  if (field == SelectionField::PanelRequest && state.panelRequest.name == kName) {
    m_focusPending = true;
    ++m_focusRequests;
  }
  if (field == SelectionField::SelectedFunction || field == SelectionField::TimeFilter ||
      field == SelectionField::All)
    context.requestRedraw();
}

ButterflyPanel::QuerySpec ButterflyPanel::currentSpec(const PanelContext& context) const {
  QuerySpec spec;
  if (!context.selection) return spec;
  const SelectionState& state = context.selection->state();
  spec.key = state.selectedFunction;
  if (spec.key && *spec.key == analysis::kRootKey) spec.key.reset();
  spec.base.filter = fl::makeFilter(state.timeFilter, m_restrictSubtree);
  const analysis::AggregationOptions options = fl::effectiveOptions(m_inline, m_kernel, spec.key);
  spec.base.expandInline = options.expandInline;
  spec.base.kernelFrames = options.kernelFrames;
  spec.base.documentGeneration = context.documentGeneration;
  spec.base.symbolGeneration = context.symbolGeneration;
  return spec;
}

void ButterflyPanel::issue(const PanelContext& context, const QuerySpec& spec) {
  analysis::AggregationOptions options;
  options.expandInline = spec.base.expandInline;
  options.kernelFrames = spec.base.kernelFrames;
  const Document& doc = *context.document;
  std::shared_ptr<const AnalysisSnapshot> snapshot =
      doc.analysis().snapshot(options, spec.base.symbolGeneration, context.queryExecutor);
  if (!snapshot) {
    m_message = "Analysis is not available for this document.";
    return;
  }
  m_requested = spec;
  m_haveRequested = true;
  m_lastIssue = Clock::now();
  const model::TraceModel& model = doc.model();
  const double clockHz =
      model.header.clock_frequency_hz ? static_cast<double>(model.header.clock_frequency_hz) : 1e9;
  analysis::QueryExecutor& executor = *context.queryExecutor;
  const analysis::Filter filter = spec.base.filter;
  const FunctionKey key = *spec.key;
  using Value = std::shared_ptr<const Data>;
  m_query.issue(executor, [&](const analysis::SubmitOptions& so) {
    return doc.analysis().submit<Value>(
        executor, so, snapshot,
        [filter, key, clockHz](const analysis::QueryContext& qc,
                               analysis::Cancel cancel) -> tachyon::Result<Value> {
          auto bf = analysis::butterflyShared(qc, filter, key, cancel);
          if (!bf.ok()) return bf.error();
          auto data = std::make_shared<Data>();
          data->clockHz = clockHz;
          data->view = fl::buildButterflyView(
              **bf, [&qc](const FunctionKey& k) { return common::splitFunctionName(&qc, k); });
          return Value(std::move(data));
        });
  });
}

void ButterflyPanel::update(const PanelContext& context) {
  if (!context.document) {
    if (m_haveDocument || m_data) resetDocumentState();
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
      applyData(result->value());
    } else {
      m_message = "Query failed: " + result->error().toString();
    }
  }

  const QuerySpec spec = currentSpec(context);
  if (!spec.key) {
    // Nothing selected: nothing to show and nothing to wait for.
    m_query.cancel();
    if (m_data) {
      m_data.reset();
      m_items.clear();
    }
    m_haveRequested = false;
    m_requested = {};
    m_shown = {};
    return;
  }
  if (!m_haveRequested || !(spec == m_requested)) {
    bool defer = false;
    if (m_throttle && m_haveRequested && context.symbols) {
      QuerySpec same = spec;
      same.base.symbolGeneration = m_requested.base.symbolGeneration;
      const symbols::SymbolProgress progress = context.symbols->progress();
      if (same == m_requested && progress.done < progress.total &&
          Clock::now() - m_lastIssue < kThrottleWhileResolving)
        defer = true;
    }
    if (defer) {
      context.requestRedraw();
    } else {
      issue(context, spec);
    }
  }
  if (m_query.pending()) context.requestRedraw();
}

void ButterflyPanel::applyData(std::shared_ptr<const Data> data) {
  m_data = std::move(data);
  ++m_delivered;
  m_items.clear();
  if (!m_data) return;
  const fl::ButterflyView& v = m_data->view;
  m_items.push_back({Item::Kind::CallersHeader, 0});
  for (uint32_t i = 0; i < v.callers.size(); ++i) m_items.push_back({Item::Kind::Caller, i});
  m_items.push_back({Item::Kind::Self, 0});
  m_items.push_back({Item::Kind::CalleesHeader, 0});
  for (uint32_t i = 0; i < v.callees.size(); ++i) m_items.push_back({Item::Kind::Callee, i});
}

void ButterflyPanel::injectData(std::shared_ptr<const Data> data) {
  applyData(std::move(data));
}

const fl::Line* ButterflyPanel::lineOf(const Item& item) const {
  if (!m_data) return nullptr;
  const fl::ButterflyView& v = m_data->view;
  switch (item.kind) {
    case Item::Kind::Caller: return &v.callers[item.index];
    case Item::Kind::Callee: return &v.callees[item.index];
    case Item::Kind::Self: return &v.self;
    default: return nullptr;
  }
}

// ---------------------------------------------------------------------------------------------------------
// Actions

bool ButterflyPanel::navigate(const PanelContext& context, const fl::Line& line) {
  const std::optional<FunctionKey> target = fl::navigationTarget(line.key);
  if (!target || !context.selection) return false;
  context.selection->setSelectedFunction(*target);
  return true;
}

void ButterflyPanel::showInCallGraph(const PanelContext& context, const fl::Line& line) {
  if (!navigate(context, line)) return;
  context.selection->requestPanel(kCallGraphPanel);
}

void ButterflyPanel::showSource(const PanelContext& context, const fl::Line& line) {
  if (!navigate(context, line)) return;
  context.selection->requestPanel(kSourcePanel);
}

void ButterflyPanel::copyName(const fl::Line& line) {
  ImGui::SetClipboardText(line.name.full.c_str());
}

// ---------------------------------------------------------------------------------------------------------
// Drawing

void ButterflyPanel::draw(const PanelContext& context, bool* open) {
  update(context);
  if (m_focusPending) {
    ImGui::SetNextWindowFocus();
    m_focusPending = false;
  }
  ImGui::SetNextWindowSize(ImVec2(760.0f, 420.0f), ImGuiCond_FirstUseEver);
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
  if (!context.selection->state().selectedFunction) {
    ImGui::TextDisabled("Select a function (Function List, Call Graph) to see its callers and callees.");
  } else if (m_data) {
    drawTable(context, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 4.0f));
  } else if (m_query.pending()) {
    drawSpinner(ImGui::GetFontSize() * 0.5f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
    ImGui::SameLine();
    ImGui::TextUnformatted("Building butterfly...");
  } else {
    ImGui::TextDisabled("%s", m_message.empty() ? "No data." : m_message.c_str());
  }
  drawContextMenu(context);
  ImGui::End();
}

void ButterflyPanel::drawToolbar(const PanelContext& context) {
  bool restrict = m_restrictSubtree;
  if (ImGui::Checkbox("Call graph subtree", & restrict)) setRestrictToSubtree(restrict);
  ImGui::SameLine();
  bool inl = m_inline;
  if (ImGui::Checkbox("Inline frames", &inl)) setInlineExpansion(inl);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
  static constexpr const char* kKernelNames[] = {"Kernel: show", "Kernel: group", "Kernel: hide"};
  int kernel = static_cast<int>(m_kernel);
  if (ImGui::Combo("##bfkernel", &kernel, kKernelNames, 3))
    setKernelFrames(static_cast<analysis::KernelFrames>(kernel));
  if (m_query.pending()) {
    ImGui::SameLine();
    drawSpinner(ImGui::GetFontSize() * 0.4f, ImGui::GetColorU32(ImGuiCol_PlotHistogram));
  }
  (void)context;
  if (!m_message.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "  %s", m_message.c_str());
  }
}

void ButterflyPanel::drawTable(const PanelContext& context, float height) {
  const bool stale = m_query.pending();
  if (stale) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                ImGuiTableFlags_SizingFixedFit;
  const float em = ImGui::GetFontSize();
  const double clockHz = m_data->clockHz;
  const fl::ButterflyView& view = m_data->view;
  std::optional<fl::Line> clicked, rightClicked;
  if (ImGui::BeginTable("##butterfly", 7, flags, ImVec2(0.0f, height))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 5.0f);
    ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, em * 8.0f);
    ImGui::TableSetupColumn("Inclusive", ImGuiTableColumnFlags_WidthFixed, em * 6.5f);
    ImGui::TableSetupColumn("Incl %", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableSetupColumn("Exclusive", ImGuiTableColumnFlags_WidthFixed, em * 6.5f);
    ImGui::TableSetupColumn("Excl %", ImGuiTableColumnFlags_WidthFixed, em * 4.5f);
    ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
    ImGui::TableHeadersRow();

    const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    char buf[64];
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(m_items.size()), rowH);
    while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
        const Item& item = m_items[static_cast<size_t>(i)];
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushID(i);
        const ImVec2 cell = ImGui::GetCursorScreenPos();
        const float selectableH = rowH - ImGui::GetStyle().CellPadding.y * 2.0f;
        if (item.kind == Item::Kind::CallersHeader || item.kind == Item::Kind::CalleesHeader) {
          const bool callers = item.kind == Item::Kind::CallersHeader;
          const size_t n = callers ? view.callers.size() : view.callees.size();
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
          ImGui::TextDisabled("%s (%zu)", callers ? "Callers" : "Callees", n);
          ImGui::PopID();
          continue;
        }
        const fl::Line& line = *lineOf(item);
        const bool self = item.kind == Item::Kind::Self;
        ImGui::Selectable("##line", self, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, selectableH));
        const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        m_drawnRows.push_back({static_cast<size_t>(i), rmin.x, rmin.y, rmax.x, rmax.y});
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !self) clicked = line;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) rightClicked = line;
        ImGui::SetCursorScreenPos(ImVec2(cell.x + (self ? 0.0f : em), cell.y));
        if (line.navigable) {
          ImGui::TextUnformatted(line.name.function.c_str());
        } else {
          ImGui::TextDisabled("%s", line.name.function.c_str());
        }
        if (line.name.inlined) {
          ImGui::SameLine(0.0f, 4.0f);
          ImGui::TextDisabled("[inline]");
        }
        ImGui::PopID();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(line.name.module.c_str());
        ImGui::TableSetColumnIndex(2);
        std::snprintf(buf, sizeof buf, "%.3f ms", common::ticksToMs(line.inclusiveTicks, clockHz));
        textRight(buf);
        ImGui::TableSetColumnIndex(3);
        std::snprintf(buf, sizeof buf, "%.1f%%", line.inclusivePercent);
        textRight(buf);
        ImGui::TableSetColumnIndex(4);
        std::snprintf(buf, sizeof buf, "%.3f ms", common::ticksToMs(line.exclusiveTicks, clockHz));
        textRight(buf);
        ImGui::TableSetColumnIndex(5);
        std::snprintf(buf, sizeof buf, "%.1f%%", line.exclusivePercent);
        textRight(buf);
        ImGui::TableSetColumnIndex(6);
        std::snprintf(buf, sizeof buf, "%u", line.instances);
        textRight(buf);
      }
    }
    ImGui::EndTable();
  }
  if (stale) ImGui::PopStyleVar();
  if (clicked) navigate(context, *clicked);
  if (rightClicked) {
    m_menuLine = *rightClicked;
    m_openMenu = true;
  }
}

void ButterflyPanel::drawContextMenu(const PanelContext& context) {
  if (m_openMenu) {
    ImGui::OpenPopup("##bfmenu");
    m_openMenu = false;
  }
  if (!ImGui::BeginPopup("##bfmenu")) return;
  if (m_menuLine) {
    const fl::Line line = *m_menuLine;
    ImGui::TextDisabled("%s", line.name.full.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Show in Call Graph", nullptr, false, line.navigable)) showInCallGraph(context, line);
    if (ImGui::MenuItem("Show source", nullptr, false, line.navigable)) showSource(context, line);
    if (ImGui::MenuItem("Copy name")) copyName(line);
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------------------

std::unique_ptr<IPanel> makeButterflyPanel() {
  return std::make_unique<ButterflyPanel>();
}

}  // namespace tachyon::viewer
