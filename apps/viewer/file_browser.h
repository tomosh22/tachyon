// Minimal ImGui file browser (SDL2 has no native file dialog).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct DirEntry {
  std::string name;  // UTF-8
  bool isDirectory = false;
  uint64_t size = 0;
};

struct DirListing {
  std::filesystem::path directory;
  std::vector<DirEntry> entries;  // directories first, then files, each group sorted case-insensitively
  std::string error;              // empty on success
};

// Thread-safe (reads the filesystem). Lists `directory`. With `extension` non-empty (e.g. ".tachyon",
// compared case-insensitively) only directories and files with that extension are returned. Never throws;
// failures land in DirListing::error.
DirListing listDirectory(const std::filesystem::path& directory, const std::string& extension);

class FileBrowser {
 public:
  // Main-thread only. Opens the modal at `startDir` (falls back to the current directory if unusable).
  void open(const std::filesystem::path& startDir);
  // Main-thread only. Draws the modal if open; returns the chosen file exactly once.
  std::optional<std::filesystem::path> draw();
  // Main-thread only.
  bool isOpen() const { return m_open; }

 private:
  void navigate(const std::filesystem::path& dir);

  bool m_open = false;
  bool m_requestOpen = false;
  bool m_showAllFiles = false;
  std::filesystem::path m_directory;
  std::array<char, 1024> m_pathInput{};
  DirListing m_listing;
  int m_selected = -1;
};

}  // namespace tachyon::viewer
