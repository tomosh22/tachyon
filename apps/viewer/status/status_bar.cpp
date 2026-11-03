#include "viewer/status/status_bar.h"

#include "viewer/common/query_filter.h"
#include "viewer/paths.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace tachyon::viewer {

namespace {

std::string groupDigits(uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
  return s;
}

}  // namespace

std::string formatDuration(uint64_t ticks, uint64_t clockHz) {
  if (clockHz == 0) return {};
  const double s = static_cast<double>(ticks) / static_cast<double>(clockHz);
  char buf[48];
  if (s < 1.0) {
    std::snprintf(buf, sizeof buf, "%.0f ms", s * 1000.0);
  } else if (s < 120.0) {
    std::snprintf(buf, sizeof buf, "%.2f s", s);
  } else {
    std::snprintf(buf, sizeof buf, "%d min %d s", static_cast<int>(s / 60.0), static_cast<int>(s) % 60);
  }
  return buf;
}

std::string describeDocument(const Document& document) {
  const model::TraceModel& m = document.model();
  std::string out = pathToUtf8(document.path().filename());
  const uint64_t span = m.header.end_ts >= m.header.start_ts ? m.header.end_ts - m.header.start_ts : 0;
  const std::string duration = formatDuration(span, m.header.clock_frequency_hz);
  if (!duration.empty()) out += "  |  " + duration;
  out += "  |  " + std::to_string(m.threads.size()) + " threads  |  " + groupDigits(document.totalSamples()) +
         " samples";
  return out;
}

std::string describeFilter(const SelectionState& state, const model::TraceModel& model) {
  std::vector<std::string> parts;
  if (state.timeFilter) {
    const ViewFilter& f = *state.timeFilter;
    const double hz =
        static_cast<double>(model.header.clock_frequency_hz ? model.header.clock_frequency_hz : 1);
    const double t0 =
        static_cast<double>(f.t0 >= model.header.start_ts ? f.t0 - model.header.start_ts : 0) / hz;
    const double t1 =
        static_cast<double>(f.t1 >= model.header.start_ts ? f.t1 - model.header.start_ts : 0) / hz;
    char buf[96];
    std::snprintf(buf, sizeof buf, "time %.3f-%.3f s", t0, t1);
    parts.emplace_back(buf);
    if (f.threads) parts.push_back(std::to_string(f.threads->size()) + " threads");
    if (f.subtreeRoot) parts.push_back("subtree #" + std::to_string(f.subtreeRoot->get()));
    if (f.subtreePath) parts.push_back("subtree " + common::subtreeText(f));
  }
  if (state.selectedThread.valid() && state.selectedThread.get() < model.threads.size()) {
    const model::Thread& t = model.threads[state.selectedThread.get()];
    parts.push_back("thread " + (t.name.empty() ? std::to_string(t.tid) : t.name));
  }
  if (parts.empty()) return "none";
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
  return out;
}

void StatusBar::update(const Document* document, uint64_t documentGeneration) {
  if (!document) {
    if (m_haveDocument) {
      m_haveDocument = false;
      m_referenced.clear();
      m_summary = {};
      m_symbolsText.clear();
      m_tooltipRows.clear();
    }
    m_generation = documentGeneration;
    return;
  }
  if (!m_haveDocument || m_generation != documentGeneration) {
    m_haveDocument = true;
    m_generation = documentGeneration;
    m_referenced = referencedModules(document->model());
  }
  if (document->symbols()) m_summary = summarizeSymbols(*document->symbols(), m_referenced);
}

void StatusBar::draw(const Document* document, const SelectionState& state, const std::string& error) {
  m_symbolsRect = {};
  m_symbolsClicked = false;
  if (!error.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", error.c_str());
    return;
  }
  if (!document) {
    ImGui::TextDisabled("Ready");
    return;
  }
  const model::TraceModel& model = document->model();
  const std::string text = describeDocument(*document);
  ImGui::TextUnformatted(text.c_str());
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  const std::string filter = describeFilter(state, model);
  if (filter == "none") {
    ImGui::TextDisabled("Filter: none");
  } else {
    ImGui::TextColored(ImVec4(0.45f, 0.7f, 1.0f, 1.0f), "Filter: %s", filter.c_str());
  }

  const symbols::SymbolService* service = document->symbols().get();
  if (!service) return;
  ImGui::SameLine();
  ImGui::TextDisabled("|");
  ImGui::SameLine();
  const SymbolSummary& s = m_summary;
  m_symbolsText = "Symbols: " + std::to_string(s.resolved) + "/" + std::to_string(s.total);
  if (s.failed > 0) m_symbolsText += " (" + std::to_string(s.failed) + " failed)";
  const ImVec4 color =
      s.failed > 0 ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImGui::GetStyle().Colors[ImGuiCol_Text];
  ImGui::TextColored(color, "%s", m_symbolsText.c_str());
  const ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
  m_symbolsRect = {rmin.x, rmin.y, rmax.x, rmax.y};
  bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    m_symbolsClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  }
  if (s.running()) {
    ImGui::SameLine();
    const float fraction = s.total ? static_cast<float>(s.done) / static_cast<float>(s.total) : 0.0f;
    ImGui::ProgressBar(fraction, ImVec2(ImGui::GetFontSize() * 6.0f, ImGui::GetFontSize() * 0.6f), "");
    hovered = hovered || ImGui::IsItemHovered();
  }
  if (hovered) {
    m_tooltipRows = moduleSymbolRows(model, *service, m_referenced, false);
    ++m_tooltipDraws;
    if (ImGui::BeginTooltip()) {
      ImGui::Text("Symbol resolution: %u of %u modules done, %u resolved, %u failed", s.done, s.total,
                  s.resolved, s.failed);
      constexpr ImGuiTableFlags kFlags =
          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit;
      if (ImGui::BeginTable("##symbolmodules", 3, kFlags)) {
        ImGui::TableSetupColumn("Module");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Source / reason");
        ImGui::TableHeadersRow();
        const size_t limit = 40;
        for (size_t i = 0; i < m_tooltipRows.size() && i < limit; ++i) {
          const ModuleSymbolRow& r = m_tooltipRows[i];
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(r.name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(symbolStateName(r.state));
          ImGui::TableNextColumn();
          // SymbolService does not expose which symbol file a module was resolved from: say "loaded".
          ImGui::TextUnformatted(
              r.detail.empty() && r.state == symbols::SymbolState::Resolved ? "loaded" : r.detail.c_str());
        }
        ImGui::EndTable();
      }
      if (m_tooltipRows.size() > 40)
        ImGui::TextDisabled("... and %zu more (see the Symbols panel)", m_tooltipRows.size() - 40);
      ImGui::EndTooltip();
    }
  }
}

}  // namespace tachyon::viewer
