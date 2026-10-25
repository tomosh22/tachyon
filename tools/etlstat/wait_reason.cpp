#include "wait_reason.h"

#include <array>
#include <cstddef>

namespace tachyon::etlstat {

namespace {
constexpr std::array<std::string_view, 39> kNames = {
    "Executive",        "FreePage",          "PageIn",           "PoolAllocation",
    "DelayExecution",   "Suspended",         "UserRequest",      "WrExecutive",
    "WrFreePage",       "WrPageIn",          "WrPoolAllocation", "WrDelayExecution",
    "WrSuspended",      "WrUserRequest",     "WrSpare0",         "WrQueue",
    "WrLpcReceive",     "WrLpcReply",        "WrVirtualMemory",  "WrPageOut",
    "WrRendezvous",     "WrKeyedEvent",      "WrTerminated",     "WrProcessInSwap",
    "WrCpuRateControl", "WrCalloutStack",    "WrKernel",         "WrResource",
    "WrPushLock",       "WrMutex",           "WrQuantumEnd",     "WrDispatchInt",
    "WrPreempted",      "WrYieldExecution",  "WrFastMutex",      "WrGuardedMutex",
    "WrRundown",        "WrAlertByThreadId", "WrDeferredPreempt"};
}  // namespace

std::string_view waitReasonName(int8_t reason) {
  if (reason < 0 || static_cast<size_t>(reason) >= kNames.size()) return "Unknown";
  return kNames[static_cast<size_t>(reason)];
}

WaitCategory waitCategory(int8_t reason) {
  switch (reason) {
    case 1:
    case 2:
    case 3:
    case 8:
    case 9:
    case 10:
    case 18:
    case 19:
    case 23: return WaitCategory::Memory;
    case 16:
    case 17: return WaitCategory::Io;
    case 30:
    case 31:
    case 32:
    case 38: return WaitCategory::Preempted;
    case 0:
    case 6:
    case 7:
    case 13:
    case 14:
    case 15:
    case 20:
    case 21:
    case 26:
    case 27:
    case 28:
    case 29:
    case 34:
    case 35:
    case 36:
    case 37: return WaitCategory::Synchronization;
    default: return WaitCategory::Other;
  }
}

std::string_view waitCategoryName(WaitCategory category) {
  switch (category) {
    case WaitCategory::Synchronization: return "Synchronization";
    case WaitCategory::Io: return "I/O";
    case WaitCategory::Memory: return "Memory";
    case WaitCategory::Preempted: return "Preempted";
    case WaitCategory::Other: return "Other";
  }
  return "Other";
}

}  // namespace tachyon::etlstat
