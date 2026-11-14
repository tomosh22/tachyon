// CaptureForm (WP-41): the values of the Run / Attach / System capture dialogs, their validation and the
// translation into a control::StartRequest. Pure data and functions: the dialogs only edit a CaptureForm.
#pragma once

#include "capture/capture_backend.h"
#include "control/protocol.h"
#include "viewer/settings.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct CaptureForm {
  capture::CaptureMode mode = capture::CaptureMode::Launch;
  // Launch
  std::string exe, args, cwd;
  std::vector<std::string> env;  // "KEY=VALUE"
  bool startImmediately = true;  // false: park the launch until Resume
  // Attach
  uint32_t pid = 0;
  std::string processName;  // of the chosen process (names the output file)
  // Common
  int samplingHz = 8000;
  int circularMb = 0;  // 0 = off
  bool followChildren = false;
  std::string childFilter;
  bool kernelStacks = true;
  std::string outPath;  // empty = <captures dir>/<stem>-<YYYYMMDD-HHMMSS>.tachyon
  // WP-61: the agent is on another machine. exe / cwd are paths there (not checked against this file
  // system), and outPath is only a file name for the agent to use in its own output directory.
  bool remote = false;
  // WP-65b: start the capture with live:true (the Live dialog): the agent streams the last 30 s to the viewer
  // while it records. Not persisted.
  bool live = false;

  // Thread-safe: pure function on the value.
  bool operator==(const CaptureForm&) const = default;
};

inline constexpr int kMinSamplingHz = 1;
inline constexpr int kMaxSamplingHz = 100000;
inline constexpr int kMinCircularMb = 4;  // same bounds as the ETW backend (windows_etw/session_config.h)
inline constexpr int kMaxCircularMb = 64 * 1024;

struct FormError {
  std::string
      field;  // "exe", "args", "cwd", "env", "pid", "samplingHz", "circularMb", "childFilter", "outPath"
  std::string message;
};

// Thread-safe: pure function on the value. Copies the persisted last-used values into a form.
CaptureForm formFromSettings(const CaptureSettings& settings, capture::CaptureMode mode);
// Thread-safe: pure function. Stores the reusable values (not pid / output path) back.
void storeFormInSettings(const CaptureForm& form, CaptureSettings& settings);

// Thread-safe (reads the filesystem unless form.remote). Every problem of the form, empty when it can be
// sent. Launch: the executable must exist and be a file (remote: be non-empty), the working directory (if
// any) a directory, arguments must split, environment entries must be KEY=VALUE. Attach: pid > 0. Numbers
// must be in range; the child filter needs follow-children; the output directory (if the path is given) must
// not be a file.
std::vector<FormError> validateForm(const CaptureForm& form);

// Thread-safe (reads the filesystem for the default name only through `capturesDir`). Builds the request;
// fails with the first validation message when the form is invalid. The output path is made absolute.
Result<control::StartRequest> buildStartRequest(const CaptureForm& form,
                                                const std::filesystem::path& capturesDir, std::time_t now);

// Thread-safe: pure function. "launch /path/app", "attach pid 12 (name)", "system".
std::string describeTarget(const CaptureForm& form);

}  // namespace tachyon::viewer
