// A scriptable backend that implements ITargetWatch, so the agent's launch / deferral / target-exit logic is
// testable on every platform (WP-13).
#pragma once

#include "capture/capture_backend.h"
#include "capture/windows_etw/target_watch.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <stdexcept>

namespace tachyon::agent::testing_util {

class FakeLaunchBackend final : public capture::ICaptureBackend, public capture::ITargetWatch {
 public:
  explicit FakeLaunchBackend(std::filesystem::path fixture) : m_fixture(std::move(fixture)) {}

  // ---- script knobs (thread-safe) ----
  std::atomic<bool> targetHasExited{false};
  std::atomic<bool> failNextTrigger{false};  // the next trigger start fails and drops the parked launch
  std::atomic<bool> flushThrows{false};
  std::atomic<uint64_t> eventsLostOnStop{0};  // reported by the next stop()

  // ---- observations ----
  capture::CaptureOptions lastStart() const {
    std::lock_guard lock(m_mutex);
    return m_lastStart;
  }
  int startCalls() const { return m_startCalls.load(); }
  int stopCalls() const { return m_stopCalls.load(); }

  Result<void> checkPrivileges() override { return ok(); }

  Result<void> start(const capture::CaptureOptions& o) override {
    std::lock_guard lock(m_mutex);
    ++m_startCalls;
    if (m_pending) {  // the trigger
      if (failNextTrigger.exchange(false)) {
        m_pending = false;
        return makeError(ErrorCode::IoError, "fake: the session could not start");
      }
      m_pending = false;
      m_capturing = true;
      targetHasExited = false;
      return ok();
    }
    if (m_capturing) return makeError(ErrorCode::Busy, "fake: already capturing");
    m_lastStart = o;
    m_pid = 4242;
    targetHasExited = false;
    if (o.mode == capture::CaptureMode::Launch && !o.startImmediately) {
      m_pending = true;
      return ok();
    }
    m_capturing = true;
    return ok();
  }

  Result<capture::CaptureResult> stop() override {
    std::lock_guard lock(m_mutex);
    ++m_stopCalls;
    if (m_pending) {
      m_pending = false;
      return makeError(ErrorCode::NotFound, "fake: parked launch terminated");
    }
    if (!m_capturing) return makeError(ErrorCode::NotFound, "fake: not capturing");
    m_capturing = false;
    std::error_code ec;
    std::filesystem::copy_file(m_fixture, m_lastStart.outPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    capture::CaptureResult r;
    r.tracePath = m_lastStart.outPath;
    r.durationS = 0.25;
    r.eventsLost = eventsLostOnStop.load();
    return r;
  }

  Result<capture::CaptureResult> flush(std::string, uint32_t) override {
    if (flushThrows) throw std::runtime_error("fake flush failure");
    return makeError(ErrorCode::InvalidArgument, "fake: not circular");
  }

  capture::CaptureState state() const override {
    std::lock_guard lock(m_mutex);
    return m_pending ? capture::CaptureState::Starting
                     : (m_capturing ? capture::CaptureState::Capturing : capture::CaptureState::Idle);
  }
  void setProgressCallback(std::function<void(capture::CaptureState, float)>) override {}

  bool launchPending() const override {
    std::lock_guard lock(m_mutex);
    return m_pending;
  }
  bool targetExited() const override {
    std::lock_guard lock(m_mutex);
    return m_capturing && targetHasExited.load();
  }
  uint32_t rootPid() const override {
    std::lock_guard lock(m_mutex);
    return (m_pending || m_capturing) ? m_pid : 0;
  }

 private:
  std::filesystem::path m_fixture;
  mutable std::mutex m_mutex;
  capture::CaptureOptions m_lastStart;
  bool m_pending = false;
  bool m_capturing = false;
  uint32_t m_pid = 0;
  std::atomic<int> m_startCalls{0};
  std::atomic<int> m_stopCalls{0};
};

}  // namespace tachyon::agent::testing_util
