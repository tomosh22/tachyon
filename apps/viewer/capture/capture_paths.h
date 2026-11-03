// Paths and small text helpers of the capture UI (WP-41).
#pragma once

#include "viewer/paths.h"
#include "viewer/settings.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::viewer {

// Thread-safe: pure function. %LOCALAPPDATA%/Tachyon when `windowsStyle`, otherwise $XDG_DATA_HOME/tachyon
// falling back to ~/.local/share/tachyon. Empty path if no base directory is known.
std::filesystem::path dataDirFromEnv(const EnvLookup& env, bool windowsStyle);
// Thread-safe: reads the process environment.
std::filesystem::path defaultDataDir();
// Thread-safe: reads the process environment. `settings.capturesDir`, else <user data dir>/captures, else
// ./captures.
std::filesystem::path capturesDir(const CaptureSettings& settings);

// Thread-safe: pure function. "<stem>-YYYYMMDD-HHMMSS.tachyon" inside `dir`; `stem` is reduced to characters
// that are safe in file names (empty -> "capture").
std::filesystem::path defaultCaptureFile(const std::filesystem::path& dir, std::string_view stem,
                                         std::time_t when);

// Thread-safe: pure function. Splits `text` like a shell: whitespace separates, '...' is literal, "..."
// allows a backslash before a quote or a backslash; outside quotes a backslash escapes only a space, a quote
// or a backslash (so Windows paths such as C:\tools\x.exe survive). `error` is set (and the result empty) on
// an unterminated quote.
std::vector<std::string> splitArguments(std::string_view text, std::string& error);

// Thread-safe: reads the OS. Path of the running executable (empty if unknown).
std::filesystem::path currentExecutablePath();

// Thread-safe: reads the OS. Windows: process token is elevated. Linux: effective uid 0 or CAP_PERFMON
// (CapEff bit 38). Same rule as apps/agent/privileges.cpp, copied because the agent target is not a library
// the viewer links (apps/agent is off limits for WP-41).
bool currentProcessHasCapturePrivileges();

}  // namespace tachyon::viewer
