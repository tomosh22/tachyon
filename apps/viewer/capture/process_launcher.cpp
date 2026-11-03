#include "viewer/capture/process_launcher.h"

#include "core/platform.h"
#include "viewer/paths.h"

#include <mutex>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <shellapi.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char** environ;
#endif

namespace tachyon::viewer {

namespace {

#if TACHYON_PLATFORM_WINDOWS

std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

// CommandLineToArgvW quoting rules.
std::wstring quoteArgument(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (size_t i = 0; i < arg.size(); ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++backslashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(backslashes, L'\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

class SystemLauncher final : public IProcessLauncher {
 public:
  Result<LaunchedProcess> launch(const LaunchSpec& spec) override {
    std::wstring params;
    for (const std::string& a : spec.args) {
      if (!params.empty()) params.push_back(L' ');
      params += quoteArgument(widen(a));
    }
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = spec.elevate ? L"runas" : L"open";
    const std::wstring file = spec.exe.wstring();
    info.lpFile = file.c_str();
    info.lpParameters = params.empty() ? nullptr : params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
      const DWORD err = GetLastError();
      if (err == ERROR_CANCELLED) return makeError(ErrorCode::Cancelled, "elevation was declined");
      return makeError(ErrorCode::IoError, "cannot start " + spec.description + " (error " +
                                               std::to_string(static_cast<unsigned long>(err)) + ")");
    }
    LaunchedProcess out;
    if (info.hProcess) {
      out.pid = GetProcessId(info.hProcess);
      CloseHandle(info.hProcess);
    }
    return out;
  }
};

#else

class SystemLauncher final : public IProcessLauncher {
 public:
  ~SystemLauncher() override { reap(); }

  Result<LaunchedProcess> launch(const LaunchSpec& spec) override {
    reap();
    std::vector<std::string> argv;
    if (spec.elevate) argv.push_back("pkexec");
    argv.push_back(pathToUtf8(spec.exe));
    for (const std::string& a : spec.args) argv.push_back(a);
    std::vector<char*> raw;
    raw.reserve(argv.size() + 1);
    for (std::string& a : argv) raw.push_back(a.data());
    raw.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, raw[0], &actions, nullptr, raw.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
      return makeError(rc == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError,
                       "cannot start " + (spec.description.empty() ? argv[0] : spec.description) + ": " +
                           std::strerror(rc));
    }
    {
      std::lock_guard lock(m_mutex);
      m_children.push_back(pid);
    }
    LaunchedProcess out;
    out.pid = static_cast<uint32_t>(pid);
    return out;
  }

 private:
  void reap() {
    std::lock_guard lock(m_mutex);
    for (auto it = m_children.begin(); it != m_children.end();) {
      int status = 0;
      const pid_t r = waitpid(*it, &status, WNOHANG);
      if (r == *it || (r < 0 && errno != EINTR)) {
        it = m_children.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::mutex m_mutex;
  std::vector<pid_t> m_children;
};

#endif

}  // namespace

std::unique_ptr<IProcessLauncher> makeSystemProcessLauncher() {
  return std::make_unique<SystemLauncher>();
}

}  // namespace tachyon::viewer
