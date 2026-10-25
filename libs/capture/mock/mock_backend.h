// MockBackend: a capture backend that "captures" by copying a fixture trace to the requested output path.
#pragma once

#include "capture/capture_backend.h"

#include <chrono>
#include <filesystem>
#include <mutex>

namespace tachyon::capture {

struct MockBackendConfig {
  // Source trace to copy. If empty, the TACHYON_MOCK_FIXTURE environment variable is read at start().
  std::filesystem::path fixture;
  // Artificial conversion delay spent in stop(), split into progressSteps progress callbacks.
  std::chrono::milliseconds captureDuration{0};
  uint32_t progressSteps = 4;
};

// Thread-safe: all members may be called from any thread. The progress callback is invoked from the thread
// calling stop(), without internal locks held.
class MockBackend final : public ICaptureBackend {
 public:
  explicit MockBackend(MockBackendConfig config = {});

  Result<void> checkPrivileges() override;
  Result<void> start(const CaptureOptions& options) override;
  Result<CaptureResult> stop() override;
  // Works only while capturing with circularMb > 0; writes <outPath dir>/<name>.tachyon.
  Result<CaptureResult> flush(std::string name, uint32_t seconds) override;
  CaptureState state() const override;
  void setProgressCallback(std::function<void(CaptureState, float)> callback) override;

 private:
  Result<std::filesystem::path> resolveFixture() const;
  void notify(CaptureState state, float progress);

  mutable std::mutex m_mutex;
  MockBackendConfig m_config;
  CaptureState m_state = CaptureState::Idle;
  CaptureOptions m_options;
  std::chrono::steady_clock::time_point m_startTime;
  std::function<void(CaptureState, float)> m_callback;
};

}  // namespace tachyon::capture
