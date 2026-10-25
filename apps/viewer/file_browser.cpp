#include "viewer/file_browser.h"

#include "viewer/paths.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>

namespace tachyon::viewer {

namespace {

std::string lowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

}  // namespace

DirListing listDirectory(const std::filesystem::path& directory, const std::string& extension) {
  namespace fs = std::filesystem;
  DirListing out;
  out.directory = directory;
  std::error_code ec;
  fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    out.error = ec.message();
    return out;
  }
  const std::string wantExt = lowerAscii(extension);
  for (const fs::directory_iterator end; it != end; it.increment(ec)) {
    if (ec) {
      out.error = ec.message();
      break;
    }
    std::error_code entryEc;
    DirEntry e;
    e.name = pathToUtf8(it->path().filename());
    e.isDirectory = it->is_directory(entryEc);
    if (!e.isDirectory) {
      if (!wantExt.empty() && lowerAscii(pathToUtf8(it->path().extension())) != wantExt) continue;
      const auto size = it->file_size(entryEc);
      e.size = entryEc ? 0 : static_cast<uint64_t>(size);
    }
    out.entries.push_back(std::move(e));
  }
  std::sort(out.entries.begin(), out.entries.end(), [](const DirEntry& a, const DirEntry& b) {
    if (a.isDirectory != b.isDirectory) return a.isDirectory;
    return lowerAscii(a.name) < lowerAscii(b.name);
  });
  return out;
}

void FileBrowser::open(const std::filesystem::path& startDir) {
  std::error_code ec;
  std::filesystem::path dir = startDir;
  if (dir.empty() || !std::filesystem::is_directory(dir, ec)) dir = std::filesystem::current_path(ec);
  navigate(dir);
  m_requestOpen = true;
  m_open = true;
}

void FileBrowser::navigate(const std::filesystem::path& dir) {
  std::error_code ec;
  std::filesystem::path canonical = std::filesystem::weakly_canonical(dir, ec);
  m_directory = ec ? dir : canonical;
  m_pathInput = {};
  const std::string utf8 = pathToUtf8(m_directory);
  std::memcpy(m_pathInput.data(), utf8.data(), std::min(utf8.size(), m_pathInput.size() - 1));
  m_listing = listDirectory(m_directory, m_showAllFiles ? std::string() : std::string(".tachyon"));
  m_selected = -1;
}

std::optional<std::filesystem::path> FileBrowser::draw() {
  if (!m_open) return std::nullopt;
  static constexpr const char* kTitle = "Open trace";
  if (m_requestOpen) {
    ImGui::OpenPopup(kTitle);
    m_requestOpen = false;
  }
  std::optional<std::filesystem::path> chosen;
  ImGui::SetNextWindowSize(ImVec2(640.0f, 460.0f), ImGuiCond_Appearing);
  bool keepOpen = true;
  if (ImGui::BeginPopupModal(kTitle, &keepOpen, ImGuiWindowFlags_NoSavedSettings)) {
    if (ImGui::Button("Up")) {
      if (m_directory.has_parent_path() && m_directory.parent_path() != m_directory) {
        navigate(m_directory.parent_path());
      }
    }
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
    if (ImGui::Checkbox("Show all files", &m_showAllFiles)) navigate(m_directory);

    if (!m_listing.error.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_listing.error.c_str());

    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.5f;
    if (ImGui::BeginChild("##entries", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
      for (size_t i = 0; i < m_listing.entries.size(); ++i) {
        const DirEntry& e = m_listing.entries[i];
        const std::string label = (e.isDirectory ? "[dir]  " : "       ") + e.name;
        const bool selected = m_selected == static_cast<int>(i);
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
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

    const bool canOpen = m_selected >= 0 && m_selected < static_cast<int>(m_listing.entries.size()) &&
                         !m_listing.entries[static_cast<size_t>(m_selected)].isDirectory;
    ImGui::BeginDisabled(!canOpen);
    if (ImGui::Button("Open")) {
      chosen = m_directory / pathFromUtf8(m_listing.entries[static_cast<size_t>(m_selected)].name);
    }
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
