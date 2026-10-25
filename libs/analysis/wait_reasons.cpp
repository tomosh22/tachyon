#include "analysis/wait_reasons.h"

#include "trace/trace_format.h"

#include <array>

namespace tachyon::analysis {
namespace {

constexpr std::array<std::string_view, kWindowsWaitReasonCount> kWindowsNames = {
    "Executive",        "FreePage",         "PageIn",           "PoolAllocation",   "DelayExecution",
    "Suspended",        "UserRequest",      "WrExecutive",      "WrFreePage",       "WrPageIn",
    "WrPoolAllocation", "WrDelayExecution", "WrSuspended",      "WrUserRequest",    "WrEventPair",
    "WrQueue",          "WrLpcReceive",     "WrLpcReply",       "WrVirtualMemory",  "WrPageOut",
    "WrRendezvous",     "WrKeyedEvent",     "WrTerminated",     "WrProcessInSwap",  "WrCpuRateControl",
    "WrCalloutStack",   "WrKernel",         "WrResource",       "WrPushLock",       "WrMutex",
    "WrQuantumEnd",     "WrDispatchInt",    "WrPreempted",      "WrYieldExecution", "WrFastMutex",
    "WrGuardedMutex",   "WrRundown",        "WrAlertByThreadId"};

constexpr uint8_t kLinuxInterruptible = 0x01;
constexpr uint8_t kLinuxUninterruptible = 0x02;
constexpr uint8_t kLinuxStopped = 0x04;
constexpr uint8_t kLinuxTraced = 0x08;
constexpr uint8_t kLinuxExitDead = 0x10;
constexpr uint8_t kLinuxExitZombie = 0x20;
constexpr uint8_t kLinuxParked = 0x40;
constexpr uint8_t kLinuxDead = 0x80;
constexpr uint8_t kLinuxExitMask = kLinuxExitDead | kLinuxExitZombie | kLinuxDead;

bool isLinux(uint32_t platform) {
  return platform == trace::kPlatformLinux;
}

WaitCategory windowsCategory(uint8_t r) {
  switch (r) {
    case 1:   // FreePage
    case 2:   // PageIn
    case 3:   // PoolAllocation
    case 8:   // WrFreePage
    case 9:   // WrPageIn
    case 10:  // WrPoolAllocation
    case 18:  // WrVirtualMemory
    case 19:  // WrPageOut
    case 23:  // WrProcessInSwap
      return WaitCategory::Memory;
    case 16:  // WrLpcReceive
    case 17:  // WrLpcReply
      return WaitCategory::IO;
    case 30:  // WrQuantumEnd
    case 31:  // WrDispatchInt
    case 32:  // WrPreempted
    case 33:  // WrYieldExecution
      return WaitCategory::Preempted;
    case 0:   // Executive
    case 6:   // UserRequest
    case 7:   // WrExecutive
    case 13:  // WrUserRequest
    case 14:  // WrEventPair
    case 15:  // WrQueue
    case 20:  // WrRendezvous
    case 21:  // WrKeyedEvent
    case 27:  // WrResource
    case 28:  // WrPushLock
    case 29:  // WrMutex
    case 34:  // WrFastMutex
    case 35:  // WrGuardedMutex
    case 36:  // WrRundown
    case 37:  // WrAlertByThreadId
      return WaitCategory::Synchronization;
    default:  // DelayExecution, Suspended, WrDelayExecution, WrSuspended, WrTerminated, WrCpuRateControl,
              // WrCalloutStack, WrKernel and anything out of range
      return WaitCategory::Other;
  }
}

}  // namespace

std::string_view waitReasonName(uint32_t platform, uint8_t reason) {
  if (isLinux(platform)) {
    if (reason == 0) return "Preempted (R)";
    if (reason & kLinuxUninterruptible) return "Uninterruptible (D)";
    if (reason & kLinuxInterruptible) return "Interruptible (S)";
    if (reason & kLinuxStopped) return "Stopped (T)";
    if (reason & kLinuxTraced) return "Traced (t)";
    if (reason & kLinuxParked) return "Parked";
    if (reason & kLinuxExitMask) return "Exited";
    return "Unknown";
  }
  return reason < kWindowsWaitReasonCount ? kWindowsNames[reason] : std::string_view("Unknown");
}

WaitCategory waitReasonCategory(uint32_t platform, uint8_t reason) {
  if (isLinux(platform)) {
    if (reason == 0) return WaitCategory::Preempted;
    if (reason & kLinuxUninterruptible) return WaitCategory::IO;
    if (reason & kLinuxInterruptible) return WaitCategory::Synchronization;
    return WaitCategory::Other;
  }
  return windowsCategory(reason);
}

std::string_view waitCategoryName(WaitCategory c) {
  switch (c) {
    case WaitCategory::Synchronization: return "Synchronization";
    case WaitCategory::IO: return "I/O";
    case WaitCategory::Memory: return "Memory";
    case WaitCategory::Preempted: return "Preempted";
    case WaitCategory::Other: return "Other";
  }
  return "Other";
}

bool oldStateIsReady(uint8_t oldState) {
  return oldState == 1 || oldState == 3 || oldState == 7;  // Ready, Standby, DeferredReady
}

bool oldStateIsExit(uint8_t oldState) {
  return oldState == 4;  // Terminated
}

}  // namespace tachyon::analysis
