// Capture backend interface (Part 3.5). Implementations: MockBackend (WP-10), WindowsEtwBackend (WP-11/12),
// LinuxPerfBackend (WP-70). Orchestrator-owned after WP-10 merge.
#pragma once

#include "core/result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

// WP-66b: identity (st_dev/st_ino; volume serial and file index on Windows) of an output file the caller
// reserved. The backend writes its final trace into exactly that file and fails if the path holds another.
struct ReservedOutput {
  uint64_t volume = 0;
  uint64_t index = 0;
};

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
  // WP-65a (appended): decode events as they arrive and hand them to the ILiveSink set with setLiveSink().
  bool live = false;
  // WP-66b (appended): the file reserved at outPath, when the caller created it; the final trace (not a
  // flush) is written into exactly that file.
  std::optional<ReservedOutput> reservedOutput;
};

struct CaptureResult {
  std::filesystem::path tracePath;
  uint64_t eventsLost = 0;
  std::filesystem::path rawPath;
  double durationS = 0.0;
  // Problems that did not fail the capture (lost samples, throttling, ...) with advice on what to change. The
  // agent adds them to capture_done.warnings. Appended last so positional initialisers keep working.
  std::vector<std::string> warnings;
};

// ---- Live events (WP-65a, docs/specs/live.md) ----
//
// Backend-neutral, already decoded events with RAW identities (pids, tids, addresses, module paths, strings):
// the sink interns them into indices. Timestamps are in the trace clock, as in the final trace. All spans and
// string_views are valid only for the duration of the call.

// Constant facts of the capture, sent once before any event.
struct LiveSessionInfo {
  uint64_t clockFrequencyHz = 0;  // the trace clock (header.clock_frequency_hz)
  uint32_t platform = 0;          // trace::kPlatformWindows / kPlatformLinux
  uint32_t samplingHz = 0;
  uint64_t startTs = 0;  // trace clock when the capture started
};

struct LiveProcess {
  uint32_t pid = 0, ppid = 0;
  uint64_t startTs = 0, endTs = 0;  // endTs 0: still running
  std::string name, commandLine;
};

// A thread appeared, was renamed (the same tid/pid with a new name) or ended (endTs != 0).
struct LiveThread {
  uint32_t tid = 0, pid = 0;
  uint64_t startTs = 0, endTs = 0;
  std::string name;
  bool isTarget = false;  // belongs to the capture's target process(es)
};

// An executable image of a process. Reported when it first matters (load) and again with unloadTs != 0 when
// it goes away. A module is identified by (pid, base, size, path). `base` follows the trace-format rule (ELF:
// lowest PT_LOAD, PE: image base); a frame's RVA is address - base.
struct LiveModule {
  uint32_t pid = 0;
  uint64_t base = 0, size = 0;
  std::string path;
  uint8_t idKind = 0;  // trace::kModuleIdPdb / kModuleIdGnuBuildId / kModuleIdPeTimestampSize, 0 = none
  uint8_t idLen = 0;
  std::array<uint8_t, 32> id{};
  uint32_t age = 0;
  uint64_t loadTs = 0, unloadTs = 0;
};

// A context switch (trace::ContextSwitchRec with raw ids). `oldStack` is the blocking stack of the thread
// that switched out (leaf first), resolved in the address space of `oldPid`; it may be empty.
struct LiveContextSwitch {
  uint64_t ts = 0;
  uint32_t cpu = 0;
  uint32_t oldPid = 0, oldTid = 0, newPid = 0, newTid = 0;  // pid 0: not known; tid 0: the idle task
  uint8_t oldState = 0, oldWaitReason = 0, oldWaitMode = 0, newPriority = 0;
  uint32_t flags = 0;  // trace::kSwitchFlag*
  std::span<const uint64_t> oldStack;
};

// A ready-thread (wakeup) event (trace::ReadyRec). `readyingStack` is the waker's stack in `readyingPid`.
struct LiveReady {
  uint64_t ts = 0;
  uint32_t cpu = 0;
  uint32_t readiedPid = 0, readiedTid = 0;
  uint32_t readyingPid = 0, readyingTid = 0;
  uint8_t adjustReason = 0, flags = 0;
  std::span<const uint64_t> readyingStack;
};

struct LiveScopeEvent {
  uint64_t ts = 0;
  uint32_t pid = 0, tid = 0;
  uint8_t kind = 0;  // trace::kScopeBegin / kScopeEnd
  std::string_view id, data;
  uint32_t colorRgba = 0;
  uint64_t callerAddr = 0;
};

struct LiveFrameMark {
  uint64_t ts = 0;
  uint32_t pid = 0, tid = 0;
  std::string_view name;
  uint64_t frameNumber = 0;
};

struct LiveMarker {
  uint64_t ts = 0;
  uint32_t pid = 0, tid = 0;  // tid 0: a global marker
  std::string_view name, data;
  uint32_t colorRgba = 0;
};

struct LivePlot {
  uint64_t ts = 0;
  std::string_view name;
  double value = 0;
};

// Receives the events of a live capture. Every method has an empty default so that a consumer implements what
// it needs. Thread-safety: a backend calls the sink from ONE thread at a time (it serialises its own calls);
// the agent's instrumentation ingest (onThread names, onScopeEvent, onFrameMark, onMarker, onPlot) calls from
// its connection threads, so those methods must tolerate calls concurrent with the backend's. LiveEncoder
// (libs/control) is fully internally synchronised. A sink must never block for long: a backend that has to
// wait for it drops live events instead (and says so with onDropped), never trace events.
class ILiveSink {
 public:
  virtual ~ILiveSink() = default;
  // Once, before any other call.
  virtual void onSessionInfo(const LiveSessionInfo&) {}
  virtual void onProcess(const LiveProcess&) {}
  virtual void onThread(const LiveThread&) {}
  virtual void onModule(const LiveModule&) {}
  // `ips` are instruction pointers / return addresses, leaf first; kernel addresses are absolute. `flags` are
  // the SampleBatch flags (trace::kSampleFlagKernelLeaf).
  virtual void onSample(uint64_t /*ts*/, uint32_t /*pid*/, uint32_t /*tid*/, uint32_t /*cpu*/,
                        uint16_t /*flags*/, std::span<const uint64_t> /*ips*/) {}
  virtual void onContextSwitch(const LiveContextSwitch&) {}
  virtual void onReady(const LiveReady&) {}
  virtual void onScopeEvent(const LiveScopeEvent&) {}
  virtual void onFrameMark(const LiveFrameMark&) {}
  virtual void onMarker(const LiveMarker&) {}
  virtual void onPlot(const LivePlot&) {}
  // The capture's clock is at `ts` (a heartbeat, so a quiet capture still scrolls).
  virtual void onTimeAdvance(uint64_t /*ts*/) {}
  // `events` live events were dropped because the live path fell behind (the final trace is unaffected).
  virtual void onDropped(uint64_t /*events*/) {}
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
  // WP-65a. Call before start(). With CaptureOptions::live the backend streams decoded events to `sink` (see
  // ILiveSink). The default implementation supports no live source: start() with `live` set must then fail
  // with ErrorCode::Unsupported.
  virtual void setLiveSink(std::shared_ptr<ILiveSink> /*sink*/) {}
  // True when the backend can stream live events (the agent refuses a live start otherwise).
  virtual bool supportsLive() const { return false; }
};

}  // namespace tachyon::capture
