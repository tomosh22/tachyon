#include "agent/agent.h"

#include "capture/windows_etw/launch.h"
#include "core/log.h"

#include <chrono>
#include <system_error>

namespace tachyon::agent {

using capture::CaptureState;
using namespace control;

namespace {

// The exe of the start request that triggers a parked launch (see agent.h).
constexpr const char* kResumeExe = "-";

CaptureSummary toSummary(const capture::CaptureResult& r) {
  CaptureSummary s;
  s.path = r.tracePath.string();
  s.events_lost = r.eventsLost;
  s.duration_s = r.durationS;
  return s;
}

}  // namespace

Agent::Agent(std::unique_ptr<capture::ICaptureBackend> backend, AgentConfig config)
    : m_backend(std::move(backend)), m_config(std::move(config)) {
  m_backend->setProgressCallback([this](CaptureState s, float progress) {
    if (s != CaptureState::Converting || m_state.load() != CaptureState::Converting) return;
    std::lock_guard lock(m_mutex);
    if (m_state.load() == CaptureState::Converting) setStateLocked(CaptureState::Converting, progress);
  });
  m_watch = dynamic_cast<const capture::ITargetWatch*>(m_backend.get());
  if (m_watch != nullptr) m_watcher = std::thread([this] { watcherLoop(); });
}

Agent::~Agent() {
  shutdown();
}

void Agent::setEventSink(EventSink sink) {
  m_sink = std::move(sink);
}

void Agent::emit(const Event& event) {
  if (m_sink) m_sink(event);
}

void Agent::setStateLocked(CaptureState state, double progress) {
  m_state.store(state);
  m_progress.store(progress);
  emit(StateEvent{state, progress});
}

Response Agent::handle(ClientId client, const Request& request) {
  (void)client;
  const std::optional<int64_t> id = request.id;
  Response resp;
  if (std::holds_alternative<StatusRequest>(request.body)) {
    resp = handleStatus(id);
  } else if (const auto* s = std::get_if<StartRequest>(&request.body)) {
    resp = handleStart(*s);
  } else if (std::holds_alternative<StopRequest>(request.body)) {
    resp = handleStop();
  } else if (const auto* f = std::get_if<FlushRequest>(&request.body)) {
    resp = handleFlush(*f);
  } else if (std::holds_alternative<CancelRequest>(request.body)) {
    resp = handleCancel();
  } else if (const auto* h = std::get_if<InstHelloRequest>(&request.body)) {
    resp = handleInstHello(*h);
  }
  resp.id = id;
  return resp;
}

Response Agent::handleStatus(std::optional<int64_t> id) {
  StatusBody body;
  body.state = m_state.load();
  body.progress = m_progress.load();
  body.backend = m_config.backendName;
  body.elevated = m_config.elevated;
  return makeOkResponse(id, body);
}

bool Agent::armedLocked() const {
  return m_state.load() == CaptureState::Starting && m_watch != nullptr && m_watch->launchPending();
}

Response Agent::handleStart(const StartRequest& req) {
  capture::CaptureOptions opts;
  opts.mode = req.mode;
  opts.exe = req.exe;
  opts.cwd = req.cwd;
  opts.args = req.args;
  opts.env = req.env;
  opts.pid = req.pid;
  opts.samplingHz = req.sampling_hz;
  opts.followChildren = req.follow_children;
  opts.childFilter = req.child_filter;
  opts.circularMb = req.circular_mb;
  opts.startImmediately = req.start_immediately;
  opts.stackProfile = req.stackwalk.profile;
  opts.stackCSwitch = req.stackwalk.cswitch;
  opts.stackReady = req.stackwalk.readythread;
  opts.kernelStacks = req.kernel_stacks;
  if (opts.mode == capture::CaptureMode::Launch && !m_config.agentAddress.empty())
    opts.env[std::string(capture::etw::kAgentPipeEnvVar)] = m_config.agentAddress;  // the SDK connects here
  if (req.out_path.empty()) {
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    opts.outPath = "capture-" + std::to_string(now) + ".tachyon";
  } else {
    opts.outPath = req.out_path;
  }

  bool resuming = false;
  {
    std::lock_guard lock(m_mutex);
    // exe "-" is the CLI's `resume`: a trigger for a parked launch, never a program to start.
    if (opts.mode == capture::CaptureMode::Launch && opts.exe == kResumeExe &&
        !(armedLocked() && !m_resuming))
      return makeErrorResponse(
          std::nullopt, makeError(ErrorCode::NotFound,
                                  "no parked launch to resume (start one with --launch ... --no-start)"));
    if (armedLocked() && !m_resuming) {
      // The trigger of a deferred launch (see the class comment).
      if (!req.start_immediately)
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy,
                                                         "a launched process is already waiting to be "
                                                         "started; send start with start_immediately"));
      m_resuming = true;
      resuming = true;
    } else {
      if (m_state.load() != CaptureState::Idle)
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "a capture is already in progress"));
      setStateLocked(CaptureState::Starting, 0.0);
    }
  }

  Result<void> started = resuming ? ok() : m_backend->checkPrivileges();
  if (started.ok()) started = m_backend->start(opts);

  std::lock_guard lock(m_mutex);
  if (resuming) m_resuming = false;
  if (!started.ok()) {
    TACHYON_LOG_WARN("agent: start failed: {}", started.error().toString());
    // A failed trigger can leave the launch parked (the backend decides); anything else is idle again.
    if (!(resuming && m_watch->launchPending())) setStateLocked(CaptureState::Idle, 0.0);
    return makeErrorResponse(std::nullopt, started.error());
  }
  if (!resuming && opts.mode == capture::CaptureMode::Launch && !opts.startImmediately &&
      m_watch != nullptr && m_watch->launchPending()) {
    TACHYON_LOG_INFO("agent: launched {} (pid {}) parked; waiting for start", opts.exe, m_watch->rootPid());
    return makeOkResponse(std::nullopt);  // state stays `starting`
  }
  TACHYON_LOG_INFO("agent: capture started ({}) -> {}", capture::captureModeName(opts.mode),
                   opts.outPath.string());
  setStateLocked(CaptureState::Capturing, 0.0);
  return makeOkResponse(std::nullopt);
}

void Agent::beginConversionLocked() {
  setStateLocked(CaptureState::Converting, 0.0);
  if (m_worker.joinable()) m_worker.join();  // previous worker has already finished all locked work
  m_worker = std::thread([this] { conversionWorker(); });
}

void Agent::watcherLoop() {
  std::unique_lock lock(m_mutex);
  while (!m_watchStop) {
    m_watchCv.wait_for(lock, std::chrono::milliseconds(50), [this] { return m_watchStop; });
    if (m_watchStop) break;
    if (m_state.load() != CaptureState::Capturing || m_flushing) continue;
    lock.unlock();
    const bool exited = m_watch->targetExited();  // never under m_mutex: it takes the backend's lock
    lock.lock();
    if (!exited || m_watchStop || m_state.load() != CaptureState::Capturing || m_flushing) continue;
    TACHYON_LOG_INFO("agent: capture target exited, finishing the capture");
    beginConversionLocked();
  }
}

Response Agent::handleStop() {
  std::unique_lock lock(m_mutex);
  m_flushDone.wait(lock, [this] { return !m_flushing; });  // a flush in flight finishes first
  switch (m_state.load()) {
    case CaptureState::Idle:
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no capture is running"));
    case CaptureState::Capturing: break;
    case CaptureState::Starting:
      if (armedLocked())
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy,
                                                         "the launched process is not recording yet; send "
                                                         "start to begin or cancel to terminate it"));
      return makeErrorResponse(std::nullopt,
                               makeError(ErrorCode::Busy, "capture is not in a stoppable state"));
    default:
      return makeErrorResponse(std::nullopt,
                               makeError(ErrorCode::Busy, "capture is not in a stoppable state"));
  }
  beginConversionLocked();
  return makeOkResponse(std::nullopt);
}

void Agent::conversionWorker() {
  auto result = m_backend->stop();
  std::lock_guard lock(m_mutex);
  if (result.ok()) {
    TACHYON_LOG_INFO("agent: capture done: {} ({} events lost)", result->tracePath.string(),
                     result->eventsLost);
    setStateLocked(CaptureState::Done, 1.0);
    emit(CaptureDoneEvent{toSummary(*result)});
  } else {
    TACHYON_LOG_ERROR("agent: conversion failed: {}", result.error().toString());
    emit(ErrorEvent{result.error().message});
  }
  setStateLocked(CaptureState::Idle, 0.0);
}

Response Agent::handleFlush(const FlushRequest& req) {
  {
    std::lock_guard lock(m_mutex);
    switch (m_state.load()) {
      case CaptureState::Idle:
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no capture is running"));
      case CaptureState::Capturing: break;
      default:
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "capture is not in a flushable state"));
    }
    if (m_flushing)
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy, "a flush is already in progress"));
    m_flushing = true;  // stop / cancel / shutdown wait for it instead of interleaving
  }
  // Clears the flag on every exit path (return or exception), so stop / cancel / shutdown cannot wedge.
  struct FlushGuard {
    Agent& agent;
    ~FlushGuard() {
      {
        std::lock_guard lock(agent.m_mutex);
        agent.m_flushing = false;
      }
      agent.m_flushDone.notify_all();
    }
  } flushGuard{*this};
  // No lock across the backend call (it may take seconds and may call the progress callback).
  auto result = m_backend->flush(req.name, req.seconds);
  if (!result.ok()) return makeErrorResponse(std::nullopt, result.error());
  return makeOkResponse(std::nullopt, toSummary(*result));
}

Response Agent::handleCancel() {
  {
    std::unique_lock lock(m_mutex);
    m_flushDone.wait(lock, [this] { return !m_flushing; });
    switch (m_state.load()) {
      case CaptureState::Idle:
        return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no capture is running"));
      case CaptureState::Capturing: break;
      case CaptureState::Starting:
        if (armedLocked() && !m_resuming) {
          // A parked launch: terminate it (the backend's stop does that) and go back to idle.
          m_resuming = true;
          lock.unlock();
          (void)m_backend->stop();
          lock.lock();
          m_resuming = false;
          TACHYON_LOG_INFO("agent: parked launch cancelled");
          setStateLocked(CaptureState::Idle, 0.0);
          return makeOkResponse(std::nullopt);
        }
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "capture is not in a cancellable state"));
      default:
        return makeErrorResponse(std::nullopt,
                                 makeError(ErrorCode::Busy, "capture is not in a cancellable state"));
    }
    setStateLocked(CaptureState::Converting, 0.0);
  }
  auto result = m_backend->stop();
  if (result.ok()) {
    std::error_code ec;
    std::filesystem::remove(result->tracePath, ec);
    if (!result->rawPath.empty()) std::filesystem::remove(result->rawPath, ec);
  }
  std::lock_guard lock(m_mutex);
  TACHYON_LOG_INFO("agent: capture cancelled");
  setStateLocked(CaptureState::Idle, 0.0);
  if (!result.ok()) return makeErrorResponse(std::nullopt, result.error());
  return makeOkResponse(std::nullopt);
}

Response Agent::handleInstHello(const InstHelloRequest& req) {
  if (req.api_version >> 16 != kProtocolApiVersion >> 16)
    return makeErrorResponse(std::nullopt, makeError(ErrorCode::Internal, "unsupported SDK api_version"));
  InstHelloBody body;
  body.pipe = instrumentationAddress(m_config.agentAddress, req.pid);
  return makeOkResponse(std::nullopt, body);
}

void Agent::shutdown() {
  std::thread watcher;
  {
    std::lock_guard lock(m_mutex);
    m_watchStop = true;
    watcher = std::move(m_watcher);
  }
  m_watchCv.notify_all();
  if (watcher.joinable()) watcher.join();

  bool needStop = false;
  bool parked = false;
  {
    std::unique_lock lock(m_mutex);
    m_flushDone.wait(lock, [this] { return !m_flushing; });
    if (m_state.load() == CaptureState::Capturing) {
      setStateLocked(CaptureState::Converting, 0.0);
      needStop = true;
    } else if (armedLocked()) {
      parked = true;
    }
  }
  if (parked) {
    (void)m_backend->stop();  // terminates the parked process
    std::lock_guard lock(m_mutex);
    setStateLocked(CaptureState::Idle, 0.0);
  }
  if (needStop) {
    auto result = m_backend->stop();
    std::lock_guard lock(m_mutex);
    if (result.ok()) {
      TACHYON_LOG_INFO("agent: shutdown stopped capture: {}", result->tracePath.string());
      setStateLocked(CaptureState::Done, 1.0);
      emit(CaptureDoneEvent{toSummary(*result)});
    } else {
      TACHYON_LOG_ERROR("agent: shutdown stop failed: {}", result.error().toString());
    }
    setStateLocked(CaptureState::Idle, 0.0);
  }
  std::thread worker;
  {
    std::lock_guard lock(m_mutex);
    worker = std::move(m_worker);
  }
  if (worker.joinable()) worker.join();
}

}  // namespace tachyon::agent
