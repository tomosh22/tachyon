// WP-67e: the operating system's light / dark preference, for ThemeKind::System.
//
// Windows: HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize, DWORD AppsUseLightTheme
// (0 = dark, anything else or missing = light). Linux: the xdg-desktop-portal setting
// org.freedesktop.appearance color-scheme read through `gdbus` (1 = dark, 2 = light, 0 = no preference), then
// `gsettings get org.gnome.desktop.interface color-scheme`, then the GTK_THEME environment variable; when
// nothing answers the scheme is dark. No library is linked for it: the tools are run as child processes with
// a short timeout, always off the UI thread (OsThemeWatcher).
#pragma once

#include "viewer/theme.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace tachyon::viewer {

// What the probe of an App returns. Blocking is fine: it runs on the watcher's thread.
using OsSchemeProbe = std::function<ColorScheme()>;

// Thread-safe: pure. Parses the output of
// `gdbus call ... org.freedesktop.portal.Settings.Read org.freedesktop.appearance color-scheme`
// ("(<<uint32 1>>,)", also "(<uint32 1>,)" and a bare "1"): 1 = dark, 2 = light; 0 / unparsable = nullopt.
std::optional<ColorScheme> parsePortalColorScheme(std::string_view output);
// Thread-safe: pure. Parses `gsettings get org.gnome.desktop.interface color-scheme` ("'prefer-dark'",
// "'prefer-light'"; "'default'" and anything else = nullopt).
std::optional<ColorScheme> parseGsettingsColorScheme(std::string_view output);
// Thread-safe: pure. GTK_THEME value ("Adwaita:dark", "Adwaita-dark"): dark when it ends in ":dark" or
// contains "-dark" / ":dark" (case-insensitive); a non-empty other name is light; empty = nullopt.
std::optional<ColorScheme> parseGtkTheme(std::string_view value);
// Thread-safe: pure. Windows: the AppsUseLightTheme value (missing = light).
ColorScheme schemeFromAppsUseLightTheme(std::optional<uint32_t> value);

// Thread-safe. Windows: reads AppsUseLightTheme from the registry (nullopt when the value is absent).
// Elsewhere: always nullopt.
std::optional<uint32_t> readAppsUseLightTheme();

// Thread-safe, blocking (Linux: up to a few seconds in the worst case, each tool is killed after ~1.5 s).
// The real probe of the interactive viewer; never throws and never fails: unknown = Dark.
ColorScheme queryOsColorScheme();

// Polls a probe on a background thread, first right away, then every `intervalMs`. current() is a lock-free
// read for the UI thread; `onChange` (optional, called on the watcher thread) fires when the value changes,
// e.g. to wake the event loop. Destruction stops the thread (waiting for a probe still running).
class OsThemeWatcher {
 public:
  // Thread-safe. `initial` is what current() returns until the first probe answered.
  OsThemeWatcher(OsSchemeProbe probe, int intervalMs, ColorScheme initial, std::function<void()> onChange);
  ~OsThemeWatcher();
  OsThemeWatcher(const OsThemeWatcher&) = delete;
  OsThemeWatcher& operator=(const OsThemeWatcher&) = delete;

  // Thread-safe.
  ColorScheme current() const { return m_current.load(std::memory_order_acquire); }
  // Thread-safe. Number of probes that completed so far.
  uint64_t probeCount() const { return m_probes.load(std::memory_order_acquire); }
  // Thread-safe. Probe again soon (e.g. the window got focus).
  void pollNow();

 private:
  void run();

  OsSchemeProbe m_probe;
  int m_intervalMs;
  std::function<void()> m_onChange;
  std::atomic<ColorScheme> m_current;
  std::atomic<uint64_t> m_probes{0};
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_stop = false;
  bool m_pollNow = false;
  std::thread m_thread;  // last: starts in the constructor body
};

}  // namespace tachyon::viewer
