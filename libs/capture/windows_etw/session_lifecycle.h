// Start / flush / stop phase logic of the ETW backend, separated from the Windows calls so it is unit-tested
// on every platform.
//
//   Idle --beginStart--> Starting --startSucceeded--> Running --beginStop--> Stopping --stopFinished--> Idle
//                            \--startFailed--> Idle      |  ^
//                                                        |  +-- flushRestarted (segment + 1)
//                                          beginFlush    v  |
//                                                     Flushing --flushRestartFailed--> Faulted
//   Faulted --beginStop--> Stopping (no live session: nothing to stop, the last segment is already converted)
//
// A flush stops the session, restarts it on the next ETL segment and converts the stopped segment. Only the
// stop/restart part happens in the Flushing phase; the conversion runs after flushRestarted(), while the new
// session is already recording.
#pragma once

#include "core/result.h"

#include <cstdint>

namespace tachyon::capture::etw {

enum class SessionPhase : uint8_t { Idle, Starting, Running, Flushing, Stopping, Faulted };

// Thread-safe: pure function.
constexpr const char* sessionPhaseName(SessionPhase p) {
  switch (p) {
    case SessionPhase::Idle: return "idle";
    case SessionPhase::Starting: return "starting";
    case SessionPhase::Running: return "running";
    case SessionPhase::Flushing: return "flushing";
    case SessionPhase::Stopping: return "stopping";
    case SessionPhase::Faulted: return "faulted";
  }
  return "unknown";
}

enum class StopKind : uint8_t {
  LiveSession,  // a session is running: stop it and convert its segment
  NoSession,    // a failed flush restart left no session; the capture is over
};

// Not thread-safe: the owner serialises calls (the backend holds its mutex).
class SessionLifecycle {
 public:
  SessionPhase phase() const { return m_phase; }
  // Index of the ETL segment the live session writes to (0 until the first flush).
  uint32_t segment() const { return m_segment; }
  bool circular() const { return m_circular; }

  // Idle -> Starting, else Busy.
  Result<void> beginStart();
  // Starting -> Running; resets the segment index.
  void startSucceeded(bool circular);
  // Starting -> Idle.
  void startFailed();

  // Running and circular -> Flushing. NotFound when idle/faulted, Busy while starting/flushing/stopping,
  // InvalidArgument when the capture is not in circular mode.
  Result<void> beginFlush();
  // Flushing -> Running on the next segment.
  void flushRestarted();
  // Flushing -> Faulted (the old session is stopped, the new one could not be started).
  void flushRestartFailed();

  // Running -> Stopping (LiveSession), Faulted -> Stopping (NoSession). NotFound when idle, Busy otherwise.
  Result<StopKind> beginStop();
  // Stopping -> Idle.
  void stopFinished();

 private:
  SessionPhase m_phase = SessionPhase::Idle;
  uint32_t m_segment = 0;
  bool m_circular = false;
};

}  // namespace tachyon::capture::etw
