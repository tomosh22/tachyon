// Session Explorer panel (WP-41): recent traces and the captures directory with the metadata of each capture
// (SessionCatalog), and Open / Reveal / Delete per row, sort by time / name / size and a text filter.
#pragma once

#include "viewer/capture/process_launcher.h"
#include "viewer/capture/session_catalog.h"
#include "viewer/panel.h"
#include "viewer/settings.h"
#include "viewer/status/path_picker.h"
#include "viewer/status/toast.h"

#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct SessionExplorerHost {
  Settings* settings = nullptr;
  std::function<void()> saveSettings;
  std::function<void(const std::filesystem::path&)> openFile;
  std::function<void(ToastLevel, std::string)> toast;
  std::function<void()> wake;                  // thread-safe: the catalog worker published rows
  std::shared_ptr<IProcessLauncher> launcher;  // "Reveal"
};

inline constexpr const char* kSessionExplorerPanelName = "Session Explorer";

// Thread-safe: pure functions. "512 B", "1.5 KiB", "12.3 MiB", "1.20 GiB".
std::string formatFileSize(uint64_t bytes);
// "2026-11-03 14:05:09" in local time ("" for 0).
std::string formatSessionTime(int64_t epochSeconds);

class SessionExplorerPanel final : public IPanel {
 public:
  explicit SessionExplorerPanel(SessionExplorerHost host);
  // Main-thread only.
  ~SessionExplorerPanel() override;

  const char* name() const override { return kSessionExplorerPanelName; }
  void draw(const PanelContext& context, bool* open) override;
  void onVisibilityChanged(bool visible) override;

  // Main-thread only. Rescans the captures directory and re-reads what changed (asynchronously).
  void refresh();
  // Main-thread only. Takes the worker's latest rows.
  bool poll();
  // Main-thread only. Opens `path` in the viewer.
  void open(const std::filesystem::path& path);
  // Main-thread only. Starts the file manager on the trace's directory (through the injected launcher).
  void reveal(const std::filesystem::path& path);
  // Main-thread only. Arms the confirmation modal for `path` (nothing is deleted yet).
  void requestDelete(const std::filesystem::path& path);
  // Main-thread only. Deletes what requestDelete announced and forgets the recent entry. Returns false if
  // nothing was armed or the deletion failed (a toast says why).
  bool confirmDelete();
  void cancelDelete();
  // Main-thread only. Removes a stale entry from the recent files without touching the disk.
  void forget(const std::filesystem::path& path);

  // ---- state (tests); main-thread only ----
  SessionCatalog& catalog() { return m_catalog; }
  const std::vector<SessionInfo>& rows() const { return m_rows; }
  // The rows as drawn: filtered and sorted.
  const std::vector<SessionInfo>& visibleRows();
  void setSort(SessionSort key, bool ascending);
  void setFilter(std::string text);
  const std::vector<std::filesystem::path>& pendingDelete() const { return m_deleteFiles; }
  bool deleteArmed() const { return !m_deletePath.empty(); }

 private:
  void rebuildVisible();
  void drawToolbar();
  void drawTable();
  void drawDeleteModal();
  std::filesystem::path directory() const;

  SessionExplorerHost m_host;
  SessionCatalog m_catalog;
  std::vector<SessionInfo> m_rows;
  std::vector<SessionInfo> m_visible;
  bool m_visibleDirty = true;
  SessionSort m_sort = SessionSort::Time;
  bool m_ascending = false;
  std::string m_filter;
  std::string m_lastKey;  // recent files + directory the catalog was last asked about
  bool m_haveRefreshed = false;

  std::filesystem::path m_deletePath;
  std::vector<std::filesystem::path> m_deleteFiles;
  bool m_openDeleteModal = false;

  PathPicker m_picker;
  std::string m_dirInput;
  bool m_dirInputInit = false;
};

// Thread-safe: factory.
std::unique_ptr<IPanel> makeSessionExplorerPanel(SessionExplorerHost host);

}  // namespace tachyon::viewer
