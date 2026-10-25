#include "viewer/paths.h"

#include <cstdlib>

namespace tachyon::viewer {

std::filesystem::path pathFromUtf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string pathToUtf8(const std::filesystem::path& path) {
  const std::u8string s = path.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string getEnvironmentVariable(const char* name) {
#ifdef _WIN32
  char* buffer = nullptr;
  size_t size = 0;
  if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) return {};
  std::string value(buffer);
  std::free(buffer);
  return value;
#else
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
#endif
}

std::filesystem::path configDirFromEnv(const EnvLookup& env, bool windowsStyle) {
  if (windowsStyle) {
    const std::string appdata = env("APPDATA");
    if (!appdata.empty()) return pathFromUtf8(appdata) / "Tachyon";
    return {};
  }
  const std::string xdg = env("XDG_CONFIG_HOME");
  if (!xdg.empty()) return pathFromUtf8(xdg) / "tachyon";
  const std::string home = env("HOME");
  if (!home.empty()) return pathFromUtf8(home) / ".config" / "tachyon";
  return {};
}

std::filesystem::path symbolCacheDirFromEnv(const EnvLookup& env, bool windowsStyle) {
  if (windowsStyle) {
    const std::string local = env("LOCALAPPDATA");
    if (!local.empty()) return pathFromUtf8(local) / "Tachyon" / "symbols";
    return {};
  }
  const std::string xdg = env("XDG_CACHE_HOME");
  if (!xdg.empty()) return pathFromUtf8(xdg) / "tachyon" / "symbols";
  const std::string home = env("HOME");
  if (!home.empty()) return pathFromUtf8(home) / ".cache" / "tachyon" / "symbols";
  return {};
}

std::filesystem::path defaultSymbolCacheDir() {
#ifdef _WIN32
  constexpr bool kWindows = true;
#else
  constexpr bool kWindows = false;
#endif
  return symbolCacheDirFromEnv(getEnvironmentVariable, kWindows);
}

std::filesystem::path defaultConfigDir() {
#ifdef _WIN32
  constexpr bool kWindows = true;
#else
  constexpr bool kWindows = false;
#endif
  return configDirFromEnv(getEnvironmentVariable, kWindows);
}

}  // namespace tachyon::viewer
