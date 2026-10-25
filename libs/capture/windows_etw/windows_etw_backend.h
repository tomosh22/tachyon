// WindowsEtwBackend: ICaptureBackend over an ETW kernel logger session (WP-11), converting with WP-12's
// EtlConverter on stop.
//
// Modes: System (whole machine) and Attach (pid becomes the converter's target filter). Launch is WP-13 and
// returns Unsupported. On non-Windows platforms start() returns Unsupported.
//
// Files: the ETL of a capture is "<outPath>.etl" (see etlSegmentPath); flush restarts on "<outPath>.<n>.etl".
// A flush writes "<dir of outPath>/<name>.tachyon" (+ "<name>.etl" when keepRawEtl).
//
// Flush gap: flush() runs ControlTrace(FLUSH), ControlTrace(STOP), then StartTrace + TraceSetInformation for
// the next segment. Events between the stop completing and the new session being configured are not
// recorded. The gap is measured (QPC-based steady clock) and reported by diagnostics().lastFlushGapSeconds.
// The stopped segment is converted after the new session is running, so conversion time does not widen it.
// Proposal to remove the gap entirely: start the next session BEFORE stopping the old one (up to 8 system
// loggers may coexist); not done here because the work package prescribes stop-then-restart.
#pragma once

#include "capture/capture_backend.h"
#include "capture/windows_etw/etw_session.h"
#include "capture/windows_etw/session_config.h"
#include "capture/windows_etw/session_lifecycle.h"
#include "etlconvert/etl_converter.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace tachyon::capture {

struct WindowsEtwBackendConfig {
  etw::BufferOverrides buffers;  // overrides for the buffer sizing formula
  bool keepRawEtl = true;        // keep the ETL next to the output (rawPath) instead of deleting it
  bool cleanupOrphans = true;    // stop Tachyon sessions of dead agents at start()
  std::string agentVersion = "tachyon-agent";  // recorded in the trace
};

// What the last start/stop/flush observed; for tests, logs and the report.
struct WindowsEtwDiagnostics {
  etw::SessionCounters lastCounters;          // counters returned by the last ControlTrace(STOP)
  etw::SessionStartInfo startInfo;            // how the last session had to be started
  std::string sessionName;                    // name of the live (or last) session
  double lastFlushGapSeconds = -1.0;          // stop -> new session configured; -1 before the first flush
  etlconvert::ConvertStats lastConvertStats;  // of the last stop() conversion
  uint32_t flushCount = 0;
  etw::OrphanReport lastOrphanReport;
};

// Thread-safe: all members may be called from any thread. start / stop / flush are serialised through the
// phase logic (a second call while one is in flight gets Busy). The progress callback is invoked without
// internal locks held, from the thread calling stop().
class WindowsEtwBackend final : public ICaptureBackend {
 public:
  explicit WindowsEtwBackend(WindowsEtwBackendConfig config = {});
  ~WindowsEtwBackend() override;

  // Windows: enables SeSystemProfilePrivilege (NoPrivilege when not elevated). Elsewhere: Unsupported.
  Result<void> checkPrivileges() override;
  Result<void> start(const CaptureOptions& options) override;
  Result<CaptureResult> stop() override;
  // Circular mode only. `seconds` == 0 keeps everything the circular file holds.
  Result<CaptureResult> flush(std::string name, uint32_t seconds) override;
  CaptureState state() const override;
  void setProgressCallback(std::function<void(CaptureState, float)> callback) override;

  // Thread-safe. A copy of the diagnostics.
  WindowsEtwDiagnostics diagnostics() const;

 private:
  void notify(CaptureState state, float progress);
  Result<etw::SessionConfig> configureSegment(const CaptureOptions& options, uint32_t segment);
  Result<std::unique_ptr<etw::EtwSession>> startSegment(const CaptureOptions& options, uint32_t segment);
  etlconvert::ConvertOptions convertOptionsFor(const CaptureOptions& options, const etw::SessionConfig& cfg,
                                               const etw::SessionCounters& counters) const;

  mutable std::mutex m_mutex;
  WindowsEtwBackendConfig m_config;
  CaptureState m_state = CaptureState::Idle;
  etw::SessionLifecycle m_lifecycle;
  CaptureOptions m_options;
  etw::SessionConfig m_sessionConfig;  // config of the live session (for the converter's JSON)
  std::unique_ptr<etw::EtwSession> m_session;
  std::atomic<uint32_t> m_nameCounter{0};
  std::chrono::steady_clock::time_point m_startTime;
  std::function<void(CaptureState, float)> m_callback;
  WindowsEtwDiagnostics m_diag;
};

}  // namespace tachyon::capture
