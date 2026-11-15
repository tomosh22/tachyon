#include "viewer/dock_layout.h"

#include <imgui_internal.h>

#include <algorithm>

namespace tachyon::viewer {

namespace {

constexpr float kMinLayoutExtent = 16.0f;
// Fractions of the dockspace (Superluminal proportions): stats strip ~20% of the height, timeline ~45%, the
// bottom row the remaining ~35%; the analysis tab group takes ~65% of the bottom row's width.
constexpr float kStatsFraction = 0.20f;
constexpr float kTimelineFractionOfRest = 0.45f / (1.0f - kStatsFraction);
constexpr float kAnalysisFraction = 0.65f;

// The window that stands for a slot once the layout exists (its dock node is the slot's node).
const char* anchorName(DockSlot slot) {
  switch (slot) {
    case DockSlot::Stats: return "Function Statistics";
    case DockSlot::Timeline: return "Threads Timeline";
    case DockSlot::Analysis: return "Call Graph";
    case DockSlot::Source: return "Source";
    case DockSlot::None: break;
  }
  return nullptr;
}

// Position of a panel in its tab group (the order of this list).
short tabOrderOf(const std::string& name) {
  static constexpr const char* kOrder[] = {"Function Statistics",
                                           "Trace Info",
                                           "Call Graph",
                                           "Function List",
                                           "Butterfly",
                                           "Thread Interaction",
                                           "Capture",
                                           "Session Explorer",
                                           "Live",
                                           "Flight Recorder",
                                           "Symbols"};
  for (size_t i = 0; i < std::size(kOrder); ++i) {
    if (name == kOrder[i]) return static_cast<short>(i);
  }
  return static_cast<short>(std::size(kOrder));
}

}  // namespace

DockSlot dockSlotFor(const std::string& name) {
  if (name == "Threads Timeline") return DockSlot::Timeline;
  if (name == "Function Statistics" || name == "Trace Info") return DockSlot::Stats;
  if (name == "Source") return DockSlot::Source;
  // The capture family (used mostly while no trace is open) shares the roomy bottom-left group.
  for (const char* n : {"Call Graph", "Function List", "Butterfly", "Thread Interaction", "Capture",
                        "Flight Recorder", "Live", "Session Explorer", "Symbols"}) {
    if (name == n) return DockSlot::Analysis;
  }
  return DockSlot::None;
}

ImGuiID mainDockspaceId(const ImGuiViewport* viewport) {
  // Mirrors DockSpaceOverViewport(0, ...): GetID("DockSpace") inside the "WindowOverViewport_<id>" window.
  char label[32];
  ImFormatString(label, IM_ARRAYSIZE(label), "WindowOverViewport_%08X", viewport->ID);
  return ImHashStr("DockSpace", 0, ImHashStr(label));
}

bool hasSavedLayout(ImGuiID dockspaceId, const std::vector<std::string>& panelNames) {
  if (ImGui::DockBuilderGetNode(dockspaceId) != nullptr) return true;
  for (const std::string& name : panelNames) {
    if (ImGui::FindWindowSettingsByID(ImHashStr(name.c_str())) != nullptr) return true;
  }
  return false;
}

bool buildDefaultDockLayout(ImGuiID dockspaceId, ImVec2 size, const std::vector<std::string>& panelNames,
                            const std::vector<std::string>* openNames) {
  if (!(size.x >= kMinLayoutExtent) || !(size.y >= kMinLayoutExtent)) return false;

  ImGui::DockBuilderRemoveNode(dockspaceId);  // also undocks every window that sat in it
  ImGui::DockBuilderAddNode(dockspaceId, static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_DockSpace) |
                                             ImGuiDockNodeFlags_PassthruCentralNode);
  ImGui::DockBuilderSetNodeSize(dockspaceId, size);

  ImGuiID stats = 0, rest = 0;
  ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Up, kStatsFraction, &stats, &rest);
  ImGuiID timeline = 0, bottom = 0;
  ImGui::DockBuilderSplitNode(rest, ImGuiDir_Up, kTimelineFractionOfRest, &timeline, &bottom);
  ImGuiID analysis = 0, source = 0;
  ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Left, kAnalysisFraction, &analysis, &source);

  auto nodeOf = [&](DockSlot slot) -> ImGuiID {
    switch (slot) {
      case DockSlot::Stats: return stats;
      case DockSlot::Timeline: return timeline;
      case DockSlot::Analysis: return analysis;
      case DockSlot::Source: return source;
      case DockSlot::None: break;
    }
    return 0;
  };
  for (const std::string& name : panelNames) {
    const ImGuiID node = nodeOf(dockSlotFor(name));
    if (node == 0) continue;
    ImGui::DockBuilderDockWindow(name.c_str(), node);
    // Tab order inside a group: the main panel first (registration order would put Thread Interaction there).
    const short order = tabOrderOf(name);
    if (ImGuiWindow* window = ImGui::FindWindowByName(name.c_str())) {
      window->DockOrder = order;
    } else if (ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(name.c_str()))) {
      settings->DockOrder = order;
    }
  }
  // Tab groups open on the main panel of their node.
  for (const DockSlot slot : {DockSlot::Stats, DockSlot::Analysis}) {
    const std::string anchor = anchorName(slot);
    if (openNames != nullptr && std::find(openNames->begin(), openNames->end(), anchor) == openNames->end())
      continue;  // the main panel is closed: whatever opens first is shown
    if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(nodeOf(slot)))
      node->SelectedTabId = ImHashStr("#TAB", 0, ImHashStr(anchorName(slot)));  // = ImGuiWindow::TabId
  }
  ImGui::DockBuilderFinish(dockspaceId);
  return true;
}

ImGuiID dockTargetFor(const std::string& panelName) {
  const char* anchor = anchorName(dockSlotFor(panelName));
  if (anchor == nullptr || panelName == anchor) return 0;
  const ImGuiWindow* window = ImGui::FindWindowByName(anchor);
  if (window == nullptr || window->DockId == 0) return 0;
  const ImGuiDockNode* node = ImGui::DockBuilderGetNode(window->DockId);
  return (node != nullptr && node->IsLeafNode()) ? node->ID : 0;
}

}  // namespace tachyon::viewer
