// Wait-reason name tables and UI categories (WP-31, Part 3.8 rule 4).
//
// ThreadStateInterval::wait_reason is a copy of ContextSwitchRec::old_wait_reason and is interpreted
// according to FileHeader::platform:
//
//   Windows (platform 1, also used when the platform field is 0/unknown): wait_reason is a KWAIT_REASON
//   0..37.
//
//   Linux (platform 2): wait_reason is the sched_switch prev_state & 0xFF. Bits: 0x01 S (TASK_INTERRUPTIBLE),
//   0x02 D (TASK_UNINTERRUPTIBLE), 0x04 T (stopped), 0x08 t (traced), 0x10 EXIT_DEAD, 0x20 EXIT_ZOMBIE,
//   0x40 TASK_PARKED, 0x80 TASK_DEAD; 0 is TASK_RUNNING (a preempted thread).
//
// ContextSwitchRec::old_state uses the KTHREAD_STATE numbering on BOTH platforms (this is what tracegen
// writes for its Linux scenarios, and what WP-71 must write): Initialized 0, Ready 1, Running 2, Standby 3,
// Terminated 4, Waiting 5, Transition 6, DeferredReady 7. States 1, 3 and 7 mean the thread left the CPU
// still runnable (preempted), so the off-CPU interval is Ready instead of Waiting; state 4 means the thread
// exited at the switch-out; anything else is a wait. WP-71 encoding for a sched_switch record: old_state = 1
// when prev_state is TASK_RUNNING (preempted) or 4 when the task is exiting (EXIT_DEAD / EXIT_ZOMBIE /
// TASK_DEAD), else 5; old_wait_reason = prev_state & 0xFF (the TASK_REPORT_MAX "preempted" flag is folded
// into old_state, not stored).
#pragma once

#include <cstdint>
#include <string_view>

namespace tachyon::analysis {

// UI category of a wait; feeds the WP-42 band colours (red sync, dark blue I/O, light blue memory, grey
// other; Preempted is shown as the amber Ready band).
enum class WaitCategory : uint8_t { Synchronization, IO, Memory, Preempted, Other };

inline constexpr uint8_t kWindowsWaitReasonCount = 38;

// Thread-safe: pure function. Name of a wait reason for the given FileHeader::platform ("Unknown" if
// unknown).
std::string_view waitReasonName(uint32_t platform, uint8_t reason);

// Thread-safe: pure function. Windows mapping: FreePage/PageIn/PoolAllocation/WrFreePage/WrPageIn/
// WrPoolAllocation/WrVirtualMemory/WrPageOut/WrProcessInSwap -> Memory; WrLpcReceive/WrLpcReply -> IO;
// WrQuantumEnd/WrDispatchInt/WrPreempted/WrYieldExecution -> Preempted; Executive/WrExecutive/UserRequest/
// WrUserRequest/WrQueue/WrEventPair/WrRendezvous/WrKeyedEvent/WrResource/WrPushLock/WrMutex/WrFastMutex/
// WrGuardedMutex/WrRundown/WrAlertByThreadId -> Synchronization; everything else -> Other.
// Linux: D -> IO, S -> Synchronization, preempted (0) -> Preempted, others -> Other.
WaitCategory waitReasonCategory(uint32_t platform, uint8_t reason);

// Thread-safe: pure function.
std::string_view waitCategoryName(WaitCategory c);

// Thread-safe: pure function. True when ContextSwitchRec::old_state (KTHREAD_STATE 1, 3, 7) says the thread
// left the CPU still runnable. The same on every platform.
bool oldStateIsReady(uint8_t oldState);

// Thread-safe: pure function. True when the switch-out is the thread's exit (old_state Terminated, 4).
bool oldStateIsExit(uint8_t oldState);

}  // namespace tachyon::analysis
