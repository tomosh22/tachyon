// Attaches StackWalk events to the parent events (SampledProfile, CSwitch, ReadyThread) they belong to.
//
// Parents are queued in arrival order and indexed by (timestamp, thread) under every candidate thread id of
// the parent (which id the kernel uses for StackThread is an open [VERIFY] item, see docs/etw-notes.md).
// A StackWalk looks its parent up by (EventTimeStamp, StackThread). A parent completes when a stack arrives
// that contains user-mode frames (or is empty of kernel-only content); a stack of kernel frames only keeps
// the parent open for a following user-mode StackWalk, and the two are concatenated kernel first, then user,
// leaf to root. Parents leave the queue strictly in arrival order, so output stays timestamp-ordered; a
// parent that has not completed when a newer event is more than `windowTicks` ahead is released as it is (a
// stack that never arrived leaves it without frames).
#pragma once

#include "etlconvert/etl_events.h"

#include <cstdint>
#include <vector>

namespace tachyon::etlconvert {

enum class ParentKind : uint8_t { Sample = 0, CSwitch = 1, Ready = 2, kCount = 3 };
// Which identifier of the parent event the StackWalk's StackThread matched.
enum class MatchRole : uint8_t { HeaderThread = 0, PayloadThread = 1, PayloadOldThread = 2, kCount = 3 };

// Thread-safe: pure function.
const char* parentKindName(ParentKind kind);
// Thread-safe: pure function.
const char* matchRoleName(MatchRole role);

struct PendingParent {
  enum class State : uint8_t { Pending, KernelOnly, Complete };

  ParentKind kind = ParentKind::Sample;
  State state = State::Pending;
  uint64_t ts = 0;
  uint16_t cpu = 0;
  uint32_t headerTid = 0;
  uint32_t headerPid = 0;
  // Sample: tidA = sampled thread. CSwitch: tidA = new thread, tidB = old thread. Ready: tidA = readied
  // thread, tidB = readying thread.
  uint32_t tidA = 0;
  uint32_t tidB = 0;
  uint64_t ip = 0;  // Sample: instruction pointer
  uint8_t bytes[4] =
      {};  // CSwitch: oldState, oldWaitReason, oldWaitMode, newPriority. Ready: adjustReason, flag
  // Filled by the matcher once a stack is attached.
  uint32_t stackProcess = 0;
  uint32_t stackThread = 0;
  MatchRole role = MatchRole::HeaderThread;
  bool twoPart = false;
  std::vector<uint64_t> frames;  // leaf first; capacity is recycled

  // The candidate threads this parent can be matched under.
  struct Candidate {
    uint32_t thread = 0;
    MatchRole role = MatchRole::HeaderThread;
  };
  Candidate candidates[3];
  uint8_t candidateCount = 0;

  // Thread-safe for const access.
  bool hasStack() const { return state != State::Pending; }
};

// Receives parents in arrival order once they are complete or released.
class ParentListener {
 public:
  virtual ~ParentListener() = default;
  // Not thread-safe. `parent` is only valid for the call.
  virtual void onParentDone(PendingParent& parent) = 0;
};

struct StackMatchResult {
  enum class Kind : uint8_t { Unmatched, Matched, MatchedSecondPart } kind = Kind::Unmatched;
  ParentKind parent = ParentKind::Sample;
  MatchRole role = MatchRole::HeaderThread;
};

class StackMatcher {
 public:
  // Not thread-safe.
  StackMatcher(uint64_t windowTicks, ParentListener& listener);

  // Not thread-safe. Queues a parent and returns it so the caller can fill in its event data. The reference
  // is valid until the next add(). `kind`, `ts` and the candidates must be set through this call.
  PendingParent& add(ParentKind kind, uint64_t ts, const PendingParent::Candidate* candidates, uint8_t count);

  // Not thread-safe. Attaches a decoded StackWalk to its parent.
  StackMatchResult onStack(const StackWalkPayload& stack);

  // Not thread-safe. Releases every parent that is complete at the front of the queue, and every parent more
  // than the window behind `now`.
  void advance(uint64_t now);

  // Not thread-safe. Releases everything that is still queued (complete or not).
  void flush();

  // Thread-safe for const access.
  size_t pending() const { return static_cast<size_t>(m_tail - m_head); }

 private:
  struct Slot {
    uint64_t ts = 0;
    uint64_t id = 0;  // 0 = empty
    uint32_t thread = 0;
  };

  PendingParent& parent(uint64_t id) { return m_ring[id & m_ringMask]; }
  void growRing();
  void insertKey(uint64_t ts, uint32_t thread, uint64_t id);
  void eraseKey(uint64_t ts, uint32_t thread, uint64_t id);
  void growTable();
  void removeKeys(const PendingParent& p, uint64_t id);
  void popFront();

  uint64_t m_window;
  ParentListener& m_listener;
  std::vector<PendingParent> m_ring;
  size_t m_ringMask = 0;
  uint64_t m_head = 1;  // id of the oldest queued parent
  uint64_t m_tail = 1;  // id the next parent will get
  std::vector<Slot> m_table;
  size_t m_tableMask = 0;
  size_t m_tableUsed = 0;
};

}  // namespace tachyon::etlconvert
