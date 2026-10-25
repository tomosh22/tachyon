// Capture backend interface (Part 3.5). Implementations: MockBackend (WP-10), WindowsEtwBackend (WP-11/12),
// LinuxPerfBackend (WP-70). Orchestrator-owned after WP-10 merge.
#pragma once

#include "core/result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tachyon::capture {

enum class CaptureMode { Launch, Attach, System };

// State names match the protocol "state" event values.
enum class CaptureState { Idle, Starting, Capturing, Converting, Done };

// Thread-safe: pure function.
constexpr std::string_view captureModeName(CaptureMode m) {
  switch (m) {
    case CaptureMode::Launch: return "launch";
    case CaptureMode::Attach: return "attach";
    case CaptureMode::System: return "system";
  }
  return "unknown";
}

// Thread-safe: pure function.
constexpr std::string_view captureStateName(CaptureState s) {
  switch (s) {
    case CaptureState::Idle: return "idle";
    case CaptureState::Starting: return "starting";
    case CaptureState::Capturing: return "capturing";
    case CaptureState::Converting: return "converting";
    case CaptureState::Done: return "done";
  }
  return "unknown";
}

// Thread-safe: pure function.
inline std::optional<CaptureMode> parseCaptureMode(std::string_view s) {
  for (CaptureMode m : {CaptureMode::Launch, CaptureMode::Attach, CaptureMode::System})
    if (captureModeName(m) == s) return m;
  return std::nullopt;
}

// Thread-safe: pure function.
inline std::optional<CaptureState> parseCaptureState(std::string_view s) {
  for (CaptureState st : {CaptureState::Idle, CaptureState::Starting, CaptureState::Capturing,
                          CaptureState::Converting, CaptureState::Done})
    if (captureStateName(st) == s) return st;
  return std::nullopt;
}

struct CaptureOptions {
  CaptureMode mode = CaptureMode::System;
  std::string exe, cwd;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  uint32_t pid = 0;
  uint32_t samplingHz = 8000;
  bool followChildren = false;
  std::string childFilter;
  uint32_t circularMb = 0;
  std::filesystem::path outPath;
  bool startImmediately = true;
  bool stackProfile = true, stackCSwitch = true, stackReady = true;
  bool kernelStacks = true;
};

struct CaptureResult {
  std::filesystem::path tracePath;
  uint64_t eventsLost = 0;
  std::filesystem::path rawPath;
  double durationS = 0.0;
};

class ICaptureBackend {
 public:
  virtual ~ICaptureBackend() = default;
  // Implementations are thread-safe unless stated otherwise by the concrete class.
  virtual Result<void> checkPrivileges() = 0;             // SeSystemProfilePrivilege / CAP_PERFMON
  virtual Result<void> start(const CaptureOptions&) = 0;  // returns once the kernel session is live
  virtual Result<CaptureResult> stop() = 0;               // stops session, converts, returns result
  virtual Result<CaptureResult> flush(std::string name, uint32_t seconds) = 0;  // circular mode only
  virtual CaptureState state() const = 0;
  virtual void setProgressCallback(std::function<void(CaptureState, float)>) = 0;
};

}  // namespace tachyon::capture
