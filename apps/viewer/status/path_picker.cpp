#include "viewer/status/path_picker.h"

#include "viewer/paths.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace tachyon::viewer {

namespace {
constexpr const char* kTitle = "Add symbol path";
}

void PathPicker::open(const std::filesystem::path& startDir) {
  std::error_code ec;
  std::filesystem::path dir = startDir;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) dir = std::filesystem::current_path(ec);
  navigate(dir);
  m_requestOpen = true;
  m_open = true;
}

void PathPicker::navigate(const std::filesystem::path& dir) {
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(dir, ec);
  m_directory = ec ? dir : canonical;
  m_pathInput = {};
  const std::string utf8 = pathToUtf8(m_directory);
  std::memcpy(m_pathInput.data(), utf8.data(), std::min(utf8.size(), m_pathInput.size() - 1));
  m_listing = listDirectory(m_directory, std::string());
  m_selected = -1;
}

std::optional<std::filesystem::path> PathPicker::draw() {
  if (!m_open) return std::nullopt;
  if (m_requestOpen) {
    ImGui::OpenPopup(kTitle);
    m_requestOpen = false;
  }
  std::optional<std::filesystem::path> chosen;
  ImGui::SetNextWindowSize(ImVec2(640.0f, 460.0f), ImGuiCond_Appearing);
  bool keepOpen = true;
  if (ImGui::BeginPopupModal(kTitle, &keepOpen, ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextUnformatted(
        "Choose a directory with symbol files (or a symbols file) for the unresolved modules.");
    if (ImGui::Button("Up") && m_directory.has_parent_path() && m_directory.parent_path() != m_directory)
      navigate(m_directory.parent_path());
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##path", m_pathInput.data(), m_pathInput.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      std::error_code ec;
      const std::filesystem::path typed = pathFromUtf8(m_pathInput.data());
      if (std::filesystem::is_directory(typed, ec)) {
        navigate(typed);
      } else if (std::filesystem::is_regular_file(typed, ec)) {
        chosen = typed;
      }
    }
    if (!m_listing.error.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_listing.error.c_str());
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.5f;
    if (ImGui::BeginChild("##entries", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
      for (size_t i = 0; i < m_listing.entries.size(); ++i) {
        const DirEntry& e = m_listing.entries[i];
        const std::string label = (e.isDirectory ? "[dir]  " : "       ") + e.name;
        if (ImGui::Selectable(label.c_str(), m_selected == static_cast<int>(i),
                              ImGuiSelectableFlags_AllowDoubleClick)) {
          m_selected = static_cast<int>(i);
          if (ImGui::IsMouseDoubleClicked(0)) {
            if (e.isDirectory) {
              navigate(m_directory / pathFromUtf8(e.name));
              break;  // listing replaced
            }
            chosen = m_directory / pathFromUtf8(e.name);
          }
        }
      }
    }
    ImGui::EndChild();
    if (ImGui::Button("Use this directory")) chosen = m_directory;
    ImGui::SameLine();
    const bool fileSelected = m_selected >= 0 && m_selected < static_cast<int>(m_listing.entries.size()) &&
                              !m_listing.entries[static_cast<size_t>(m_selected)].isDirectory;
    ImGui::BeginDisabled(!fileSelected);
    if (ImGui::Button("Use selected file"))
      chosen = m_directory / pathFromUtf8(m_listing.entries[static_cast<size_t>(m_selected)].name);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) keepOpen = false;
    if (chosen || !keepOpen) {
      ImGui::CloseCurrentPopup();
      m_open = false;
    }
    ImGui::EndPopup();
  } else if (!m_requestOpen) {
    m_open = false;
  }
  return chosen;
}

}  // namespace tachyon::viewer
