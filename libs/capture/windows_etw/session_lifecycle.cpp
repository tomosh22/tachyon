#include "capture/windows_etw/session_lifecycle.h"

#include <string>

namespace tachyon::capture::etw {

namespace {
Error wrongPhase(ErrorCode code, const char* what, SessionPhase phase) {
  return makeError(code, std::string(what) + " (capture is " + sessionPhaseName(phase) + ")");
}
}  // namespace

Result<void> SessionLifecycle::beginStart() {
  if (m_phase != SessionPhase::Idle)
    return wrongPhase(ErrorCode::Busy, "an ETW capture is already in progress", m_phase);
  m_phase = SessionPhase::Starting;
  return ok();
}

void SessionLifecycle::startSucceeded(bool circular) {
  if (m_phase != SessionPhase::Starting) return;
  m_phase = SessionPhase::Running;
  m_segment = 0;
  m_circular = circular;
}

void SessionLifecycle::startFailed() {
  if (m_phase == SessionPhase::Starting) m_phase = SessionPhase::Idle;
}

Result<void> SessionLifecycle::beginFlush() {
  switch (m_phase) {
    case SessionPhase::Idle:
    case SessionPhase::Faulted: return wrongPhase(ErrorCode::NotFound, "no ETW capture is running", m_phase);
    case SessionPhase::Starting:
    case SessionPhase::Flushing:
    case SessionPhase::Stopping: return wrongPhase(ErrorCode::Busy, "capture is busy", m_phase);
    case SessionPhase::Running: break;
  }
  if (!m_circular)
    return makeError(ErrorCode::InvalidArgument, "flush requires circular mode (circularMb > 0)");
  m_phase = SessionPhase::Flushing;
  return ok();
}

void SessionLifecycle::flushRestarted() {
  if (m_phase != SessionPhase::Flushing) return;
  m_phase = SessionPhase::Running;
  ++m_segment;
}

void SessionLifecycle::flushRestartFailed() {
  if (m_phase == SessionPhase::Flushing) m_phase = SessionPhase::Faulted;
}

Result<StopKind> SessionLifecycle::beginStop() {
  switch (m_phase) {
    case SessionPhase::Idle: return wrongPhase(ErrorCode::NotFound, "no ETW capture is running", m_phase);
    case SessionPhase::Starting:
    case SessionPhase::Flushing:
    case SessionPhase::Stopping: return wrongPhase(ErrorCode::Busy, "capture is busy", m_phase);
    case SessionPhase::Running: m_phase = SessionPhase::Stopping; return StopKind::LiveSession;
    case SessionPhase::Faulted: m_phase = SessionPhase::Stopping; return StopKind::NoSession;
  }
  return makeError(ErrorCode::Internal, "unreachable");
}

void SessionLifecycle::stopFinished() {
  if (m_phase == SessionPhase::Stopping) m_phase = SessionPhase::Idle;
}

}  // namespace tachyon::capture::etw
