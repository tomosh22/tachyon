#include "viewer/builtin_panels.h"

#include "viewer/document.h"
#include "viewer/paths.h"

#include <imgui.h>

namespace tachyon::viewer {

namespace {

const char* roleName(model::ThreadRole role) {
  switch (role) {
    case model::ThreadRole::Main: return "main";
    case model::ThreadRole::Render: return "render";
    case model::ThreadRole::Worker: return "worker";
    case model::ThreadRole::Other: return "other";
    case model::ThreadRole::Unknown: break;
  }
  return "";
}

class TraceInfoPanel final : public IPanel {
 public:
  const char* name() const override { return "Trace Info"; }

  void draw(const PanelContext& context, bool* open) override {
    if (!ImGui::Begin(name(), open)) {
      ImGui::End();
      return;
    }
    const Document* doc = context.document;
    if (!doc) {
      ImGui::TextDisabled("No trace open. Use File > Open, Ctrl+O, or drop a .tachyon file here.");
      ImGui::End();
      return;
    }
    const model::TraceModel& m = doc->model();
    ImGui::TextUnformatted(pathToUtf8(doc->path().filename()).c_str());
    if (m.isPartial) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Partial trace (not finalized)");
    const double hz = static_cast<double>(m.header.clock_frequency_hz);
    const double seconds = (hz > 0.0 && m.header.end_ts > m.header.start_ts)
                               ? static_cast<double>(m.header.end_ts - m.header.start_ts) / hz
                               : 0.0;
    ImGui::Text("Duration: %.3f s   Threads: %zu   Modules: %zu   Samples: %llu", seconds, m.threads.size(),
                m.modules.size(), static_cast<unsigned long long>(doc->totalSamples()));
    ImGui::Text("Callstack nodes: %zu   Resident: %.1f MiB", m.nodes.size(),
                static_cast<double>(m.residentBytes()) / (1024.0 * 1024.0));
    ImGui::Separator();

    if (ImGui::BeginTable("threads", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Name");
      ImGui::TableSetupColumn("TID");
      ImGui::TableSetupColumn("Role");
      ImGui::TableSetupColumn("Samples");
      ImGui::TableHeadersRow();
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(m.threads.size()));
      while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          const model::Thread& t = m.threads[static_cast<size_t>(i)];
          const ThreadIdx idx(static_cast<uint32_t>(i));
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          const bool selected = context.selection && context.selection->state().selectedThread == idx;
          ImGui::PushID(i);
          if (ImGui::Selectable(t.name.empty() ? "(unnamed)" : t.name.c_str(), selected,
                                ImGuiSelectableFlags_SpanAllColumns) &&
              context.selection) {
            context.selection->setSelectedThread(idx);
          }
          ImGui::PopID();
          ImGui::TableNextColumn();
          ImGui::Text("%u", t.tid);
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(roleName(t.role));
          ImGui::TableNextColumn();
          const size_t samples = static_cast<size_t>(i) < m.samplesByThread.size()
                                     ? m.samplesByThread[static_cast<size_t>(i)].ts.size()
                                     : 0;
          ImGui::Text("%zu", samples);
        }
      }
      ImGui::EndTable();
    }
    ImGui::End();
  }
};

}  // namespace

std::unique_ptr<IPanel> makeTraceInfoPanel() {
  return std::make_unique<TraceInfoPanel>();
}

}  // namespace tachyon::viewer
