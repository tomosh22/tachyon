// One ETW kernel logger session (WP-11): StartTrace / ControlTrace / TraceSetInformation through advapi32.
//
// The recipe is the one WP-01 (tools/etlstat) proved on the CI runner: a uniquely named private system logger
// (EVENT_TRACE_SYSTEM_LOGGER_MODE, Windows 8+) with its own GUID, falling back to SystemTraceControlGuid when
// StartTrace insists on it; Wnode.ClientContext = 1 (QPC); the sampled-profile interval set with the session
// handle first and handle 0 as a fallback; stack walking for SampledProfile / CSwitch / ReadyThread.
//
// On non-Windows platforms the class still compiles; every operation returns ErrorCode::Unsupported (orphan
// cleanup reports nothing to do) so the platform-independent code above it stays testable.
#pragma once

#include "capture/windows_etw/session_config.h"
#include "core/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::capture::etw {

// Final or current counters of a session (from EVENT_TRACE_PROPERTIES after ControlTrace).
struct SessionCounters {
  uint64_t eventsLost = 0;
  uint64_t logBuffersLost = 0;
  uint64_t realTimeBuffersLost = 0;
  uint64_t buffersWritten = 0;
  uint64_t numberOfBuffers = 0;
  uint64_t freeBuffers = 0;
  uint64_t bufferSizeKb = 0;
};

// Facts learned while starting a session (the open [VERIFY] items of docs/etw-notes.md).
struct SessionStartInfo {
  bool kernelGuidFallback = false;            // StartTrace needed SystemTraceControlGuid
  bool intervalSetWithSessionHandle = false;  // true: session handle worked; false: needed handle 0
  bool stackTracingEnabled = false;
};

struct OrphanReport {
  std::vector<std::string> stopped;       // Tachyon sessions of dead processes that were stopped
  std::vector<std::string> skippedAlive;  // Tachyon sessions whose owner still runs (left alone)
  std::vector<std::string> failed;        // orphans that could not be stopped ("name: reason")
  size_t inspected = 0;                   // all sessions enumerated, Tachyon or not
};

class EtwSession {
 public:
  // Thread-safe. Stops the session if it is still running (best effort).
  ~EtwSession();
  EtwSession(const EtwSession&) = delete;
  EtwSession& operator=(const EtwSession&) = delete;

  // Thread-safe. Enables SeSystemProfilePrivilege on the process token. NoPrivilege when the token lacks it.
  static Result<void> enableProfilePrivilege();

  // Thread-safe. Creates the file's directory, enables the privilege, starts the session and configures
  // sampling and stack walking. ErrorCode::Busy means the session name already exists (pick another);
  // NoPrivilege means not elevated.
  static Result<std::unique_ptr<EtwSession>> start(const SessionConfig& config);

  // Not thread-safe against other calls on the same object. ControlTrace(EVENT_TRACE_CONTROL_FLUSH): writes
  // the in-memory buffers to the file now.
  Result<void> flushBuffers();
  // Not thread-safe against other calls on the same object. ControlTrace(EVENT_TRACE_CONTROL_QUERY).
  Result<SessionCounters> query();
  // Not thread-safe against other calls on the same object. ControlTrace(EVENT_TRACE_CONTROL_STOP); returns
  // the final counters. After this the session is gone and the ETL file is complete.
  Result<SessionCounters> stop();

  // Thread-safe.
  const SessionConfig& config() const { return m_config; }
  const SessionStartInfo& startInfo() const { return m_startInfo; }
  bool running() const { return m_running; }

  // Thread-safe. Names of all active loggers (QueryAllTraces).
  static Result<std::vector<std::string>> listSessions();

  // Thread-safe. Orphan cleanup (see classifySession): stops every logger whose name starts with "Tachyon "
  // and whose owning pid (parsed from the name) is no longer alive. Never touches other loggers, never stops
  // a session whose owner is alive (a sibling agent). Intended to run at agent startup and before start().
  static Result<OrphanReport> cleanupOrphans();

  // Thread-safe. True when a process with this pid exists (access-denied counts as existing).
  static bool processAlive(uint32_t pid);
  // Thread-safe. Logical processors across all processor groups.
  static uint32_t logicalCoreCount();
  // Thread-safe. Current process id.
  static uint32_t currentPid();

 private:
  EtwSession(SessionConfig config, uint64_t handle, std::vector<uint64_t> properties);

  SessionConfig m_config;
  SessionStartInfo m_startInfo;
  [[maybe_unused]] uint64_t m_handle = 0;               // TRACEHANDLE
  [[maybe_unused]] std::vector<uint64_t> m_properties;  // EVENT_TRACE_PROPERTIES + logger / file name strings
  bool m_running = false;
};

}  // namespace tachyon::capture::etw
