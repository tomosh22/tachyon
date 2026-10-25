#include "capture/mock/mock_backend.h"

#include <cstdlib>
#include <system_error>
#include <thread>

namespace tachyon::capture {

namespace fs = std::filesystem;

MockBackend::MockBackend(MockBackendConfig config) : m_config(std::move(config)) {}

Result<void> MockBackend::checkPrivileges() {
  return ok();
}

Result<fs::path> MockBackend::resolveFixture() const {
  fs::path fixture = m_config.fixture;
  if (fixture.empty()) {
    const char* env = std::getenv("TACHYON_MOCK_FIXTURE");
    if (env != nullptr) fixture = env;
  }
  if (fixture.empty())
    return makeError(ErrorCode::InvalidArgument, "mock backend has no fixture (set TACHYON_MOCK_FIXTURE)");
  std::error_code ec;
  if (!fs::is_regular_file(fixture, ec))
    return makeError(ErrorCode::NotFound, "mock fixture not found: " + fixture.string());
  return fixture;
}

Result<void> MockBackend::start(const CaptureOptions& options) {
  std::lock_guard lock(m_mutex);
  if (m_state == CaptureState::Starting || m_state == CaptureState::Capturing ||
      m_state == CaptureState::Converting)
    return makeError(ErrorCode::Busy, "mock capture already running");
  if (options.outPath.empty()) return makeError(ErrorCode::InvalidArgument, "outPath is required");
  auto fixture = resolveFixture();
  if (!fixture.ok()) return fixture.error();
  m_options = options;
  m_startTime = std::chrono::steady_clock::now();
  m_state = CaptureState::Capturing;
  return ok();
}

void MockBackend::notify(CaptureState state, float progress) {
  std::function<void(CaptureState, float)> cb;
  {
    std::lock_guard lock(m_mutex);
    cb = m_callback;
  }
  if (cb) cb(state, progress);
}

Result<CaptureResult> MockBackend::stop() {
  CaptureOptions options;
  double durationS = 0.0;
  {
    std::lock_guard lock(m_mutex);
    if (m_state != CaptureState::Capturing)
      return makeError(ErrorCode::InvalidArgument, "mock backend is not capturing");
    m_state = CaptureState::Converting;
    options = m_options;
    durationS = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();
  }
  const uint32_t steps = m_config.progressSteps == 0 ? 1 : m_config.progressSteps;
  notify(CaptureState::Converting, 0.0f);
  for (uint32_t i = 1; i <= steps; ++i) {
    if (m_config.captureDuration.count() > 0) std::this_thread::sleep_for(m_config.captureDuration / steps);
    if (i < steps) notify(CaptureState::Converting, static_cast<float>(i) / static_cast<float>(steps));
  }

  Result<CaptureResult> result = [&]() -> Result<CaptureResult> {
    auto fixture = resolveFixture();
    if (!fixture.ok()) return fixture.error();
    std::error_code ec;
    if (options.outPath.has_parent_path()) fs::create_directories(options.outPath.parent_path(), ec);
    fs::copy_file(*fixture, options.outPath, fs::copy_options::overwrite_existing, ec);
    if (ec) return makeError(ErrorCode::IoError, "copy to " + options.outPath.string() + ": " + ec.message());
    CaptureResult r;
    r.tracePath = options.outPath;
    r.durationS = durationS;
    return r;
  }();

  {
    std::lock_guard lock(m_mutex);
    m_state = result.ok() ? CaptureState::Done : CaptureState::Idle;
  }
  if (result.ok()) notify(CaptureState::Done, 1.0f);
  return result;
}

Result<CaptureResult> MockBackend::flush(std::string name, uint32_t seconds) {
  (void)seconds;
  CaptureOptions options;
  double durationS = 0.0;
  {
    std::lock_guard lock(m_mutex);
    if (m_state != CaptureState::Capturing)
      return makeError(ErrorCode::InvalidArgument, "mock backend is not capturing");
    if (m_options.circularMb == 0)
      return makeError(ErrorCode::InvalidArgument, "flush requires circular mode (circularMb > 0)");
    options = m_options;
    durationS = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();
  }
  if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
    return makeError(ErrorCode::InvalidArgument, "invalid flush name");
  auto fixture = resolveFixture();
  if (!fixture.ok()) return fixture.error();
  const fs::path out = options.outPath.parent_path() / (name + ".tachyon");
  std::error_code ec;
  fs::copy_file(*fixture, out, fs::copy_options::overwrite_existing, ec);
  if (ec) return makeError(ErrorCode::IoError, "copy to " + out.string() + ": " + ec.message());
  CaptureResult r;
  r.tracePath = out;
  r.durationS = durationS;
  return r;
}

CaptureState MockBackend::state() const {
  std::lock_guard lock(m_mutex);
  return m_state;
}

void MockBackend::setProgressCallback(std::function<void(CaptureState, float)> callback) {
  std::lock_guard lock(m_mutex);
  m_callback = std::move(callback);
}

}  // namespace tachyon::capture
