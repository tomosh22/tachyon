#include "agent/privileges.h"

#include "core/platform.h"

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

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#endif

namespace tachyon::agent {

#if TACHYON_PLATFORM_WINDOWS

bool hasCapturePrivileges() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = 0;
  const BOOL got = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
  CloseHandle(token);
  return got != 0 && elevation.TokenIsElevated != 0;
}

#else

bool hasCapturePrivileges() {
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
}

#endif

}  // namespace tachyon::agent
