// WindowsEtwBackend: ICaptureBackend over an ETW kernel logger session (WP-11), converting with WP-12's
// EtlConverter on stop.
//
// Modes: System (whole machine), Attach (the pid becomes the converter's target) and Launch (WP-13). On
// non-Windows platforms start() returns Unsupported.
//
// Launch: the kernel session is started FIRST, then the exe is created with CREATE_SUSPENDED (environment =
// agent environment + options.env, so the agent can inject TACHYON_AGENT_PIPE), then its main thread is
// resumed, so the loader and static initialisation are inside the capture. (The work package lists
// CreateProcess before the session; creating the process after the session is deliberate: only then are the
// process start and the image loads of the exe and ntdll logged live. A process created earlier shows up
// only in the end rundown, which does not exist for a process that has already exited when the capture
// stops, and its user-mode frames could not be resolved to modules.) The process joins a job object when
// possible, which lets targetExited() see the whole tree. With startImmediately == false the process is
// created at once and parked (suspended, state Starting, launchPending()) and no session runs; a second
// start() call starts the session and resumes it, and that call's other fields are ignored. In that case
// the exe/ntdll mappings pre-date the session and are known from the stop-time rundown only, so stop the
// capture while the process is still alive (see the proposal in the WP-13 report for etlconvert). stop() on a
// pending launch terminates the process and reports NotFound. A launched process is never killed by a normal
// stop(): the capture ends, the game keeps running.
//
// Child following (followChildren): the converter takes a fixed pid list, so the list is completed at
// conversion time. stop() / flush() first scan the ETL's Process Start/End/DCStart events (extra ProcessTrace
// pass, process events only) with one ProcessTreeTracker per capture (see process_tree.h): a Start whose
// parent is a live target and whose image matches childFilter joins the target set, recursively. Targets are
// handed to the converter as pid + time interval, so a pid reused after the process ended is not kept. The
// tracker survives flushes, so children of earlier circular segments stay targets. There is no live tracking
// of the target list (that needs a real-time consumer, a v1.1 option); the job object is used only for exit
// detection. Children that existed before an attach are not followed, except through a rundown record in a
// later segment.
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
#include "capture/windows_etw/launch.h"
#include "capture/windows_etw/process_tree.h"
#include "capture/windows_etw/session_config.h"
#include "capture/windows_etw/session_lifecycle.h"
#include "capture/windows_etw/target_watch.h"
#include "etlconvert/etl_converter.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
  uint32_t rootPid = 0;                              // launched or attached pid of the last start (0: system)
  std::vector<etw::FollowedChild> followedChildren;  // children found by the last conversion scan
  std::vector<uint32_t> targetPids;                  // pids handed to the last conversion
};

// Thread-safe: all members may be called from any thread. start / stop / flush are serialised through the
// phase logic (a second call while one is in flight gets Busy). The progress callback is invoked without
// internal locks held, from the thread calling stop().
class WindowsEtwBackend final : public ICaptureBackend, public ITargetWatch {
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

  // ITargetWatch. Thread-safe.
  bool launchPending() const override;
  bool targetExited() const override;
  uint32_t rootPid() const override;

 private:
  void notify(CaptureState state, float progress);
  Result<etw::SessionConfig> configureSegment(const CaptureOptions& options, uint32_t segment);
  Result<std::unique_ptr<etw::EtwSession>> startSegment(const CaptureOptions& options, uint32_t segment);
  static etw::LaunchSpec launchSpecFor(const CaptureOptions& options);
  // Lifecycle is already in Starting. Starts the session; then, for Launch mode, creates the suspended
  // process (launchNow) or takes the parked one, and resumes it.
  Result<void> startRecording(const CaptureOptions& options, std::unique_ptr<etw::TargetProcess> target,
                              bool launchNow);
  etlconvert::ConvertOptions convertOptionsFor(const CaptureOptions& options, const etw::SessionConfig& cfg,
                                               const etw::SessionCounters& counters);

  mutable std::mutex m_mutex;
  WindowsEtwBackendConfig m_config;
  CaptureState m_state = CaptureState::Idle;
  etw::SessionLifecycle m_lifecycle;
  CaptureOptions m_options;
  etw::SessionConfig m_sessionConfig;  // config of the live session (for the converter's JSON)
  std::unique_ptr<etw::EtwSession> m_session;
  std::unique_ptr<etw::TargetProcess> m_target;  // launched (or attached) root process, if any
  std::mutex m_trackerMutex;
  std::unique_ptr<etw::ProcessTreeTracker> m_tracker;  // process tree of the live capture, all segments
  bool m_pending = false;                              // launched suspended, waiting for the second start()
  std::atomic<uint32_t> m_nameCounter{0};
  std::chrono::steady_clock::time_point m_startTime;
  std::function<void(CaptureState, float)> m_callback;
  WindowsEtwDiagnostics m_diag;
};

}  // namespace tachyon::capture
