// Command-line parsing for tachyon-viewer.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct CliArgs {
  std::optional<std::filesystem::path> openPath;      // positional trace to open at start
  std::optional<std::filesystem::path> configPath;    // --config <settings.json>
  std::optional<std::filesystem::path> selftestPath;  // --selftest <trace>
  bool help = false;
  std::string error;  // non-empty when the arguments are invalid
};

// Thread-safe: pure function. `args` excludes argv[0] and is UTF-8.
CliArgs parseCliArgs(const std::vector<std::string>& args);
// Thread-safe: pure function.
const char* cliUsage();

}  // namespace tachyon::viewer
