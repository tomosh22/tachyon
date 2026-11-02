#include "viewer/status/symbols_panel.h"

#include "viewer/document.h"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace tachyon::viewer {

SymbolsPanel::SymbolsPanel(SymbolsPanelHost host) : m_host(std::move(host)) {}

std::unique_ptr<IPanel> makeSymbolsPanel(SymbolsPanelHost host) {
  return std::make_unique<SymbolsPanel>(std::move(host));
}

void SymbolsPanel::onDocumentChanged(const Document*) {
  m_haveDocument = false;
  m_referenced.clear();
  m_rows.clear();
  m_summary = {};
}

void SymbolsPanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                      SelectionField field) {
  if (field == SelectionField::PanelRequest && state.panelRequest.name == kName) {
    m_focus = true;
    ++m_focusRequests;
    context.requestRedraw();
  }
}

void SymbolsPanel::update(const PanelContext& context) {
  if (!context.document || !context.symbols) {
    if (m_haveDocument) onDocumentChanged(nullptr);
    return;
  }
  if (!m_haveDocument || context.documentGeneration != m_generation) {
    m_haveDocument = true;
    m_generation = context.documentGeneration;
    m_referenced = referencedModules(context.document->model());
  }
  m_summary = summarizeSymbols(*context.symbols, m_referenced);
  m_rows =
      moduleSymbolRows(context.document->model(), *context.symbols, m_referenced, /*onlyUnresolved=*/true);
}

void SymbolsPanel::copyModuleInfo(const PanelContext& context, const ModuleSymbolRow& row) {
  if (!context.document || !row.module.valid() ||
      row.module.get() >= context.document->model().modules.size())
    return;
  const std::string text = moduleInfoText(context.document->model().modules[row.module.get()], row);
  ImGui::SetClipboardText(text.c_str());
  if (m_host.toast) m_host.toast(ToastLevel::Info, "Copied module info for " + row.name);
}

void SymbolsPanel::draw(const PanelContext& context, bool* open) {
  update(context);
  if (m_focus) {
    ImGui::SetNextWindowFocus();
    m_focus = false;
  }
  ImGui::SetNextWindowSize(ImVec2(680.0f, 320.0f), ImGuiCond_FirstUseEver);
  m_addButton = {};
  if (!ImGui::Begin(name(), open)) {
    ImGui::End();
    return;
  }
  if (!context.document || !context.symbols) {
    ImGui::TextDisabled("No trace open.");
    ImGui::End();
    return;
  }
  if (ImGui::Button("Add symbol path...") && m_host.addSymbolPath) m_host.addSymbolPath();
  const ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
  m_addButton = {bmin.x, bmin.y, bmax.x, bmax.y};
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Choose a directory (searched for symbol files and *.symbols.json) or a symbols file.\n"
        "Resolution then runs again for all modules.");
  ImGui::SameLine();
  if (ImGui::Button("Retry all") && m_host.retry) m_host.retry();
  ImGui::SameLine();
  ImGui::Text("%u of %u modules resolved, %u failed", m_summary.resolved, m_summary.total, m_summary.failed);

  if (m_rows.empty()) {
    ImGui::TextDisabled(m_summary.running() ? "Resolving symbols..." : "All modules are resolved.");
  } else {
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_SizingFixedFit;
    const float em = ImGui::GetFontSize();
    const float tableH = std::max(em * 4.0f, ImGui::GetContentRegionAvail().y - em * 4.0f);
    if (ImGui::BeginTable("##unresolved", 4, kFlags, ImVec2(0.0f, tableH))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Module", ImGuiTableColumnFlags_WidthFixed, em * 14.0f);
      ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, em * 6.0f);
      ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, em * 14.0f);
      ImGui::TableHeadersRow();
      std::optional<ModuleSymbolRow> copy;
      bool retry = false;
      for (size_t i = 0; i < m_rows.size(); ++i) {
        const ModuleSymbolRow& r = m_rows[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.name.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(symbolStateName(r.state));
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", r.detail.c_str());
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Retry")) retry = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy module info")) copy = r;
        ImGui::PopID();
      }
      ImGui::EndTable();
      if (retry && m_host.retry) m_host.retry();
      if (copy) copyModuleInfo(context, *copy);
    }
  }

  if (context.settings && !context.settings->symbolSearchPaths.empty()) {
    ImGui::SeparatorText("Symbol search paths");
    for (const std::string& p : context.settings->symbolSearchPaths) ImGui::BulletText("%s", p.c_str());
  }
  ImGui::End();
}

}  // namespace tachyon::viewer
