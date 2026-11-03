#include "viewer/capture/capture_paths.h"

#include "core/platform.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tachyon::viewer {

std::filesystem::path dataDirFromEnv(const EnvLookup& env, bool windowsStyle) {
  if (windowsStyle) {
    const std::string base = env("LOCALAPPDATA");
    return base.empty() ? std::filesystem::path() : pathFromUtf8(base) / "Tachyon";
  }
  const std::string xdg = env("XDG_DATA_HOME");
  if (!xdg.empty()) return pathFromUtf8(xdg) / "tachyon";
  const std::string home = env("HOME");
  return home.empty() ? std::filesystem::path() : pathFromUtf8(home) / ".local" / "share" / "tachyon";
}

std::filesystem::path defaultDataDir() {
  return dataDirFromEnv([](const char* name) { return getEnvironmentVariable(name); },
                        TACHYON_PLATFORM_WINDOWS != 0);
}

std::filesystem::path capturesDir(const CaptureSettings& settings) {
  if (!settings.capturesDir.empty()) return pathFromUtf8(settings.capturesDir);
  const std::filesystem::path data = defaultDataDir();
  return data.empty() ? std::filesystem::path("captures") : data / "captures";
}

std::filesystem::path defaultCaptureFile(const std::filesystem::path& dir, std::string_view stem,
                                         std::time_t when) {
  std::string clean;
  for (const char c : stem) {
    const unsigned char u = static_cast<unsigned char>(c);
    clean.push_back((std::isalnum(u) || c == '-' || c == '_' || c == '.' || u >= 0x80) ? c : '_');
  }
  while (!clean.empty() && clean.front() == '.') clean.erase(clean.begin());
  if (clean.empty()) clean = "capture";
  std::tm tm{};
#if TACHYON_PLATFORM_WINDOWS
  localtime_s(&tm, &when);
#else
  localtime_r(&when, &tm);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  return dir / pathFromUtf8(clean + "-" + stamp + ".tachyon");
}

std::vector<std::string> splitArguments(std::string_view text, std::string& error) {
  error.clear();
  std::vector<std::string> out;
  std::string current;
  bool inToken = false;
  char quote = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (quote != 0) {
      if (c == quote) {
        quote = 0;
      } else if (quote == '"' && c == '\\' && i + 1 < text.size() &&
                 (text[i + 1] == '"' || text[i + 1] == '\\')) {
        current.push_back(text[++i]);
      } else {
        current.push_back(c);
      }
      continue;
    }
    if (c == '"' || c == '\'') {
      quote = c;
      inToken = true;
    } else if (c == '\\' && i + 1 < text.size() &&
               (text[i + 1] == ' ' || text[i + 1] == '"' || text[i + 1] == '\'' || text[i + 1] == '\\')) {
      current.push_back(text[++i]);
      inToken = true;
    } else if (std::isspace(static_cast<unsigned char>(c))) {
      if (inToken) out.push_back(std::move(current));
      current.clear();
      inToken = false;
    } else {
      current.push_back(c);
      inToken = true;
    }
  }
  if (quote != 0) {
    error = std::string("unterminated ") + (quote == '"' ? "double" : "single") + " quote";
    return {};
  }
  if (inToken) out.push_back(std::move(current));
  return out;
}

std::filesystem::path currentExecutablePath() {
#if TACHYON_PLATFORM_WINDOWS
  std::wstring buffer(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (n == 0 || n >= buffer.size()) return {};
  buffer.resize(n);
  return std::filesystem::path(buffer);
#else
  std::error_code ec;
  std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
  return ec ? std::filesystem::path() : p;
#endif
}

bool currentProcessHasCapturePrivileges() {
#if TACHYON_PLATFORM_WINDOWS
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const BOOL got = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return got != 0 && elevation.TokenIsElevated != 0;
#else
  if (geteuid() == 0) return true;
  constexpr int kCapPerfmon = 38;
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("CapEff:", 0) == 0) {
      const uint64_t caps = std::strtoull(line.c_str() + 7, nullptr, 16);
      return ((caps >> kCapPerfmon) & 1U) != 0;
    }
  }
  return false;
#endif
}

}  // namespace tachyon::viewer
