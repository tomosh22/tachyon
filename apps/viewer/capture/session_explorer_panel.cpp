#include "viewer/capture/session_explorer_panel.h"

#include "viewer/capture/capture_paths.h"
#include "viewer/capture/capture_ui.h"
#include "viewer/paths.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace tachyon::viewer {

namespace fs = std::filesystem;
using capture_ui::inputString;

namespace {

constexpr ImVec4 kRed(1.0f, 0.4f, 0.4f, 1.0f);
constexpr ImVec4 kAmber(1.0f, 0.75f, 0.2f, 1.0f);

bool samePath(const fs::path& a, const fs::path& b) {
  return a.lexically_normal() == b.lexically_normal();
}

}  // namespace

std::string formatFileSize(uint64_t bytes) {
  char buffer[32];
  if (bytes < 1024) {
    std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
  } else if (bytes < 1024ull * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.1f KiB", static_cast<double>(bytes) / 1024.0);
  } else if (bytes < 1024ull * 1024 * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f GiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  }
  return buffer;
}

std::string formatSessionTime(int64_t epochSeconds) {
  if (epochSeconds <= 0) return {};
  const std::time_t t = static_cast<std::time_t>(epochSeconds);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);
  return buffer;
}

SessionExplorerPanel::SessionExplorerPanel(SessionExplorerHost host) : m_host(std::move(host)) {
  m_catalog.setOnUpdate(m_host.wake);
}

SessionExplorerPanel::~SessionExplorerPanel() = default;

fs::path SessionExplorerPanel::directory() const {
  return capturesDir(m_host.settings ? m_host.settings->capture : CaptureSettings());
}

void SessionExplorerPanel::refresh() {
  std::vector<fs::path> recent;
  if (m_host.settings) {
    for (const std::string& r : m_host.settings->recentFiles) recent.push_back(pathFromUtf8(r));
  }
  m_catalog.refresh(std::move(recent), directory());
  m_haveRefreshed = true;
}

bool SessionExplorerPanel::poll() {
  if (!m_catalog.poll(m_rows)) return false;
  m_visibleDirty = true;
  return true;
}

void SessionExplorerPanel::onVisibilityChanged(bool visible) {
  if (visible) m_lastKey.clear();  // rescan when the panel is shown again
}

void SessionExplorerPanel::setSort(SessionSort key, bool ascending) {
  m_sort = key;
  m_ascending = ascending;
  m_visibleDirty = true;
}

void SessionExplorerPanel::setFilter(std::string text) {
  m_filter = std::move(text);
  m_visibleDirty = true;
}

void SessionExplorerPanel::rebuildVisible() {
  m_visible = sortSessions(filterSessions(m_rows, m_filter), m_sort, m_ascending);
  m_visibleDirty = false;
}

const std::vector<SessionInfo>& SessionExplorerPanel::visibleRows() {
  if (m_visibleDirty) rebuildVisible();
  return m_visible;
}

void SessionExplorerPanel::open(const fs::path& path) {
  if (m_host.openFile) m_host.openFile(path);
}

void SessionExplorerPanel::reveal(const fs::path& path) {
  if (!m_host.launcher) return;
  const Result<LaunchedProcess> r = m_host.launcher->launch(revealSpec(path));
  if (!r.ok() && m_host.toast)
    m_host.toast(ToastLevel::Error, "Cannot reveal the file: " + r.error().message);
}

void SessionExplorerPanel::forget(const fs::path& path) {
  if (!m_host.settings) return;
  std::vector<std::string>& recent = m_host.settings->recentFiles;
  recent.erase(std::remove_if(recent.begin(), recent.end(),
                              [&](const std::string& r) { return samePath(pathFromUtf8(r), path); }),
               recent.end());
  if (m_host.saveSettings) m_host.saveSettings();
  m_lastKey.clear();
}

void SessionExplorerPanel::requestDelete(const fs::path& path) {
  m_deletePath = path;
  m_deleteFiles = sessionFilesFor(path);
  m_openDeleteModal = true;
}

void SessionExplorerPanel::cancelDelete() {
  m_deletePath.clear();
  m_deleteFiles.clear();
  m_openDeleteModal = false;
}

bool SessionExplorerPanel::confirmDelete() {
  if (m_deletePath.empty()) return false;
  const fs::path trace = m_deletePath;
  cancelDelete();
  std::vector<fs::path> removed;
  const Result<void> r = deleteSession(trace, &removed);
  if (!r.ok()) {
    if (m_host.toast) m_host.toast(ToastLevel::Error, "Delete failed: " + r.error().message);
  } else if (m_host.toast) {
    m_host.toast(ToastLevel::Info,
                 "Deleted " + pathToUtf8(trace.filename()) +
                     (removed.size() > 1 ? " and " + std::to_string(removed.size() - 1) + " related item(s)"
                                         : std::string()));
  }
  forget(trace);
  refresh();
  return r.ok();
}

void SessionExplorerPanel::drawDeleteModal() {
  if (m_openDeleteModal) {
    ImGui::OpenPopup("Delete capture##explorer");
    m_openDeleteModal = false;
  }
  bool keepOpen = true;
  if (!ImGui::BeginPopupModal("Delete capture##explorer", &keepOpen,
                              ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
    return;
  ImGui::TextUnformatted("These files will be deleted permanently:");
  for (const fs::path& f : m_deleteFiles) ImGui::BulletText("%s", pathToUtf8(f).c_str());
  if (m_deleteFiles.empty()) ImGui::TextDisabled("(nothing on disk)");
  if (ImGui::Button("Delete")) {
    ImGui::CloseCurrentPopup();
    confirmDelete();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel") || !keepOpen) {
    ImGui::CloseCurrentPopup();
    cancelDelete();
  }
  ImGui::EndPopup();
}

void SessionExplorerPanel::drawToolbar() {
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
  if (inputString("##filter", m_filter, "Filter (name, target, mode)")) m_visibleDirty = true;
  ImGui::SameLine();
  if (ImGui::Button("Refresh")) refresh();
  ImGui::SameLine();
  ImGui::TextDisabled("Sort:");
  ImGui::SameLine();
  const char* names[] = {"Time", "Name", "Size"};
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
  int sort = static_cast<int>(m_sort);
  if (ImGui::Combo("##sort", &sort, names, 3)) setSort(static_cast<SessionSort>(sort), m_ascending);
  ImGui::SameLine();
  bool ascending = m_ascending;
  if (ImGui::Checkbox("Ascending", &ascending)) setSort(m_sort, ascending);

  if (!m_dirInputInit) {
    m_dirInput = pathToUtf8(directory());
    m_dirInputInit = true;
  }
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 28.0f);
  const bool entered =
      inputString("Captures directory", m_dirInput, nullptr, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if (ImGui::Button("Browse..."))
    m_picker.open(directory(), "Captures directory", "Choose the directory to list captures from.");
  if (entered && m_host.settings) {
    m_host.settings->capture.capturesDir = m_dirInput;
    if (m_host.saveSettings) m_host.saveSettings();
  }
  if (auto chosen = m_picker.draw(); chosen && m_host.settings) {
    std::error_code ec;
    m_host.settings->capture.capturesDir =
        pathToUtf8(fs::is_directory(*chosen, ec) ? *chosen : chosen->parent_path());
    m_dirInput = m_host.settings->capture.capturesDir;
    if (m_host.saveSettings) m_host.saveSettings();
  }
}

void SessionExplorerPanel::drawTable() {
  const std::vector<SessionInfo>& rows = visibleRows();
  constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                    ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp |
                                    ImGuiTableFlags_Sortable | ImGuiTableFlags_Borders;
  if (!ImGui::BeginTable("##sessions", 10, flags)) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.5f);
  ImGui::TableSetupColumn("Captured",
                          ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort |
                              ImGuiTableColumnFlags_PreferSortDescending,
                          1.4f);
  ImGui::TableSetupColumn(
      "Size", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 0.8f);
  ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 0.7f);
  ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.8f);
  ImGui::TableSetupColumn("Hz", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 0.5f);
  ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                          0.7f);
  ImGui::TableSetupColumn("Lost", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 0.5f);
  ImGui::TableSetupColumn("Agent", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort, 1.0f);
  ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoSort,
                          1.8f);
  ImGui::TableHeadersRow();
  if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
      specs && specs->SpecsDirty && specs->SpecsCount > 0) {
    const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
    const SessionSort key = s.ColumnIndex == 0   ? SessionSort::Name
                            : s.ColumnIndex == 2 ? SessionSort::Size
                                                 : SessionSort::Time;
    setSort(key, s.SortDirection == ImGuiSortDirection_Ascending);
    specs->SpecsDirty = false;
  }

  // Row actions only record what was clicked: they change the list the loop is walking.
  fs::path toOpen, toReveal, toDelete, toForget;
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
      const SessionInfo& r = rows[static_cast<size_t>(i)];
      ImGui::PushID(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const std::string name = pathToUtf8(r.path.filename());
      ImGui::TextUnformatted(name.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pathToUtf8(r.path).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(formatSessionTime(r.mtime).c_str());
      ImGui::TableNextColumn();
      if (r.state != SessionState::Missing) ImGui::TextUnformatted(formatFileSize(r.sizeBytes).c_str());
      if (r.state == SessionState::Missing) {
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextColored(kRed, "file is missing");
      } else if (r.state == SessionState::Unreadable) {
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextColored(kRed, "unreadable");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.error.c_str());
      } else if (r.state == SessionState::Pending) {
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("reading...");
      } else {
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.mode.c_str());
        ImGui::TableNextColumn();
        std::string target = r.exe;
        if (r.pid != 0)
          target +=
              (target.empty() ? "pid " : " (pid ") + std::to_string(r.pid) + (target.empty() ? "" : ")");
        ImGui::TextUnformatted(target.c_str());
        if (r.partial) {
          ImGui::SameLine();
          ImGui::TextColored(kAmber, "[partial]");
        }
      }
      ImGui::TableNextColumn();
      if (r.state == SessionState::Ok && r.samplingHz != 0) ImGui::Text("%u", r.samplingHz);
      ImGui::TableNextColumn();
      if (r.state == SessionState::Ok && r.durationS > 0.0) ImGui::Text("%.2f s", r.durationS);
      ImGui::TableNextColumn();
      if (r.state == SessionState::Ok) {
        if (r.eventsLost > 0) {
          ImGui::TextColored(kAmber, "%llu", static_cast<unsigned long long>(r.eventsLost));
        } else {
          ImGui::TextDisabled("0");
        }
      }
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(r.agentVersion.c_str());
      ImGui::TableNextColumn();
      const bool exists = r.state != SessionState::Missing;
      ImGui::BeginDisabled(!exists || r.state == SessionState::Unreadable ||
                           r.state == SessionState::Pending);
      if (ImGui::SmallButton("Open")) toOpen = r.path;
      ImGui::EndDisabled();
      ImGui::SameLine();
      ImGui::BeginDisabled(!exists);
      if (ImGui::SmallButton("Reveal")) toReveal = r.path;
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (exists) {
        if (ImGui::SmallButton("Delete")) toDelete = r.path;
      } else if (ImGui::SmallButton("Forget")) {
        toForget = r.path;
      }
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
  if (!toOpen.empty()) open(toOpen);
  if (!toReveal.empty()) reveal(toReveal);
  if (!toDelete.empty()) requestDelete(toDelete);
  if (!toForget.empty()) forget(toForget);
}

void SessionExplorerPanel::draw(const PanelContext&, bool* open) {
  if (!ImGui::Begin(name(), open)) {
    ImGui::End();
    return;
  }
  // Rescan when the recent files or the directory changed (or the panel was just shown).
  std::string key = pathToUtf8(directory());
  if (m_host.settings) {
    for (const std::string& r : m_host.settings->recentFiles) key += "\n" + r;
  }
  if (!m_haveRefreshed || key != m_lastKey) {
    m_lastKey = key;
    refresh();
  }
  poll();
  drawToolbar();
  if (m_catalog.busy()) {
    ImGui::SameLine();
    ImGui::TextDisabled("scanning...");
  }
  drawTable();
  if (m_rows.empty() && !m_catalog.busy())
    ImGui::TextDisabled("No captures yet. Use Capture > Run... or File > Open.");
  drawDeleteModal();
  ImGui::End();
}

std::unique_ptr<IPanel> makeSessionExplorerPanel(SessionExplorerHost host) {
  return std::make_unique<SessionExplorerPanel>(std::move(host));
}

}  // namespace tachyon::viewer
