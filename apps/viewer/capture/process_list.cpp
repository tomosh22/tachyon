#include "viewer/capture/process_list.h"

#include "core/platform.h"
#include "viewer/paths.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format off
#include <tlhelp32.h>
// clang-format on
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <unordered_map>
#endif

namespace tachyon::viewer {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

#if TACHYON_PLATFORM_WINDOWS

std::string narrow(const std::wstring& w) {
  if (w.empty()) return {};
  const int n =
      WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// Image path and owner of a process; empty strings when the process cannot be opened.
void queryProcess(DWORD pid, std::string& imagePath, std::string& user) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return;
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
    path.resize(size);
    imagePath = narrow(path);
  }
  HANDLE token = nullptr;
  if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<char> buffer(needed);
    if (needed > 0 && GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
      const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
      wchar_t name[256], domain[256];
      DWORD nameLen = 256, domainLen = 256;
      SID_NAME_USE use;
      if (LookupAccountSidW(nullptr, tokenUser->User.Sid, name, &nameLen, domain, &domainLen, &use))
        user = narrow(std::wstring(name, nameLen));
    }
    CloseHandle(token);
  }
  CloseHandle(process);
}

#endif

}  // namespace

Result<std::vector<ProcessInfo>> enumerateProcesses(const std::filesystem::path& procRoot) {
  std::vector<ProcessInfo> out;
#if TACHYON_PLATFORM_WINDOWS
  (void)procRoot;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return makeError(ErrorCode::IoError, "cannot snapshot the processes");
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == 0) continue;
      ProcessInfo p;
      p.pid = entry.th32ProcessID;
      p.name = narrow(entry.szExeFile);
      queryProcess(entry.th32ProcessID, p.commandLine, p.user);
      out.push_back(std::move(p));
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
#else
  std::error_code ec;
  std::filesystem::directory_iterator it(procRoot, ec);
  if (ec) return makeError(ErrorCode::IoError, "cannot read " + pathToUtf8(procRoot) + ": " + ec.message());
  std::unordered_map<uint32_t, std::string> userNames;
  for (const std::filesystem::directory_entry& e : it) {
    const std::string dirName = pathToUtf8(e.path().filename());
    if (dirName.empty() ||
        !std::all_of(dirName.begin(), dirName.end(), [](unsigned char c) { return std::isdigit(c); }))
      continue;
    ProcessInfo p;
    p.pid = static_cast<uint32_t>(std::strtoul(dirName.c_str(), nullptr, 10));
    {
      std::ifstream cmd(e.path() / "cmdline", std::ios::binary);
      std::stringstream buffer;
      buffer << cmd.rdbuf();
      std::string raw = buffer.str();
      if (raw.empty()) continue;  // kernel thread or a process that is gone
      for (char& c : raw)
        if (c == '\0') c = ' ';
      while (!raw.empty() && raw.back() == ' ') raw.pop_back();
      p.commandLine = std::move(raw);
    }
    {
      std::ifstream comm(e.path() / "comm");
      std::getline(comm, p.name);
    }
    if (p.name.empty()) p.name = p.commandLine.substr(0, p.commandLine.find(' '));
    {
      std::ifstream status(e.path() / "status");
      std::string line;
      while (std::getline(status, line)) {
        if (line.rfind("Uid:", 0) != 0) continue;
        const uint32_t uid = static_cast<uint32_t>(std::strtoul(line.c_str() + 4, nullptr, 10));
        auto found = userNames.find(uid);
        if (found == userNames.end()) {
          std::string name = std::to_string(uid);
          passwd pwd{};
          passwd* result = nullptr;
          char buf[1024];
          if (getpwuid_r(uid, &pwd, buf, sizeof(buf), &result) == 0 && result != nullptr)
            name = result->pw_name;
          found = userNames.emplace(uid, std::move(name)).first;
        }
        p.user = found->second;
        break;
      }
    }
    out.push_back(std::move(p));
  }
#endif
  std::sort(out.begin(), out.end(), [](const ProcessInfo& a, const ProcessInfo& b) { return a.pid < b.pid; });
  return out;
}

std::vector<size_t> filterProcesses(const std::vector<ProcessInfo>& list, std::string_view text) {
  std::vector<size_t> out;
  const std::string needle = lower(text);
  for (size_t i = 0; i < list.size(); ++i) {
    const ProcessInfo& p = list[i];
    if (needle.empty() || std::to_string(p.pid).find(needle) != std::string::npos ||
        lower(p.name).find(needle) != std::string::npos || lower(p.user).find(needle) != std::string::npos ||
        lower(p.commandLine).find(needle) != std::string::npos)
      out.push_back(i);
  }
  return out;
}

}  // namespace tachyon::viewer
