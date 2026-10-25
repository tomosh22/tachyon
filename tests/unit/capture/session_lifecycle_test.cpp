// Start / flush / stop phase logic of the ETW backend.
#include "capture/windows_etw/session_lifecycle.h"

#include <gtest/gtest.h>

namespace tachyon::capture::etw {
namespace {

SessionLifecycle running(bool circular) {
  SessionLifecycle l;
  EXPECT_TRUE(l.beginStart().ok());
  l.startSucceeded(circular);
  EXPECT_EQ(l.phase(), SessionPhase::Running);
  return l;
}

TEST(SessionLifecycleTest, StartStopCycle) {
  SessionLifecycle l;
  EXPECT_EQ(l.phase(), SessionPhase::Idle);
  ASSERT_TRUE(l.beginStart().ok());
  EXPECT_EQ(l.phase(), SessionPhase::Starting);
  l.startSucceeded(false);
  EXPECT_EQ(l.phase(), SessionPhase::Running);
  EXPECT_EQ(l.segment(), 0u);
  auto kind = l.beginStop();
  ASSERT_TRUE(kind.ok());
  EXPECT_EQ(*kind, StopKind::LiveSession);
  EXPECT_EQ(l.phase(), SessionPhase::Stopping);
  l.stopFinished();
  EXPECT_EQ(l.phase(), SessionPhase::Idle);
  // And again.
  EXPECT_TRUE(l.beginStart().ok());
}

TEST(SessionLifecycleTest, FailedStartReturnsToIdle) {
  SessionLifecycle l;
  ASSERT_TRUE(l.beginStart().ok());
  l.startFailed();
  EXPECT_EQ(l.phase(), SessionPhase::Idle);
  EXPECT_TRUE(l.beginStart().ok());
}

TEST(SessionLifecycleTest, SecondStartIsBusy) {
  SessionLifecycle l;
  ASSERT_TRUE(l.beginStart().ok());
  auto again = l.beginStart();
  ASSERT_FALSE(again.ok());
  EXPECT_EQ(again.error().code, ErrorCode::Busy);
  l.startSucceeded(false);
  EXPECT_EQ(l.beginStart().error().code, ErrorCode::Busy);
}

TEST(SessionLifecycleTest, StopOrFlushWhileIdleIsNotFound) {
  SessionLifecycle l;
  EXPECT_EQ(l.beginStop().error().code, ErrorCode::NotFound);
  EXPECT_EQ(l.beginFlush().error().code, ErrorCode::NotFound);
}

TEST(SessionLifecycleTest, FlushNeedsCircularMode) {
  SessionLifecycle l = running(/*circular=*/false);
  auto f = l.beginFlush();
  ASSERT_FALSE(f.ok());
  EXPECT_EQ(f.error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(l.phase(), SessionPhase::Running);  // a rejected flush changes nothing
}

TEST(SessionLifecycleTest, FlushRestartAdvancesTheSegment) {
  SessionLifecycle l = running(/*circular=*/true);
  for (uint32_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(l.beginFlush().ok());
    EXPECT_EQ(l.phase(), SessionPhase::Flushing);
    EXPECT_EQ(l.segment(), i - 1);
    l.flushRestarted();
    EXPECT_EQ(l.phase(), SessionPhase::Running);
    EXPECT_EQ(l.segment(), i);
  }
  auto kind = l.beginStop();
  ASSERT_TRUE(kind.ok());
  EXPECT_EQ(*kind, StopKind::LiveSession);
}

TEST(SessionLifecycleTest, OperationsDuringAFlushAreBusy) {
  SessionLifecycle l = running(true);
  ASSERT_TRUE(l.beginFlush().ok());
  EXPECT_EQ(l.beginFlush().error().code, ErrorCode::Busy);
  EXPECT_EQ(l.beginStop().error().code, ErrorCode::Busy);
  EXPECT_EQ(l.beginStart().error().code, ErrorCode::Busy);
}

TEST(SessionLifecycleTest, OperationsWhileStoppingAreBusy) {
  SessionLifecycle l = running(true);
  ASSERT_TRUE(l.beginStop().ok());
  EXPECT_EQ(l.beginStop().error().code, ErrorCode::Busy);
  EXPECT_EQ(l.beginFlush().error().code, ErrorCode::Busy);
  EXPECT_EQ(l.beginStart().error().code, ErrorCode::Busy);
}

TEST(SessionLifecycleTest, FailedRestartFaultsTheCaptureAndStopCleansUp) {
  SessionLifecycle l = running(true);
  ASSERT_TRUE(l.beginFlush().ok());
  l.flushRestartFailed();
  EXPECT_EQ(l.phase(), SessionPhase::Faulted);
  EXPECT_EQ(l.beginFlush().error().code, ErrorCode::NotFound);  // no session to flush
  auto kind = l.beginStop();
  ASSERT_TRUE(kind.ok());
  EXPECT_EQ(*kind, StopKind::NoSession);
  l.stopFinished();
  EXPECT_EQ(l.phase(), SessionPhase::Idle);
  EXPECT_TRUE(l.beginStart().ok());
}

TEST(SessionLifecycleTest, StrayTransitionsAreIgnored) {
  SessionLifecycle l;
  l.startSucceeded(true);  // not starting
  l.flushRestarted();
  l.flushRestartFailed();
  l.stopFinished();
  l.startFailed();
  EXPECT_EQ(l.phase(), SessionPhase::Idle);
}

TEST(SessionLifecycleTest, PhaseNames) {
  EXPECT_STREQ(sessionPhaseName(SessionPhase::Flushing), "flushing");
  EXPECT_STREQ(sessionPhaseName(SessionPhase::Faulted), "faulted");
}

}  // namespace
}  // namespace tachyon::capture::etw
