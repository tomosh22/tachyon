#include "agent/agent.h"

#include "core/log.h"

#include <chrono>
#include <system_error>

namespace tachyon::agent {

using capture::CaptureState;
using namespace control;

namespace {

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
  if (req.out_path.empty()) {
    const auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    opts.outPath = "capture-" + std::to_string(now) + ".tachyon";
  } else {
    opts.outPath = req.out_path;
  }

  {
    std::lock_guard lock(m_mutex);
    if (m_state.load() != CaptureState::Idle)
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::Busy, "a capture is already in progress"));
    setStateLocked(CaptureState::Starting, 0.0);
  }

  Result<void> started = m_backend->checkPrivileges();
  if (started.ok()) started = m_backend->start(opts);

  std::lock_guard lock(m_mutex);
  if (!started.ok()) {
    TACHYON_LOG_WARN("agent: start failed: {}", started.error().toString());
    setStateLocked(CaptureState::Idle, 0.0);
    return makeErrorResponse(std::nullopt, started.error());
  }
  TACHYON_LOG_INFO("agent: capture started ({}) -> {}", capture::captureModeName(opts.mode),
                   opts.outPath.string());
  setStateLocked(CaptureState::Capturing, 0.0);
  return makeOkResponse(std::nullopt);
}

Response Agent::handleStop() {
  std::unique_lock lock(m_mutex);
  m_flushDone.wait(lock, [this] { return !m_flushing; });  // a flush in flight finishes first
  switch (m_state.load()) {
    case CaptureState::Idle:
      return makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "no capture is running"));
    case CaptureState::Capturing: break;
    default:
      return makeErrorResponse(std::nullopt,
                               makeError(ErrorCode::Busy, "capture is not in a stoppable state"));
  }
  setStateLocked(CaptureState::Converting, 0.0);
  if (m_worker.joinable()) m_worker.join();  // previous worker has already finished all locked work
  m_worker = std::thread([this] { conversionWorker(); });
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
  // No lock across the backend call (it may take seconds and may call the progress callback).
  auto result = m_backend->flush(req.name, req.seconds);
  {
    std::lock_guard lock(m_mutex);
    m_flushing = false;
  }
  m_flushDone.notify_all();
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
  bool needStop = false;
  {
    std::unique_lock lock(m_mutex);
    m_flushDone.wait(lock, [this] { return !m_flushing; });
    if (m_state.load() == CaptureState::Capturing) {
      setStateLocked(CaptureState::Converting, 0.0);
      needStop = true;
    }
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
