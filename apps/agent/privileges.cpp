#include "agent/privileges.h"

#include "core/platform.h"

#include <cerrno>
#include <vector>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#endif

namespace tachyon::agent {

bool parseSocketOwnerUid(const std::string& text, uint32_t& uid) {
  if (text.empty() || text.size() > 10) return false;
  uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  if (value == 0 || value >= 0xFFFFFFFFULL) return false;
  uid = static_cast<uint32_t>(value);
  return true;
}

bool socketOwnerFromEnvironment(bool isRoot, const char* pkexecUid, const char* sudoUid, uint32_t& uid) {
  if (!isRoot) return false;
  for (const char* candidate : {pkexecUid, sudoUid}) {
    if (candidate != nullptr && parseSocketOwnerUid(candidate, uid)) return true;
  }
  return false;
}

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

control::SocketOwner socketOwnerForUid(uint32_t) {
  return {};
}

#else

bool hasCapturePrivileges() {
  if (geteuid() == 0) return true;
  constexpr int kCapSysAdmin = 21;  // what kernels before 5.8 (no CAP_PERFMON) ask for
  constexpr int kCapPerfmon = 38;
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("CapEff:", 0) == 0) {
      const uint64_t caps = std::strtoull(line.c_str() + 7, nullptr, 16);
      if (((caps >> kCapPerfmon) & 1U) != 0 || ((caps >> kCapSysAdmin) & 1U) != 0) return true;
      break;
    }
  }
  // kernel.perf_event_paranoid <= 0 lets any user sample system-wide (-1 also scheduler tracepoints).
  std::ifstream paranoid("/proc/sys/kernel/perf_event_paranoid");
  long value = 0;
  return static_cast<bool>(paranoid >> value) && value <= 0;
}

control::SocketOwner socketOwnerForUid(uint32_t uid) {
  control::SocketOwner owner;
  owner.uid = uid;
  std::vector<char> buffer(16384);
  passwd entry{};
  passwd* found = nullptr;
  if (getpwuid_r(uid, &entry, buffer.data(), buffer.size(), &found) == 0 && found != nullptr)
    owner.gid = found->pw_gid;
  return owner;
}

#endif

}  // namespace tachyon::agent
