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
#pragma once

#include "capture/capture_backend.h"
#include "capture/windows_etw/target_watch.h"
#include "control/server.h"

#include <atomic>
#include <condition_variable>
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
};

}  // namespace tachyon::agent
