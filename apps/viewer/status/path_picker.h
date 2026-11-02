// PathPicker (WP-48): the modal behind "Add symbol path...". The shell's FileBrowser only picks trace files,
// so this one picks a directory ("Use this directory") or any file (double-click or "Use selected file"). It
// lists directories with the same listDirectory() helper.
#pragma once

#include "viewer/file_browser.h"

#include <array>
#include <filesystem>
#include <optional>

namespace tachyon::viewer {

class PathPicker {
 public:
  // Main-thread only. Opens the modal at `startDir` (falls back to the current directory if unusable).
  void open(const std::filesystem::path& startDir);
  // Main-thread only. Draws the modal if open; returns the chosen path exactly once.
  std::optional<std::filesystem::path> draw();
  // Main-thread only.
  bool isOpen() const { return m_open; }

 private:
  void navigate(const std::filesystem::path& dir);

  bool m_open = false;
  bool m_requestOpen = false;
  std::filesystem::path m_directory;
  std::array<char, 1024> m_pathInput{};
  DirListing m_listing;
  int m_selected = -1;
};

}  // namespace tachyon::viewer
