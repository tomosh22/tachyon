#include "analysis/wait_reasons.h"

#include "trace/trace_format.h"

#include <gtest/gtest.h>

namespace tachyon::analysis {
namespace {

constexpr uint32_t kWin = trace::kPlatformWindows;
constexpr uint32_t kLin = trace::kPlatformLinux;

TEST(WaitReasons, WindowsNames) {
  const char* const expected[] = {
      "Executive",        "FreePage",         "PageIn",           "PoolAllocation",   "DelayExecution",
      "Suspended",        "UserRequest",      "WrExecutive",      "WrFreePage",       "WrPageIn",
      "WrPoolAllocation", "WrDelayExecution", "WrSuspended",      "WrUserRequest",    "WrEventPair",
      "WrQueue",          "WrLpcReceive",     "WrLpcReply",       "WrVirtualMemory",  "WrPageOut",
      "WrRendezvous",     "WrKeyedEvent",     "WrTerminated",     "WrProcessInSwap",  "WrCpuRateControl",
      "WrCalloutStack",   "WrKernel",         "WrResource",       "WrPushLock",       "WrMutex",
      "WrQuantumEnd",     "WrDispatchInt",    "WrPreempted",      "WrYieldExecution", "WrFastMutex",
      "WrGuardedMutex",   "WrRundown",        "WrAlertByThreadId"};
  static_assert(sizeof(expected) / sizeof(expected[0]) == kWindowsWaitReasonCount);
  for (uint8_t i = 0; i < kWindowsWaitReasonCount; ++i)
    EXPECT_EQ(waitReasonName(kWin, i), expected[i]) << int(i);
  EXPECT_EQ(waitReasonName(kWin, 38), "Unknown");
  EXPECT_EQ(waitReasonName(kWin, 255), "Unknown");
  // An unset platform uses the Windows table.
  EXPECT_EQ(waitReasonName(0, 6), "UserRequest");
}

TEST(WaitReasons, WindowsCategories) {
  EXPECT_EQ(waitReasonCategory(kWin, 2), WaitCategory::Memory);            // PageIn
  EXPECT_EQ(waitReasonCategory(kWin, 9), WaitCategory::Memory);            // WrPageIn
  EXPECT_EQ(waitReasonCategory(kWin, 1), WaitCategory::Memory);            // FreePage
  EXPECT_EQ(waitReasonCategory(kWin, 18), WaitCategory::Memory);           // WrVirtualMemory
  EXPECT_EQ(waitReasonCategory(kWin, 19), WaitCategory::Memory);           // WrPageOut
  EXPECT_EQ(waitReasonCategory(kWin, 32), WaitCategory::Preempted);        // WrPreempted
  EXPECT_EQ(waitReasonCategory(kWin, 30), WaitCategory::Preempted);        // WrQuantumEnd
  EXPECT_EQ(waitReasonCategory(kWin, 16), WaitCategory::IO);               // WrLpcReceive
  EXPECT_EQ(waitReasonCategory(kWin, 17), WaitCategory::IO);               // WrLpcReply
  EXPECT_EQ(waitReasonCategory(kWin, 0), WaitCategory::Synchronization);   // Executive
  EXPECT_EQ(waitReasonCategory(kWin, 6), WaitCategory::Synchronization);   // UserRequest
  EXPECT_EQ(waitReasonCategory(kWin, 15), WaitCategory::Synchronization);  // WrQueue
  EXPECT_EQ(waitReasonCategory(kWin, 29), WaitCategory::Synchronization);  // WrMutex
  EXPECT_EQ(waitReasonCategory(kWin, 4), WaitCategory::Other);             // DelayExecution
  EXPECT_EQ(waitReasonCategory(kWin, 26), WaitCategory::Other);            // WrKernel
  EXPECT_EQ(waitReasonCategory(kWin, 200), WaitCategory::Other);
  for (uint8_t i = 0; i < kWindowsWaitReasonCount; ++i)
    EXPECT_FALSE(waitCategoryName(waitReasonCategory(kWin, i)).empty());
}

TEST(WaitReasons, LinuxPrevState) {
  EXPECT_EQ(waitReasonCategory(kLin, 0), WaitCategory::Preempted);
  EXPECT_EQ(waitReasonCategory(kLin, 1), WaitCategory::Synchronization);
  EXPECT_EQ(waitReasonCategory(kLin, 2), WaitCategory::IO);
  EXPECT_EQ(waitReasonCategory(kLin, 3), WaitCategory::IO);  // D wins when both bits are set
  EXPECT_EQ(waitReasonCategory(kLin, 4), WaitCategory::Other);
  EXPECT_EQ(waitReasonName(kLin, 0), "Preempted (R)");
  EXPECT_EQ(waitReasonName(kLin, 1), "Interruptible (S)");
  EXPECT_EQ(waitReasonName(kLin, 2), "Uninterruptible (D)");
  EXPECT_EQ(waitReasonName(kLin, 4), "Stopped (T)");
  EXPECT_EQ(waitReasonName(kLin, 0x20), "Exited");
}

TEST(WaitReasons, OldStateClassification) {
  for (uint8_t s : {1, 3, 7}) EXPECT_TRUE(oldStateIsReady(s)) << int(s);
  for (uint8_t s : {0, 2, 4, 5, 6}) EXPECT_FALSE(oldStateIsReady(s)) << int(s);
  EXPECT_TRUE(oldStateIsExit(4));
  EXPECT_FALSE(oldStateIsExit(5));
  EXPECT_FALSE(oldStateIsExit(1));
}

}  // namespace
}  // namespace tachyon::analysis
