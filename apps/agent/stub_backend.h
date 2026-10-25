// Placeholder for the real capture backends (WP-11/12 ETW, WP-70 perf) until they are implemented.
#pragma once

#include "agent/privileges.h"
#include "capture/capture_backend.h"

namespace tachyon::agent {

// Thread-safe. Every operation except checkPrivileges fails with ErrorCode::Unsupported.
class StubBackend final : public capture::ICaptureBackend {
 public:
  explicit StubBackend(std::string name) : m_name(std::move(name)) {}

  Result<void> checkPrivileges() override {
    if (!hasCapturePrivileges()) return makeError(ErrorCode::NoPrivilege, "capture privileges required");
    return ok();
  }
  Result<void> start(const capture::CaptureOptions&) override { return unsupported(); }
  Result<capture::CaptureResult> stop() override { return unsupported(); }
  Result<capture::CaptureResult> flush(std::string, uint32_t) override { return unsupported(); }
  capture::CaptureState state() const override { return capture::CaptureState::Idle; }
  void setProgressCallback(std::function<void(capture::CaptureState, float)>) override {}

 private:
  Error unsupported() const {
    return makeError(ErrorCode::Unsupported, m_name + " backend is not implemented yet");
  }
  std::string m_name;
};

}  // namespace tachyon::agent
