#include "etlconvert/device_path.h"

#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <cctype>
#include <cwchar>

namespace tachyon::etlconvert {

namespace {
bool startsWithNoCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(text[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  }
  return true;
}

bool boundaryAfter(std::string_view text, size_t prefixLen) {
  return text.size() == prefixLen || text[prefixLen] == '\\' || text[prefixLen] == '/';
}
}  // namespace

void DevicePathMapper::addDevice(std::string devicePrefix, std::string dosPrefix) {
  m_devices.emplace_back(std::move(devicePrefix), std::move(dosPrefix));
}

void DevicePathMapper::setSystemRoot(std::string dosPath) {
  m_systemRoot = std::move(dosPath);
}

std::string DevicePathMapper::toDosPath(std::string_view path) const {
  constexpr std::string_view kNtPrefix = "\\??\\";
  if (startsWithNoCase(path, kNtPrefix)) return std::string(path.substr(kNtPrefix.size()));
  constexpr std::string_view kSystemRoot = "\\SystemRoot";
  if (!m_systemRoot.empty() && startsWithNoCase(path, kSystemRoot) && boundaryAfter(path, kSystemRoot.size()))
    return m_systemRoot + std::string(path.substr(kSystemRoot.size()));
  for (const auto& [device, dos] : m_devices) {
    if (startsWithNoCase(path, device) && boundaryAfter(path, device.size()))
      return dos + std::string(path.substr(device.size()));
  }
  return std::string(path);
}

#if TACHYON_PLATFORM_WINDOWS

namespace {
std::string wideToUtf8(const wchar_t* text, int length) {
  if (length <= 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, length, out.data(), n, nullptr, nullptr);
  return out;
}
}  // namespace

DevicePathMapper DevicePathMapper::fromSystem() {
  DevicePathMapper mapper;
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    const wchar_t drive[3] = {letter, L':', L'\0'};
    wchar_t target[1024];
    const DWORD n = QueryDosDeviceW(drive, target, static_cast<DWORD>(sizeof(target) / sizeof(target[0])));
    if (n == 0) continue;
    // The result is a REG_MULTI_SZ; the first string is the current mapping.
    mapper.addDevice(wideToUtf8(target, static_cast<int>(wcslen(target))), wideToUtf8(drive, 2));
  }
  wchar_t root[MAX_PATH];
  const DWORD rootLen = GetEnvironmentVariableW(L"SystemRoot", root, static_cast<DWORD>(MAX_PATH));
  if (rootLen > 0 && rootLen < MAX_PATH) mapper.setSystemRoot(wideToUtf8(root, static_cast<int>(rootLen)));
  return mapper;
}

#else

DevicePathMapper DevicePathMapper::fromSystem() {
  return DevicePathMapper();
}

#endif

}  // namespace tachyon::etlconvert
