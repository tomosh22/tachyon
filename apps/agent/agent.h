// Agent core: owns the capture backend and the single-capture state machine, and answers control requests.
//
// State machine (one capture at a time):
//   idle -> starting -> capturing -> converting -> done -> idle
// `done` is transient: after conversion the agent emits state(done), capture_done, then state(idle), so
// status requests observe `idle` again right after the capture_done event was sent. A start while the state
// is not idle is rejected with EBUSY. A failed start or conversion returns to idle (conversion failures
// additionally emit an `error` event).
//
// Deferred launch (WP-13): a `start` with mode launch and start_immediately=false launches the target parked
// (suspended) and leaves the state at `starting`. Exactly then a second `start` with start_immediately=true
// is the trigger: the backend starts recording and resumes the target, the state becomes `capturing`; every
// other field of that second request is ignored (the CLI sends exe "-", which is never launched:
// with no parked launch such a request is answered ENOTFOUND "no parked launch"). `cancel` in this state
// terminates the parked process and returns to idle; `stop` is rejected with EBUSY. Only backends
// implementing capture::ITargetWatch (the ETW backend) can defer.
//
// Target exit (WP-13): for such backends the agent watches the launched or attached target of a capturing
// session and, when ITargetWatch::targetExited() turns true, finishes the capture by itself exactly as if a
// `stop` had arrived (converting, capture_done, idle). That is what lets `tachyon-cli capture` end when the
// game exits.
//
// Environment: a launched process gets TACHYON_AGENT_PIPE=<AgentConfig::agentAddress> merged over the
// request's env, so the SDK connects to this agent.
//
// Instrumentation (WP-52): while a capture is starting or capturing, an `inst_hello` from an SDK client opens
// a per-process listener (instrumentationAddress) and a spool file next to the output trace
// (`<out>.inst/inst-<pid>.tachyon-inst`); raw wire frames are appended to the spool as they arrive. When the
// capture ends the connections are drained, the backend converts as usual, and the spools are merged into the
// finished trace (inst::mergeInstrumentation: scope / fiber / frame-mark / marker / plot / thread-name
// chunks, kFlagHasInstrumentation). A `flush` merges a snapshot of the spools into the flushed trace, clipped
// to its window. With no spooled data the trace is left byte for byte as the backend wrote it. A merge
// failure is logged and the spool directory is kept (`tachyon-cli merge` can redo it); the capture still
// completes. An `inst_hello` while no capture is running is answered ENOTFOUND: the SDK then falls back to
// its sidecar file and retries. Only pids that belong to the capture may say hello (else ENOPRIV): any pid in
// system mode, the attach pid in attach mode, the launched root pid (ITargetWatch::rootPid) in launch mode;
// with follow_children the descendants are not known through the backend interface, so any pid is accepted
// there (the peer pid check below still ties the data connection to the hello pid). Backends without
// ITargetWatch (the mock) accept any pid in launch mode. Per capture the agent caps instrumented processes,
// connections per process and spool bytes (AgentConfig), and closes data connections whose peer pid differs
// from the hello pid.
#pragma once

#include "capture/capture_backend.h"
#include "capture/windows_etw/target_watch.h"
#include "control/server.h"
#include "inst/ingest.h"
#include "inst/merge.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace tachyon::agent {

struct AgentConfig {
  std::string backendName;   // reported by status
  std::string agentAddress;  // used to derive instrumentation addresses for inst_hello
  bool elevated = false;     // reported by status

  // Instrumentation ingestion (WP-52).
  // Per-process spool cap in bytes; frames past it are dropped and reported as tachyon.dropped markers.
  uint64_t maxInstSpoolBytes = inst::kDefaultMaxSpoolBytes;
  // Directory for the spool directories; empty: next to the output trace.
  std::filesystem::path instSpoolRoot;
  // At the end of a capture each SDK connection is read until silent for instDrainIdleMs, at most
  // instDrainMaxMs.
  int instDrainIdleMs = 200;
  int instDrainMaxMs = 2000;
  // Keep the spool files after a successful merge (diagnostics).
  bool keepInstSpool = false;
  // Caps per capture: instrumented processes (further inst_hello get EBUSY), simultaneous connections per
  // process (further ones are closed), and spool bytes over all processes (further frames are dropped and
  // counted like the per-process cap).
  uint32_t maxInstProcesses = 256;
  uint32_t maxInstConnectionsPerProcess = 16;
  uint64_t maxInstTotalSpoolBytes = inst::kDefaultMaxTotalSpoolBytes;
  // Close SDK connections whose peer process id is not the pid that said hello (Linux SO_PEERCRED, Windows
  // GetNamedPipeClientProcessId).
  bool verifyInstPeerPid = true;
};

class Agent final : public control::IRequestHandler {
 public:
  using EventSink = std::function<void(const control::Event&)>;

  // Thread-safe.
  Agent(std::unique_ptr<capture::ICaptureBackend> backend, AgentConfig config);
  // Thread-safe: calls shutdown().
  ~Agent() override;

  // Not thread-safe: call before serving requests. Events are emitted from request and worker threads.
  void setEventSink(EventSink sink);

  // Thread-safe.
  control::Response handle(control::ClientId client, const control::Request& request) override;

  // Thread-safe, idempotent. Stops a running capture (discarding nothing: the result is converted and
  // reported like a normal stop) and joins the conversion worker. Stop the server first.
  void shutdown();

  // Thread-safe.
  capture::CaptureState state() const { return m_state.load(); }

 private:
  control::Response handleStatus(std::optional<int64_t> id);
  control::Response handleStart(const control::StartRequest& req);
  control::Response handleStop();
  control::Response handleFlush(const control::FlushRequest& req);
  control::Response handleCancel();
  control::Response handleInstHello(const control::InstHelloRequest& req);

  // Locked: Capturing -> Converting and starts the conversion worker.
  void beginConversionLocked();
  // True while a deferred launch waits for its trigger (guarded by m_mutex).
  bool armedLocked() const;
  void watcherLoop();
  void setStateLocked(capture::CaptureState state, double progress);
  void emit(const control::Event& event);
  void conversionWorker();

  // The instrumentation ingest of the running capture, or null.
  std::shared_ptr<inst::Ingest> currentIngest();
  // Detaches the session's ingest (null when none) and reports whether it is a circular capture.
  std::shared_ptr<inst::Ingest> takeIngest(bool& circular);
  // Stops the ingest, runs the backend's stop() and merges the spools into the result. Never holds m_mutex.
  Result<capture::CaptureResult> stopAndMerge(std::vector<std::string>& warnings);
  // True when inst_hello from `pid` belongs to the running capture (see the class comment).
  bool instPidAllowedLocked(uint32_t pid) const;
  // Merges the spools into the trace of `result`; logs problems instead of failing the capture.
  // Returns the problems worth telling the client (they end up in CaptureSummary::warnings).
  std::vector<std::string> mergeSpools(inst::Ingest& ingest, const std::vector<inst::SpoolInfo>& spools,
                                       const capture::CaptureResult& result, bool clip, bool removeWhenDone);

  std::unique_ptr<capture::ICaptureBackend> m_backend;
  AgentConfig m_config;
  EventSink m_sink;

  // Serialises state transitions and the events they emit. Never held across a backend call (start, stop,
  // flush, checkPrivileges): the backend's progress callback takes it, and a slow backend must not stall
  // status requests or other clients.
  std::mutex m_mutex;
  std::condition_variable m_flushDone;             // signalled when m_flushing drops
  bool m_flushing = false;                         // a backend->flush() is in flight (guarded by m_mutex)
  const capture::ITargetWatch* m_watch = nullptr;  // backend capability, null when absent
  bool m_resuming = false;   // a deferred launch is being triggered or cancelled (guarded by m_mutex)
  bool m_watchStop = false;  // guarded by m_mutex
  std::condition_variable m_watchCv;
  std::thread m_watcher;
  std::atomic<capture::CaptureState> m_state{capture::CaptureState::Idle};
  std::atomic<double> m_progress{0.0};
  std::thread m_worker;

  std::shared_ptr<inst::Ingest> m_ingest;  // guarded by m_mutex
  bool m_circular = false;                 // guarded by m_mutex: the running capture is a flight recorder
  capture::CaptureMode m_sessionMode = capture::CaptureMode::System;  // guarded by m_mutex
  uint32_t m_sessionPid = 0;                                          // attach target; guarded by m_mutex
  bool m_sessionFollow = false;                                       // follow_children; guarded by m_mutex
};

}  // namespace tachyon::agent
