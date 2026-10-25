// Viewer settings: JSON in the user config dir (or --config <path>). Loading never throws or fails hard.
#pragma once

#include "core/result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::viewer {

enum class ThemeKind : uint8_t { Dark, Light };

struct SourcePathMapping {
  std::string from, to;
  bool operator==(const SourcePathMapping&) const = default;
};

struct Settings {
  static constexpr size_t kMaxRecentFiles = 12;
  static constexpr float kMinUiScale = 0.5f, kMaxUiScale = 4.0f;

  std::vector<std::string> symbolServers = {"https://msdl.microsoft.com/download/symbols"};
  std::string cacheDir;  // empty = symbol cache lives under the config dir (WP-20 decides the exact layout)
  std::vector<SourcePathMapping> sourcePathMappings;
  ThemeKind theme = ThemeKind::Dark;
  float uiScale = 1.0f;                  // multiplied with the display DPI scale
  std::vector<std::string> recentFiles;  // UTF-8, most recent first

  // Thread-safe: pure function on the value.
  bool operator==(const Settings&) const = default;

  // Thread-safe (no shared state). Never fails: a missing file yields defaults silently; a corrupt file or an
  // invalid field yields defaults (per field where possible) and a logged warning.
  static Settings load(const std::filesystem::path& path);
  // Thread-safe (no shared state). Writes via a temp file and rename; creates parent directories.
  Result<void> save(const std::filesystem::path& path) const;

  // Not thread-safe. Moves/adds `path` at the front of recentFiles, de-duplicated, capped.
  void addRecentFile(const std::string& path);
};

// Thread-safe: pure functions.
const char* themeName(ThemeKind theme);

}  // namespace tachyon::viewer
