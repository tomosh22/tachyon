#include "etw_stats.h"

#include <algorithm>

#include "etw_payload.h"

namespace tachyon::etlstat {

const char* parentKindName(ParentKind kind) {
  switch (kind) {
    case ParentKind::SampledProfile: return "SampledProfile";
    case ParentKind::CSwitch: return "CSwitch";
    case ParentKind::ReadyThread: return "ReadyThread";
    case ParentKind::kCount: break;
  }
  return "?";
}

const char* matchRoleName(MatchRole role) {
  switch (role) {
    case MatchRole::HeaderThread: return "header ThreadId";
    case MatchRole::PayloadThread: return "payload thread (NewThreadId / TThreadId / ThreadId)";
    case MatchRole::PayloadOldThread: return "payload OldThreadId";
    case MatchRole::kCount: break;
  }
  return "?";
}

StatsAccumulator::StatsAccumulator(StatsConfig config) : m_config(config) {}

void StatsAccumulator::addParent(ParentKind kind, uint64_t timestamp, bool idle, const uint32_t* threads,
                                 const MatchRole* roles, size_t count) {
  Parent parent;
  parent.timestamp = timestamp;
  parent.kind = kind;
  parent.idle = idle;
  const uint64_t id = m_baseId + m_parents.size();
  for (size_t i = 0; i < count; ++i) {
    bool duplicate = false;
    for (uint8_t j = 0; j < parent.keyCount; ++j) duplicate |= parent.keys[j].thread == threads[i];
    if (duplicate) continue;
    parent.keys[parent.keyCount] = Key{timestamp, threads[i]};
    parent.roles[parent.keyCount] = roles[i];
    m_pending.emplace(parent.keys[parent.keyCount], id);
    ++parent.keyCount;
  }
  m_parents.push_back(parent);
  ++m_snap.parents[static_cast<size_t>(kind)].total;
}

void StatsAccumulator::evict(uint64_t now) {
  while (!m_parents.empty() && m_parents.front().timestamp + m_config.pendingWindowTicks < now) {
    const uint64_t id = m_baseId;
    const Parent& parent = m_parents.front();
    for (uint8_t i = 0; i < parent.keyCount; ++i) {
      auto range = m_pending.equal_range(parent.keys[i]);
      for (auto it = range.first; it != range.second; ++it) {
        if (it->second == id) {
          m_pending.erase(it);
          break;
        }
      }
    }
    if (!parent.matched) ++m_snap.parents[static_cast<size_t>(parent.kind)].withoutStack;
    m_parents.pop_front();
    ++m_baseId;
  }
}

void StatsAccumulator::handleStack(const EtwEvent& event) {
  auto stack = decodeStackWalk(event.payload);
  if (!stack) {
    ++m_snap.malformedEvents;
    return;
  }
  ++m_snap.stackEvents;
  m_snap.maxStackDepth = std::max(m_snap.maxStackDepth, stack->depth);

  const Key key{stack->eventTimeStamp, stack->stackThread};
  auto range = m_pending.equal_range(key);
  for (auto it = range.first; it != range.second; ++it) {
    Parent& parent = m_parents[static_cast<size_t>(it->second - m_baseId)];
    if (parent.matched) continue;
    parent.matched = true;
    for (uint8_t i = 0; i < parent.keyCount; ++i) {
      if (parent.keys[i] == key) {
        ++m_snap.stackRoleCount[static_cast<size_t>(parent.roles[i])];
        break;
      }
    }
    ParentStats& ps = m_snap.parents[static_cast<size_t>(parent.kind)];
    ++ps.withStack;
    ps.maxDepth = std::max(ps.maxDepth, stack->depth);
    ps.depthSum += stack->depth;
    if (parent.kind == ParentKind::SampledProfile && !parent.idle) ++m_snap.sampledNonIdleWithStack;
    ++m_snap.stackMatched;
    return;
  }
  ++m_snap.stackUnmatched;
}

void StatsAccumulator::add(const EtwEvent& event) {
  using namespace opcodes;
  ++m_snap.totalEvents;
  KeyStats& ks = m_snap.keys[EventKey{event.provider, event.opcode, event.version}];
  const auto size = static_cast<uint32_t>(event.payload.size());
  ++ks.count;
  ks.totalBytes += size;
  ks.minSize = std::min(ks.minSize, size);
  ks.maxSize = std::max(ks.maxSize, size);

  if (m_snap.totalEvents == 1 || event.timestamp < m_snap.firstEventTs) m_snap.firstEventTs = event.timestamp;
  m_snap.lastEventTs = std::max(m_snap.lastEventTs, event.timestamp);

  if (event.provider == guids::kPerfInfo && event.opcode == kPerfInfoSampledProfile) {
    auto p = decodeSampledProfile(event.payload);
    if (!p) {
      ++m_snap.malformedEvents;
    } else {
      const bool idle = p->threadId == 0;
      ++m_snap.sampledProfile;
      if (!idle) {
        ++m_snap.sampledNonIdle;
        ++m_snap.samplesPerThread[p->threadId];
      }
      if (m_snap.sampledProfile == 1 || event.timestamp < m_snap.firstSampleTs)
        m_snap.firstSampleTs = event.timestamp;
      m_snap.lastSampleTs = std::max(m_snap.lastSampleTs, event.timestamp);
      if (m_snap.cpus.size() <= event.cpu) m_snap.cpus.resize(static_cast<size_t>(event.cpu) + 1);
      CpuStats& cpu = m_snap.cpus[event.cpu];
      ++cpu.samples;
      if (!idle) ++cpu.nonIdleSamples;
      const uint32_t threads[2] = {p->threadId, event.threadId};
      const MatchRole roles[2] = {MatchRole::PayloadThread, MatchRole::HeaderThread};
      addParent(ParentKind::SampledProfile, event.timestamp, idle, threads, roles, 2);
    }
  } else if (event.provider == guids::kThread && event.opcode == kThreadCSwitch) {
    auto p = decodeCSwitch(event.payload);
    if (!p) {
      ++m_snap.malformedEvents;
    } else {
      const uint32_t threads[3] = {event.threadId, p->newThreadId, p->oldThreadId};
      const MatchRole roles[3] = {MatchRole::HeaderThread, MatchRole::PayloadThread,
                                  MatchRole::PayloadOldThread};
      addParent(ParentKind::CSwitch, event.timestamp, false, threads, roles, 3);
    }
  } else if (event.provider == guids::kThread && event.opcode == kThreadReadyThread) {
    auto p = decodeReadyThread(event.payload);
    if (!p) {
      ++m_snap.malformedEvents;
    } else {
      const uint32_t threads[2] = {event.threadId, p->threadId};
      const MatchRole roles[2] = {MatchRole::HeaderThread, MatchRole::PayloadThread};
      addParent(ParentKind::ReadyThread, event.timestamp, false, threads, roles, 2);
    }
  } else if (event.provider == guids::kStackWalk && event.opcode == kStackWalkStack) {
    handleStack(event);
  } else if (event.provider == guids::kThread && event.opcode == kThreadSetName) {
    ++m_snap.setNameEvents;
    auto p = decodeThreadSetName(event.payload);
    if (!p) {
      ++m_snap.malformedEvents;
    } else if (m_snap.threadNames.size() < m_config.maxThreadNames) {
      m_snap.threadNames.push_back(ThreadNameEvent{p->processId, p->threadId, std::move(p->name)});
    }
  } else if (event.provider == guids::kThread && event.opcode >= kThreadStart &&
             event.opcode <= kThreadDcEnd) {
    if (auto p = decodeThreadLifetime(event.payload))
      m_snap.threadToProcess.emplace(p->threadId, p->processId);
  } else if (event.provider == guids::kProcess && event.opcode >= kProcessStart &&
             event.opcode <= kProcessDcEnd) {
    if (auto p = decodeProcess(event.payload, event.version)) {
      if (!p->imageFileName.empty()) m_snap.processImage[p->processId] = p->imageFileName;
    }
  }

  evict(event.timestamp);
}

void StatsAccumulator::finish() {
  if (m_finished) return;
  m_finished = true;
  evict(~uint64_t{0});
  // Flush anything evict() left (timestamp + window overflow).
  for (const Parent& parent : m_parents) {
    if (!parent.matched) ++m_snap.parents[static_cast<size_t>(parent.kind)].withoutStack;
  }
  m_parents.clear();
  m_pending.clear();
}

}  // namespace tachyon::etlstat
