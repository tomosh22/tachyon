#include "viewer/status/shortcut_sheet.h"

#include <imgui.h>

#include <algorithm>
#include <utility>

namespace tachyon::viewer {

namespace {
constexpr const char* kTitle = "Keyboard shortcuts";

ShortcutGroup& groupFor(std::vector<ShortcutGroup>& groups, const std::string& name) {
  for (ShortcutGroup& g : groups) {
    if (g.name == name) return g;
  }
  groups.push_back({name, {}});
  return groups.back();
}
}  // namespace

void ShortcutRegistry::add(std::string group, std::string keys, std::string description) {
  for (ShortcutEntry& e : m_entries) {
    if (e.group == group && e.keys == keys) {
      e.description = std::move(description);
      return;
    }
  }
  m_entries.push_back({std::move(group), std::move(keys), std::move(description)});
}

void registerBuiltinPanelShortcuts(ShortcutRegistry& r) {
  // Timeline (timeline/timeline_widget.cpp, handleKeys; Shift speeds panning up 5x).
  r.add("Timeline", "A / Left, D / Right", "Pan left / right (Shift: faster)");
  r.add("Timeline", "W / +, S / -", "Zoom in / out around the cursor");
  r.add("Timeline", "Home", "Fit the whole trace");
  r.add("Timeline", "Z", "Zoom to the selection");
  r.add("Timeline", "F", "Use the selection as the time filter");
  r.add("Timeline", "Esc", "Clear the selection");
  r.add("Timeline", "Up / Down", "Select the previous / next thread");
  r.add("Timeline", "PageUp / PageDown", "Scroll the rows by a page");
  r.add("Timeline", "E", "Expand / collapse the selected thread");
  r.add("Timeline", "P", "Pin / unpin the selected thread");
  r.add("Timeline", "Mouse wheel", "Zoom (Shift+wheel: scroll horizontally)");
  // Function Statistics (stats/function_stats_panel.cpp).
  r.add("Function Statistics", "Up / Down", "Move the result cursor (search box)");
  r.add("Function Statistics", "PageUp / PageDown", "Move the result cursor by 10 (search box)");
  r.add("Function Statistics", "Enter", "Choose the highlighted result");
  r.add("Function Statistics", "F3 / Shift+F3", "Next / previous instance in the timeline");
  // Tables.
  r.add("Call Graph", "Double-click", "Show the function's source");
  r.add("Call Graph", "Right-click", "Context menu");
  r.add("Function List", "Double-click", "Show the function's source");
  r.add("Butterfly", "Click", "Select the caller / callee function");
}

std::vector<ShortcutGroup> buildShortcutSheet(const CommandRegistry& commands,
                                              const ShortcutRegistry& shortcuts) {
  std::vector<ShortcutGroup> groups;
  for (const Command& c : commands.commands()) {
    std::string group = "General";
    std::string title = c.title;
    if (const size_t cut = c.title.find(": "); cut != std::string::npos) {
      group = c.title.substr(0, cut);
      title = c.title.substr(cut + 2);
    }
    groupFor(groups, group).entries.push_back({group, c.shortcut, title});
  }
  for (const ShortcutEntry& e : shortcuts.entries()) groupFor(groups, e.group).entries.push_back(e);
  return groups;
}

void ShortcutSheet::draw(const CommandRegistry& commands, const ShortcutRegistry& shortcuts,
                         bool textInputActive) {
  // `?` is Shift+/ on most layouts; ImGui delivers the typed character, so check the text as well as the key.
  const ImGuiIO& io = ImGui::GetIO();
  bool pressed = ImGui::IsKeyPressed(ImGuiKey_Slash, false) && io.KeyShift && !io.KeyCtrl && !io.KeyAlt;
  if (!pressed) {
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
      if (io.InputQueueCharacters[i] == '?') pressed = true;
    }
  }
  if (pressed && !m_open && !textInputActive && !io.WantTextInput &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    open();
  }
  if (!m_open) return;
  if (m_requestOpen) {
    ImGui::OpenPopup(kTitle);
    m_requestOpen = false;
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, ImGui::GetFontSize() * 30.0f),
                           ImGuiCond_Appearing);
  bool keepOpen = true;
  if (ImGui::BeginPopupModal(kTitle, &keepOpen, ImGuiWindowFlags_NoSavedSettings)) {
    m_groups = buildShortcutSheet(commands, shortcuts);
    if (ImGui::BeginChild("##sheet", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()))) {
      for (const ShortcutGroup& g : m_groups) {
        ImGui::SeparatorText(g.name.c_str());
        if (ImGui::BeginTable(("##g" + g.name).c_str(), 2, ImGuiTableFlags_SizingFixedFit)) {
          ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 13.0f);
          ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
          for (const ShortcutEntry& e : g.entries) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (e.keys.empty()) {
              ImGui::TextDisabled("-");
            } else {
              ImGui::TextUnformatted(e.keys.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.description.c_str());
          }
          ImGui::EndTable();
        }
      }
    }
    ImGui::EndChild();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) keepOpen = false;
    if (!keepOpen) {
      ImGui::CloseCurrentPopup();
      m_open = false;
    }
    ImGui::EndPopup();
  } else if (!m_requestOpen) {
    m_open = false;
  }
}

}  // namespace tachyon::viewer
