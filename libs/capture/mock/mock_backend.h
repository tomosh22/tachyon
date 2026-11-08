// MockBackend: a capture backend that "captures" by copying a fixture trace to the requested output path.
#pragma once

#include "capture/capture_backend.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace tachyon::capture {

// The mock's live source (WP-65a). With CaptureOptions::live the mock replays the events of a trace file to
// the ILiveSink as if they were being captured: first the session facts and the tables (processes, threads,
// modules), then every sample / context switch / ready / scope / frame mark / marker / plot in timestamp
// order as the replay clock advances, each stretch of the clock ending with onTimeAdvance. The events are
// those of the capture's fixture, so a live model of the whole replay equals the final trace (the mock copies
// the same fixture to outPath).
struct MockLiveConfig {
  // The trace to replay; empty: the fixture of the capture.
  std::filesystem::path source;
  // 0: manual: nothing is emitted until the test calls MockBackend::pumpLive(). > 0: a thread replays in real
  // time, the trace clock advancing `speed` times as fast as the wall clock (1.0 = real time), and goes on
  // sending heartbeats once the trace is exhausted.
  double speed = 0.0;
  // Period of the replay thread.
  std::chrono::milliseconds tick{20};
};

struct MockBackendConfig {
  // Source trace to copy. If empty, the TACHYON_MOCK_FIXTURE environment variable is read at start().
  std::filesystem::path fixture;
  // Artificial conversion delay spent in stop(), split into progressSteps progress callbacks.
  std::chrono::milliseconds captureDuration{0};
  uint32_t progressSteps = 4;
  MockLiveConfig live = {};
};

// Thread-safe: all members may be called from any thread. The progress callback is invoked from the thread
// calling stop(), without internal locks held.
class MockBackend final : public ICaptureBackend {
 public:
  explicit MockBackend(MockBackendConfig config = {});
  ~MockBackend() override;

  Result<void> checkPrivileges() override;
  Result<void> start(const CaptureOptions& options) override;
  Result<CaptureResult> stop() override;
  // Works only while capturing with circularMb > 0; writes <outPath dir>/<name>.tachyon.
  Result<CaptureResult> flush(std::string name, uint32_t seconds) override;
  CaptureState state() const override;
  void setProgressCallback(std::function<void(CaptureState, float)> callback) override;
  // Live source (see MockLiveConfig). setLiveSink before start().
  void setLiveSink(std::shared_ptr<ILiveSink> sink) override;
  bool supportsLive() const override { return true; }

  // Thread-safe. Emits every not yet emitted event of the replay with ts <= untilTs (trace clock), then
  // onTimeAdvance(untilTs); returns how many events were emitted. Does nothing outside a live capture.
  uint64_t pumpLive(uint64_t untilTs);
  // Thread-safe. The first / last timestamp of the replayed trace (header range); 0 outside a live capture.
  uint64_t liveStartTs() const;
  uint64_t liveEndTs() const;

 private:
  Result<std::filesystem::path> resolveFixture() const;
  void notify(CaptureState state, float progress);
  Result<void> startLive(const CaptureOptions& options);
  void stopLive();

  mutable std::mutex m_mutex;
  MockBackendConfig m_config;
  CaptureState m_state = CaptureState::Idle;
  CaptureOptions m_options;
  std::chrono::steady_clock::time_point m_startTime;
  std::function<void(CaptureState, float)> m_callback;

  struct LiveReplay;
  std::shared_ptr<ILiveSink> m_sink;
  std::unique_ptr<LiveReplay> m_replay;
};

}  // namespace tachyon::capture
