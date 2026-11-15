#include "viewer/os_theme.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace tachyon::viewer {

namespace {

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

#ifndef _WIN32
// Runs `args` (args[0] is looked up in PATH) with stdin and stderr on /dev/null and returns its stdout, or
// nullopt when it cannot be started, exceeds `timeoutMs` (it is killed) or exits non-zero.
std::optional<std::string> runTool(const std::vector<std::string>& args, int timeoutMs) {
  int fds[2];
  if (pipe(fds) != 0) return std::nullopt;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);  // dup2 clears close-on-exec on the new descriptor
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  std::vector<char*> argv;
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (rc != 0) {
    close(fds[0]);
    return std::nullopt;
  }
  std::string output;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  bool timedOut = false;
  for (;;) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      timedOut = true;
      break;
    }
    pollfd pfd{fds[0], POLLIN, 0};
    const int ready = poll(&pfd, 1, static_cast<int>(left.count()));
    if (ready < 0) continue;  // EINTR
    if (ready == 0) {
      timedOut = true;
      break;
    }
    char buf[512];
    const ssize_t n = read(fds[0], buf, sizeof buf);
    if (n <= 0) break;  // EOF (the tool closed its stdout) or error
    output.append(buf, static_cast<size_t>(n));
    if (output.size() > 4096) break;  // far more than any answer
  }
  close(fds[0]);
  if (timedOut || output.size() > 4096) kill(pid, SIGKILL);
  int status = 0;
  const pid_t waited = waitpid(pid, &status, 0);
  if (timedOut) return std::nullopt;
  if (waited == pid && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) return std::nullopt;
  return output;
}
#endif

}  // namespace

std::optional<ColorScheme> parsePortalColorScheme(std::string_view output) {
  size_t pos = output.find("uint32");
  pos = pos == std::string_view::npos ? 0 : pos + 6;
  while (pos < output.size() && !std::isdigit(static_cast<unsigned char>(output[pos]))) ++pos;
  if (pos >= output.size()) return std::nullopt;
  uint32_t value = 0;
  while (pos < output.size() && std::isdigit(static_cast<unsigned char>(output[pos])) && value < 1000) {
    value = value * 10 + static_cast<uint32_t>(output[pos] - '0');
    ++pos;
  }
  if (value == 1) return ColorScheme::Dark;
  if (value == 2) return ColorScheme::Light;
  return std::nullopt;
}

std::optional<ColorScheme> parseGsettingsColorScheme(std::string_view output) {
  const std::string s = lower(output);
  if (s.find("prefer-dark") != std::string::npos) return ColorScheme::Dark;
  if (s.find("prefer-light") != std::string::npos) return ColorScheme::Light;
  return std::nullopt;
}

std::optional<ColorScheme> parseGtkTheme(std::string_view value) {
  const std::string s = lower(value);
  if (s.find_first_not_of(" \t\r\n") == std::string::npos) return std::nullopt;
  if (s.find(":dark") != std::string::npos || s.find("-dark") != std::string::npos) return ColorScheme::Dark;
  return ColorScheme::Light;  // an explicit theme that is not a dark variant
}

ColorScheme schemeFromAppsUseLightTheme(std::optional<uint32_t> value) {
  if (value && *value == 0) return ColorScheme::Dark;
  return ColorScheme::Light;
}

std::optional<uint32_t> readAppsUseLightTheme() {
#ifdef _WIN32
  DWORD data = 0;
  DWORD size = sizeof data;
  const LSTATUS rc =
      RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &data, &size);
  if (rc != ERROR_SUCCESS) return std::nullopt;
  return static_cast<uint32_t>(data);
#else
  return std::nullopt;
#endif
}

ColorScheme queryOsColorScheme() {
#ifdef _WIN32
  return schemeFromAppsUseLightTheme(readAppsUseLightTheme());
#else
  constexpr int kToolTimeoutMs = 1500;
  if (const auto out =
          runTool({"gdbus", "call", "--session", "--dest", "org.freedesktop.portal.Desktop", "--object-path",
                   "/org/freedesktop/portal/desktop", "--method", "org.freedesktop.portal.Settings.Read",
                   "org.freedesktop.appearance", "color-scheme"},
                  kToolTimeoutMs)) {
    if (const auto scheme = parsePortalColorScheme(*out)) return *scheme;
  }
  if (const auto out =
          runTool({"gsettings", "get", "org.gnome.desktop.interface", "color-scheme"}, kToolTimeoutMs)) {
    if (const auto scheme = parseGsettingsColorScheme(*out)) return *scheme;
  }
  if (const char* gtk = std::getenv("GTK_THEME")) {
    if (const auto scheme = parseGtkTheme(gtk)) return *scheme;
  }
  return ColorScheme::Dark;
#endif
}

OsThemeWatcher::OsThemeWatcher(OsSchemeProbe probe, int intervalMs, ColorScheme initial,
                               std::function<void()> onChange)
    : m_probe(std::move(probe)),
      m_intervalMs(std::max(intervalMs, 1)),
      m_onChange(std::move(onChange)),
      m_current(initial) {
  m_thread = std::thread([this] { run(); });
}

OsThemeWatcher::~OsThemeWatcher() {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stop = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable()) m_thread.join();
}

void OsThemeWatcher::pollNow() {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pollNow = true;
  }
  m_cv.notify_all();
}

void OsThemeWatcher::run() {
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_stop) return;
    }
    ColorScheme scheme = m_current.load(std::memory_order_relaxed);
    try {
      if (m_probe) scheme = m_probe();
    } catch (...) {
      // A failing probe keeps the last answer.
    }
    const ColorScheme before = m_current.exchange(scheme, std::memory_order_acq_rel);
    m_probes.fetch_add(1, std::memory_order_acq_rel);
    if (before != scheme && m_onChange) m_onChange();
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait_for(lock, std::chrono::milliseconds(m_intervalMs), [this] { return m_stop || m_pollNow; });
    m_pollNow = false;
    if (m_stop) return;
  }
}

}  // namespace tachyon::viewer
