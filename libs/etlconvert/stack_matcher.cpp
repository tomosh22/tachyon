#include "etlconvert/stack_matcher.h"

#include "core/hash.h"

#include <cstring>

namespace tachyon::etlconvert {

const char* parentKindName(ParentKind kind) {
  switch (kind) {
    case ParentKind::Sample: return "SampledProfile";
    case ParentKind::CSwitch: return "CSwitch";
    case ParentKind::Ready: return "ReadyThread";
    case ParentKind::kCount: break;
  }
  return "?";
}

const char* matchRoleName(MatchRole role) {
  switch (role) {
    case MatchRole::HeaderThread: return "header ThreadId";
    case MatchRole::PayloadThread:
      return "payload thread (SampledProfile ThreadId / CSwitch NewThreadId / ReadyThread TThreadId)";
    case MatchRole::PayloadOldThread: return "payload OldThreadId";
    case MatchRole::kCount: break;
  }
  return "?";
}

namespace {
constexpr size_t kInitialRing = 1024;
constexpr size_t kInitialTable = 4096;

inline size_t hashKey(uint64_t ts, uint32_t thread) {
  return static_cast<size_t>(core::hashU64(ts * 0x9E3779B97F4A7C15ull + thread));
}
}  // namespace

StackMatcher::StackMatcher(uint64_t windowTicks, ParentListener& listener)
    : m_window(windowTicks),
      m_listener(listener),
      m_ring(kInitialRing),
      m_ringMask(kInitialRing - 1),
      m_table(kInitialTable),
      m_tableMask(kInitialTable - 1) {}

void StackMatcher::growRing() {
  std::vector<PendingParent> bigger(m_ring.size() * 2);
  const size_t newMask = bigger.size() - 1;
  for (uint64_t id = m_head; id < m_tail; ++id) bigger[id & newMask] = std::move(m_ring[id & m_ringMask]);
  m_ring = std::move(bigger);
  m_ringMask = newMask;
}

void StackMatcher::growTable() {
  std::vector<Slot> old = std::move(m_table);
  m_table.assign(old.size() * 2, Slot{});
  m_tableMask = m_table.size() - 1;
  for (const Slot& s : old) {
    if (s.id == 0) continue;
    size_t i = hashKey(s.ts, s.thread) & m_tableMask;
    while (m_table[i].id != 0) i = (i + 1) & m_tableMask;
    m_table[i] = s;
  }
}

void StackMatcher::insertKey(uint64_t ts, uint32_t thread, uint64_t id) {
  if ((m_tableUsed + 1) * 2 > m_table.size()) growTable();
  size_t i = hashKey(ts, thread) & m_tableMask;
  while (m_table[i].id != 0) i = (i + 1) & m_tableMask;
  m_table[i] = Slot{ts, id, thread};
  ++m_tableUsed;
}

void StackMatcher::eraseKey(uint64_t ts, uint32_t thread, uint64_t id) {
  size_t i = hashKey(ts, thread) & m_tableMask;
  while (m_table[i].id != 0 && m_table[i].id != id) i = (i + 1) & m_tableMask;
  if (m_table[i].id == 0) return;
  // Backward-shift deletion keeps probe sequences intact without tombstones.
  size_t j = i;
  for (;;) {
    j = (j + 1) & m_tableMask;
    if (m_table[j].id == 0) break;
    const size_t home = hashKey(m_table[j].ts, m_table[j].thread) & m_tableMask;
    const bool homeInRange = i <= j ? (i < home && home <= j) : (i < home || home <= j);
    if (homeInRange) continue;
    m_table[i] = m_table[j];
    i = j;
  }
  m_table[i].id = 0;
  --m_tableUsed;
}

void StackMatcher::removeKeys(const PendingParent& p, uint64_t id) {
  for (uint8_t i = 0; i < p.candidateCount; ++i) eraseKey(p.ts, p.candidates[i].thread, id);
}

PendingParent& StackMatcher::add(ParentKind kind, uint64_t ts, const PendingParent::Candidate* candidates,
                                 uint8_t count) {
  if (m_tail - m_head >= m_ring.size()) growRing();
  const uint64_t id = m_tail++;
  PendingParent& p = parent(id);
  p.kind = kind;
  p.state = PendingParent::State::Pending;
  p.ts = ts;
  p.cpu = 0;
  p.headerTid = 0;
  p.headerPid = 0;
  p.tidA = 0;
  p.tidB = 0;
  p.ip = 0;
  std::memset(p.bytes, 0, sizeof(p.bytes));
  p.stackProcess = 0;
  p.stackThread = 0;
  p.role = MatchRole::HeaderThread;
  p.twoPart = false;
  p.frames.clear();
  p.candidateCount = 0;
  for (uint8_t i = 0; i < count && p.candidateCount < 3; ++i) {
    bool duplicate = false;
    for (uint8_t j = 0; j < p.candidateCount; ++j)
      duplicate |= p.candidates[j].thread == candidates[i].thread;
    if (duplicate) continue;
    p.candidates[p.candidateCount++] = candidates[i];
    insertKey(ts, candidates[i].thread, id);
  }
  return p;
}

StackMatchResult StackMatcher::onStack(const StackWalkPayload& stack) {
  StackMatchResult result;
  size_t i = hashKey(stack.eventTimeStamp, stack.stackThread) & m_tableMask;
  uint64_t id = 0;
  for (; m_table[i].id != 0; i = (i + 1) & m_tableMask) {
    const Slot& s = m_table[i];
    if (s.ts == stack.eventTimeStamp && s.thread == stack.stackThread) {
      id = s.id;
      break;
    }
  }
  if (id == 0) return result;

  PendingParent& p = parent(id);
  result.parent = p.kind;
  for (uint8_t c = 0; c < p.candidateCount; ++c) {
    if (p.candidates[c].thread == stack.stackThread) {
      result.role = p.candidates[c].role;
      break;
    }
  }

  const size_t n = stack.depth;
  if (p.state == PendingParent::State::Pending) {
    p.frames.resize(n);
    if (n > 0) std::memcpy(p.frames.data(), stack.frames.data(), n * sizeof(uint64_t));
    p.stackProcess = stack.stackProcess;
    p.stackThread = stack.stackThread;
    p.role = result.role;
    result.kind = StackMatchResult::Kind::Matched;
    const bool kernelOnly = n > 0 && p.frames[n - 1] >= kKernelAddressBase;
    if (kernelOnly) {
      p.state = PendingParent::State::KernelOnly;  // a user-mode StackWalk may follow
    } else {
      p.state = PendingParent::State::Complete;
      removeKeys(p, id);
    }
    return result;
  }

  // Second StackWalk for a parent whose first stack held kernel frames only: kernel frames of both, then the
  // user frames of the second.
  result.kind = StackMatchResult::Kind::MatchedSecondPart;
  size_t kernelCount = 0;
  while (kernelCount < n && stackFrame(stack, static_cast<uint32_t>(kernelCount)) >= kKernelAddressBase)
    ++kernelCount;
  const size_t at = p.frames.size();
  p.frames.resize(at + n);
  for (size_t k = 0; k < n; ++k) p.frames[at + k] = stackFrame(stack, static_cast<uint32_t>(k));
  p.twoPart = true;
  if (kernelCount < n) {
    p.state = PendingParent::State::Complete;
    removeKeys(p, id);
  }
  return result;
}

void StackMatcher::popFront() {
  PendingParent& p = parent(m_head);
  m_listener.onParentDone(p);
  p.frames.clear();
  ++m_head;
}

void StackMatcher::advance(uint64_t now) {
  while (m_head < m_tail) {
    PendingParent& p = parent(m_head);
    if (p.state != PendingParent::State::Complete) {
      if (p.ts + m_window >= now) break;
      removeKeys(p, m_head);
    }
    popFront();
  }
}

void StackMatcher::flush() {
  while (m_head < m_tail) {
    PendingParent& p = parent(m_head);
    if (p.state != PendingParent::State::Complete) removeKeys(p, m_head);
    popFront();
  }
}

}  // namespace tachyon::etlconvert
