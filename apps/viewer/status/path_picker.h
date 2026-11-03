// PathPicker (WP-48): the modal behind "Add symbol path...". The shell's FileBrowser only picks trace files,
// so this one picks a directory ("Use this directory") or any file (double-click or "Use selected file"). It
// lists directories with the same listDirectory() helper.
#pragma once

#include "viewer/file_browser.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace tachyon::viewer {

class PathPicker {
 public:
  // Main-thread only. Opens the modal at `startDir` (falls back to the current directory if unusable).
  // `title` (the modal's name; empty = "Add symbol path") and `hint` (the line above the list; empty = the
  // symbol files text) let other panels reuse the picker, e.g. "Locate source file" (WP-45).
  void open(const std::filesystem::path& startDir, std::string title = {}, std::string hint = {});
  // Main-thread only. Draws the modal if open; returns the chosen path exactly once.
  std::optional<std::filesystem::path> draw();
  // Main-thread only.
  bool isOpen() const { return m_open; }

 private:
  void navigate(const std::filesystem::path& dir);

  std::string m_title, m_hint;
  bool m_open = false;
  bool m_requestOpen = false;
  std::filesystem::path m_directory;
  std::array<char, 1024> m_pathInput{};
  DirListing m_listing;
  int m_selected = -1;
};

}  // namespace tachyon::viewer
