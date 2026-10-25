// Agent core: owns the capture backend and the single-capture state machine, and answers control requests.
//
// State machine (one capture at a time):
//   idle -> starting -> capturing -> converting -> done -> idle
// `done` is transient: after conversion the agent emits state(done), capture_done, then state(idle), so
// status requests observe `idle` again right after the capture_done event was sent. A start while the state
// is not idle is rejected with EBUSY. A failed start or conversion returns to idle (conversion failures
// additionally emit an `error` event).
#pragma once

#include "capture/capture_backend.h"
#include "control/server.h"

#include <atomic>
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

  void setStateLocked(capture::CaptureState state, double progress);
  void emit(const control::Event& event);
  void conversionWorker();

  std::unique_ptr<capture::ICaptureBackend> m_backend;
  AgentConfig m_config;
  EventSink m_sink;

  std::mutex m_mutex;  // serialises state transitions and the events they emit
  std::atomic<capture::CaptureState> m_state{capture::CaptureState::Idle};
  std::atomic<double> m_progress{0.0};
  std::thread m_worker;
};

}  // namespace tachyon::agent
