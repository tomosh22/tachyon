#include "capture/mock/mock_backend.h"
#include "../control/test_util.h"
#include "trace/trace_reader.h"
#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

namespace tachyon::capture {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;

std::string readFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

fs::path writeFixture(const fs::path& path) {
  trace::FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.start_ts = 10;
  h.end_ts = 20;
  h.platform = trace::kPlatformLinux;
  auto w = trace::TraceWriter::open(path, h);
  EXPECT_TRUE(w.ok());
  w->internString("hello");
  EXPECT_TRUE(w->finish().ok());
  return path;
}

CaptureOptions options(const fs::path& out, uint32_t circularMb = 0) {
  CaptureOptions o;
  o.outPath = out;
  o.circularMb = circularMb;
  return o;
}

TEST(MockBackend, StartStopProducesFixtureCopy) {
  TempDir dir;
  const fs::path fixture = writeFixture(dir.path() / "fixture.tachyon");
  MockBackend b({fixture});
  EXPECT_TRUE(b.checkPrivileges().ok());
  EXPECT_EQ(b.state(), CaptureState::Idle);
  ASSERT_TRUE(b.start(options(dir.path() / "sub" / "out.tachyon")).ok());
  EXPECT_EQ(b.state(), CaptureState::Capturing);
  auto r = b.stop();
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->tracePath, dir.path() / "sub" / "out.tachyon");
  EXPECT_EQ(r->eventsLost, 0u);
  EXPECT_EQ(b.state(), CaptureState::Done);
  EXPECT_EQ(readFile(r->tracePath), readFile(fixture));
  EXPECT_TRUE(trace::TraceReader::open(r->tracePath).ok());
}

TEST(MockBackend, FixtureFromEnvironment) {
  TempDir dir;
  const fs::path fixture = writeFixture(dir.path() / "env.tachyon");
#if TACHYON_PLATFORM_WINDOWS
  _putenv_s("TACHYON_MOCK_FIXTURE", fixture.string().c_str());
#else
  setenv("TACHYON_MOCK_FIXTURE", fixture.c_str(), 1);
#endif
  MockBackend b;
  ASSERT_TRUE(b.start(options(dir.path() / "o.tachyon")).ok());
  ASSERT_TRUE(b.stop().ok());
  EXPECT_EQ(readFile(dir.path() / "o.tachyon"), readFile(fixture));
#if TACHYON_PLATFORM_WINDOWS
  _putenv_s("TACHYON_MOCK_FIXTURE", "");
#else
  unsetenv("TACHYON_MOCK_FIXTURE");
#endif
}

TEST(MockBackend, MissingFixtureIsAnError) {
  TempDir dir;
  MockBackend b({dir.path() / "nope.tachyon"});
  auto s = b.start(options(dir.path() / "o.tachyon"));
  ASSERT_FALSE(s.ok());
  EXPECT_EQ(s.error().code, ErrorCode::NotFound);
  EXPECT_EQ(b.state(), CaptureState::Idle);
}

TEST(MockBackend, StartTwiceIsBusyAndStopWithoutStartFails) {
  TempDir dir;
  MockBackend b({writeFixture(dir.path() / "f.tachyon")});
  EXPECT_FALSE(b.stop().ok());
  ASSERT_TRUE(b.start(options(dir.path() / "o.tachyon")).ok());
  auto again = b.start(options(dir.path() / "o2.tachyon"));
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().code, ErrorCode::Busy);
  ASSERT_TRUE(b.stop().ok());
  // After done a new capture may start.
  EXPECT_TRUE(b.start(options(dir.path() / "o3.tachyon")).ok());
}

TEST(MockBackend, ProgressCallbackReportsSteps) {
  TempDir dir;
  MockBackendConfig cfg;
  cfg.fixture = writeFixture(dir.path() / "f.tachyon");
  cfg.captureDuration = std::chrono::milliseconds(20);
  cfg.progressSteps = 5;
  MockBackend b(cfg);
  std::mutex m;
  std::vector<std::pair<CaptureState, float>> seen;
  b.setProgressCallback([&](CaptureState s, float p) {
    std::lock_guard lock(m);
    seen.emplace_back(s, p);
  });
  ASSERT_TRUE(b.start(options(dir.path() / "o.tachyon")).ok());
  ASSERT_TRUE(b.stop().ok());
  ASSERT_GE(seen.size(), 6u);
  EXPECT_EQ(seen.front().first, CaptureState::Converting);
  EXPECT_FLOAT_EQ(seen.front().second, 0.0f);
  EXPECT_EQ(seen.back().first, CaptureState::Done);
  EXPECT_FLOAT_EQ(seen.back().second, 1.0f);
  for (size_t i = 1; i < seen.size(); ++i) EXPECT_GE(seen[i].second, seen[i - 1].second);
}

TEST(MockBackend, FlushOnlyInCircularMode) {
  TempDir dir;
  MockBackend b({writeFixture(dir.path() / "f.tachyon")});
  EXPECT_FALSE(b.flush("x", 5).ok());  // not capturing

  ASSERT_TRUE(b.start(options(dir.path() / "o.tachyon", 0)).ok());
  EXPECT_FALSE(b.flush("x", 5).ok());  // not circular
  ASSERT_TRUE(b.stop().ok());

  ASSERT_TRUE(b.start(options(dir.path() / "o.tachyon", 64)).ok());
  auto r = b.flush("hitch-1", 10);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->tracePath, dir.path() / "hitch-1.tachyon");
  EXPECT_EQ(readFile(r->tracePath), readFile(dir.path() / "f.tachyon"));
  EXPECT_EQ(b.state(), CaptureState::Capturing);  // flush does not end the capture
  EXPECT_FALSE(b.flush("../evil", 1).ok());
  EXPECT_FALSE(b.flush("", 1).ok());
  ASSERT_TRUE(b.stop().ok());
}

TEST(CaptureNames, StateAndModeNamesRoundTrip) {
  for (auto s : {CaptureState::Idle, CaptureState::Starting, CaptureState::Capturing,
                 CaptureState::Converting, CaptureState::Done})
    EXPECT_EQ(parseCaptureState(captureStateName(s)), s);
  for (auto m : {CaptureMode::Launch, CaptureMode::Attach, CaptureMode::System})
    EXPECT_EQ(parseCaptureMode(captureModeName(m)), m);
  EXPECT_FALSE(parseCaptureState("bogus").has_value());
}

}  // namespace
}  // namespace tachyon::capture
