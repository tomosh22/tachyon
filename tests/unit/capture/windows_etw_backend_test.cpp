// Backend behaviour that does not need a live ETW session (those tests are in tests/windows/capture).
#include "capture/windows_etw/windows_etw_backend.h"

#include "core/platform.h"

#include <gtest/gtest.h>

namespace tachyon::capture {
namespace {

CaptureOptions systemOptions() {
  CaptureOptions o;
  o.mode = CaptureMode::System;
  o.outPath = "etw_backend_unit.tachyon";
  return o;
}

TEST(WindowsEtwBackendUnit, StartsIdle) {
  WindowsEtwBackend b;
  EXPECT_EQ(b.state(), CaptureState::Idle);
  EXPECT_EQ(b.diagnostics().flushCount, 0u);
  EXPECT_LT(b.diagnostics().lastFlushGapSeconds, 0.0);
}

TEST(WindowsEtwBackendUnit, LaunchNeedsAnExeAndWindows) {
  WindowsEtwBackend b;
  CaptureOptions o = systemOptions();
  o.mode = CaptureMode::Launch;
  auto r = b.start(o);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);  // no exe
  o.exe = "game.exe";
  r = b.start(o);
#if !TACHYON_PLATFORM_WINDOWS
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
#else
  (void)r;  // would really launch game.exe; the live behaviour is covered by tests/windows
#endif
  EXPECT_FALSE(b.launchPending());
  EXPECT_FALSE(b.targetExited());
  EXPECT_EQ(b.rootPid(), 0u);
}

TEST(WindowsEtwBackendUnit, AttachNeedsAPid) {
  WindowsEtwBackend b;
  CaptureOptions o = systemOptions();
  o.mode = CaptureMode::Attach;
  o.pid = 0;
  auto r = b.start(o);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

TEST(WindowsEtwBackendUnit, MissingOutPathIsRejected) {
  WindowsEtwBackend b;
  CaptureOptions o = systemOptions();
  o.outPath.clear();
  EXPECT_EQ(b.start(o).error().code, ErrorCode::InvalidArgument);
}

TEST(WindowsEtwBackendUnit, StopAndFlushWithoutACaptureAreNotFound) {
  WindowsEtwBackend b;
  auto s = b.stop();
  ASSERT_FALSE(s.ok());
  EXPECT_EQ(s.error().code, ErrorCode::NotFound);
  auto f = b.flush("hitch", 5);
  ASSERT_FALSE(f.ok());
  EXPECT_EQ(f.error().code, ErrorCode::NotFound);
  EXPECT_EQ(b.state(), CaptureState::Idle);
}

TEST(WindowsEtwBackendUnit, FlushRejectsBadNamesFirst) {
  WindowsEtwBackend b;
  for (const char* name : {"", "..", "a/b", "a\\b"}) {
    auto f = b.flush(name, 5);
    ASSERT_FALSE(f.ok());
    EXPECT_EQ(f.error().code, ErrorCode::InvalidArgument) << name;
  }
}

#if !TACHYON_PLATFORM_WINDOWS
TEST(WindowsEtwBackendUnit, EverythingIsUnsupportedOffWindows) {
  WindowsEtwBackend b;
  EXPECT_EQ(b.checkPrivileges().error().code, ErrorCode::Unsupported);
  auto r = b.start(systemOptions());
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);
  EXPECT_EQ(b.state(), CaptureState::Idle);

  auto orphans = etw::EtwSession::cleanupOrphans();
  ASSERT_TRUE(orphans.ok());
  EXPECT_TRUE(orphans->stopped.empty());
  auto sessions = etw::EtwSession::listSessions();
  ASSERT_TRUE(sessions.ok());
  EXPECT_TRUE(sessions->empty());
  EXPECT_FALSE(etw::EtwSession::start(etw::SessionConfig{}).ok());
}
#endif

}  // namespace
}  // namespace tachyon::capture
