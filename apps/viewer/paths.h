// UTF-8 <-> std::filesystem::path helpers (Windows paths are UTF-16 natively; settings and argv are UTF-8).
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace tachyon::viewer {

// Thread-safe: pure function.
std::filesystem::path pathFromUtf8(std::string_view utf8);
// Thread-safe: pure function.
std::string pathToUtf8(const std::filesystem::path& path);

using EnvLookup = std::function<std::string(const char* name)>;

// Thread-safe: reads the process environment. Returns an empty string if unset.
std::string getEnvironmentVariable(const char* name);

// Thread-safe: pure function. User config directory: %APPDATA%/Tachyon when `windowsStyle`, otherwise
// $XDG_CONFIG_HOME/tachyon falling back to ~/.config/tachyon. Empty path if no base directory is known.
std::filesystem::path configDirFromEnv(const EnvLookup& env, bool windowsStyle);

// Thread-safe: pure function. User cache directory for downloaded symbols: %LOCALAPPDATA%/Tachyon/symbols
// when `windowsStyle`, otherwise $XDG_CACHE_HOME/tachyon/symbols falling back to ~/.cache/tachyon/symbols.
// Empty path if no base directory is known.
std::filesystem::path symbolCacheDirFromEnv(const EnvLookup& env, bool windowsStyle);

// Thread-safe: reads the process environment. symbolCacheDirFromEnv for the real environment and platform.
std::filesystem::path defaultSymbolCacheDir();

// Thread-safe: reads the process environment. configDirFromEnv for the real environment and platform.
std::filesystem::path defaultConfigDir();

}  // namespace tachyon::viewer
