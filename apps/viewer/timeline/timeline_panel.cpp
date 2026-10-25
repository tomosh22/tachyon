#include "viewer/timeline/timeline_panel.h"

#include "viewer/builtin_panels.h"

#include <imgui.h>

namespace tachyon::viewer {

void ThreadsTimelinePanel::draw(const PanelContext& context, bool* open) {
  // No scrollbar on the host window: the rows have their own, the rest must stay put.
  ImGui::SetNextWindowSize(ImVec2(960.0f, 480.0f), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(name(), open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::End();
    return;
  }
  m_widget.draw(context);
  ImGui::End();
}

void ThreadsTimelinePanel::onSelectionChanged(const PanelContext& context, const SelectionState& state,
                                              SelectionField field) {
  m_widget.onSelectionChanged(context, state, field);
}

void ThreadsTimelinePanel::onDocumentChanged(const Document* document) {
  m_widget.onDocumentChanged(document);
}

std::unique_ptr<IPanel> makeThreadsTimelinePanel() {
  return std::make_unique<ThreadsTimelinePanel>();
}

}  // namespace tachyon::viewer
