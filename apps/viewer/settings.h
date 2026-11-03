// Viewer settings: JSON in the user config dir (or --config <path>). Loading never throws or fails hard.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::viewer {

enum class ThemeKind : uint8_t { Dark, Light };

struct SourcePathMapping {
  std::string from, to;
  bool operator==(const SourcePathMapping&) const = default;
};

// WP-41: agent connection, capture dialogs and the Session Explorer. Strings are UTF-8.
struct CaptureSettings {
  // Where the Session Explorer scans and new captures go; empty = <user data dir>/captures.
  std::string capturesDir;
  // Agent address (socket path / pipe name); empty = control::defaultAgentAddress().
  std::string agentAddress;
  // Agent executable; empty = tachyon-agent next to the viewer binary.
  std::string agentExe;
  // Backend of an agent started by the viewer: "mock", "perf" or "etw"; empty = etw on Windows, perf on
  // Linux.
  std::string agentBackend;
  // Extra arguments for an agent started by the viewer (e.g. --mock-fixture FILE).
  std::vector<std::string> agentArgs;
  bool autoConnect = true;  // try to reach a running agent at startup
  bool autoOpen = true;     // open the trace when a capture finishes
  // Last used values of the Run / Attach dialogs.
  std::string exe, args, cwd;
  std::vector<std::string> env;  // "KEY=VALUE"
  bool startImmediately = true;
  uint32_t samplingHz = 8000;
  uint32_t circularMb = 0;
  bool followChildren = false;
  std::string childFilter;
  bool kernelStacks = true;

  // Thread-safe: pure function on the value.
  bool operator==(const CaptureSettings&) const = default;
};

struct Settings {
  static constexpr size_t kMaxRecentFiles = 12;
  static constexpr float kMinUiScale = 0.5f, kMaxUiScale = 4.0f;

  // Symbol servers (WP-21 locator); none unless configured, so a default install never touches the network.
  std::vector<std::string> symbolServers;
  // WP-48: explicit symbol search paths added from the Symbols panel (UTF-8). A directory is searched by the
  // locator and scanned for `*.symbols.json`; a `.json` file is registered as a symbol file override; any
  // other file is offered to the locator (used when its name matches the wanted symbol file).
  std::vector<std::string> symbolSearchPaths;
  std::string cacheDir;  // empty = <user cache dir>/tachyon/symbols (paths.h: defaultSymbolCacheDir)
  // WP-34/45: how debug-info source paths map to files on this machine. `sourcePathMappings` are prefix
  // rules (SourceLocator remaps, tried in order); `sourceRoots` are directories searched by the trailing
  // components of the debug path. "Locate file..." in the Source panel appends to them.
  std::vector<SourcePathMapping> sourcePathMappings;
  std::vector<std::string> sourceRoots;
  ThemeKind theme = ThemeKind::Dark;
  float uiScale = 1.0f;                  // multiplied with the display DPI scale
  std::vector<std::string> recentFiles;  // UTF-8, most recent first
  // WP-54: "Blend instrumentation scopes into sampled stacks" (used when the trace has instrumentation) and
  // the frame time budget of the timeline's frame graph (milliseconds; per-series overrides live in the
  // widget).
  bool blendScopes = true;
  double frameBudgetMs = 16.67;
  // WP-64: memory ceiling of the open trace's data (model + timeline index + analysis caches) as a percentage
  // of the physical RAM; 0 = no ceiling. Above it the viewer degrades gracefully (see memory_governor.h).
  static constexpr uint32_t kDefaultMemoryCeilingPercent = 75;
  static constexpr uint32_t kMinMemoryCeilingPercent = 10;
  uint32_t memoryCeilingPercent = kDefaultMemoryCeilingPercent;
  // WP-41
  CaptureSettings capture;

  // Thread-safe: pure function on the value.
  bool operator==(const Settings&) const = default;

  // Thread-safe (no shared state). Never fails: a missing file yields defaults silently; a corrupt file or an
  // invalid field yields defaults (per field where possible) and a logged warning.
  static Settings load(const std::filesystem::path& path);
  // Thread-safe (no shared state). Writes via a temp file and rename; creates parent directories.
  Result<void> save(const std::filesystem::path& path) const;

  // Not thread-safe. Moves/adds `path` at the front of recentFiles, de-duplicated, capped.
  void addRecentFile(const std::string& path);
  // Not thread-safe. Appends `path` to symbolSearchPaths unless present or empty; returns whether it was
  // added.
  bool addSymbolSearchPath(const std::string& path);
  // Not thread-safe (WP-45). Appends the rule unless an equal one exists or `from`/`to` is empty; returns
  // whether it was added.
  bool addSourcePathMapping(const SourcePathMapping& mapping);
  // Not thread-safe (WP-45). Appends `root` to sourceRoots unless present or empty; returns whether it was
  // added.
  bool addSourceRoot(const std::string& root);
};

// Thread-safe: pure functions.
const char* themeName(ThemeKind theme);

}  // namespace tachyon::viewer
