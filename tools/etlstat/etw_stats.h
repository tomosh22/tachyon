// Platform-independent event statistics: per-(provider, opcode, version) counts, StackWalk-to-parent
// matching by (EventTimeStamp, StackThread), unmatched-stack accounting, depth tracking, per-CPU rates.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "etw_types.h"

namespace tachyon::etlstat {

struct EventKey {
  Guid provider;
  uint8_t opcode = 0;
  uint8_t version = 0;
  friend bool operator==(const EventKey&, const EventKey&) = default;
  friend auto operator<=>(const EventKey&, const EventKey&) = default;
};

struct KeyStats {
  uint64_t count = 0;
  uint64_t totalBytes = 0;
  uint32_t minSize = 0xFFFFFFFFu;
  uint32_t maxSize = 0;
};

// Events that can own a stack: the three types we enable stack walking for.
enum class ParentKind : uint8_t { SampledProfile, CSwitch, ReadyThread, kCount };

// Thread-safe: pure function.
const char* parentKindName(ParentKind kind);

// Which identifier of the parent event the StackWalk's StackThread matched.
enum class MatchRole : uint8_t { HeaderThread, PayloadThread, PayloadOldThread, kCount };

// Thread-safe: pure function.
const char* matchRoleName(MatchRole role);

struct ParentStats {
  uint64_t total = 0;
  uint64_t withStack = 0;
  uint64_t withoutStack = 0;  // evicted (or flushed at the end) without ever receiving a stack
  uint32_t maxDepth = 0;
  uint64_t depthSum = 0;  // sum of depths of matched stacks
};

struct CpuStats {
  uint64_t samples = 0;         // all SampledProfile events on this CPU
  uint64_t nonIdleSamples = 0;  // SampledProfile events whose ThreadId != 0
};

struct ThreadNameEvent {
  uint32_t processId = 0;
  uint32_t threadId = 0;
  std::string name;
};

struct StatsConfig {
  // How far (in trace ticks) behind the newest event a parent stays eligible for a stack match.
  uint64_t pendingWindowTicks = 10'000'000;
  // Limit of SetName events kept for display.
  size_t maxThreadNames = 16;
};

struct StatsSnapshot {
  std::map<EventKey, KeyStats> keys;
  uint64_t totalEvents = 0;
  uint64_t malformedEvents = 0;  // payload too short for the documented layout

  uint64_t stackEvents = 0;         // StackWalk opcode 32
  uint64_t stackMatched = 0;        // matched a pending parent
  uint64_t stackUnmatched = 0;      // no pending parent with the same (timestamp, thread)
  uint32_t maxStackDepth = 0;       // over all stacks
  uint64_t stackRoleCount[3] = {};  // MatchRole histogram
  ParentStats parents[3] = {};      // indexed by ParentKind

  uint64_t sampledProfile = 0;
  uint64_t sampledNonIdle = 0;
  uint64_t sampledNonIdleWithStack = 0;
  uint64_t firstSampleTs = 0;
  uint64_t lastSampleTs = 0;
  uint64_t firstEventTs = 0;
  uint64_t lastEventTs = 0;
  std::vector<CpuStats> cpus;

  uint64_t setNameEvents = 0;
  std::vector<ThreadNameEvent> threadNames;

  // Resolved at finish(): thread id -> process id (from Thread Start/DCStart) and image name per pid.
  std::unordered_map<uint32_t, uint32_t> threadToProcess;
  std::unordered_map<uint32_t, std::string> processImage;
  std::unordered_map<uint32_t, uint64_t> samplesPerThread;  // non-idle SampledProfile by ThreadId
};

class StatsAccumulator {
 public:
  explicit StatsAccumulator(StatsConfig config = {});

  // Not thread-safe. Feed events in trace (timestamp) order, as ProcessTrace delivers them.
  void add(const EtwEvent& event);
  // Not thread-safe. Flushes still-pending parents as "without stack". Call once after the last event.
  void finish();
  // Thread-safe for concurrent const access after finish().
  const StatsSnapshot& snapshot() const { return m_snap; }

 private:
  struct Key {
    uint64_t timestamp;
    uint32_t thread;
    friend bool operator==(const Key&, const Key&) = default;
  };
  struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
      return static_cast<size_t>(k.timestamp * 0x9E3779B97F4A7C15ull ^
                                 (static_cast<uint64_t>(k.thread) << 1));
    }
  };
  struct Parent {
    uint64_t timestamp = 0;
    ParentKind kind = ParentKind::SampledProfile;
    bool matched = false;
    bool idle = false;
    uint8_t keyCount = 0;
    Key keys[3] = {};
    MatchRole roles[3] = {};
  };

  void addParent(ParentKind kind, uint64_t timestamp, bool idle, const uint32_t* threads,
                 const MatchRole* roles, size_t count);
  void handleStack(const EtwEvent& event);
  void evict(uint64_t now);

  StatsConfig m_config;
  StatsSnapshot m_snap;
  std::deque<Parent> m_parents;  // ordered by arrival; m_parents[i] has id m_baseId + i
  uint64_t m_baseId = 0;
  std::unordered_multimap<Key, uint64_t, KeyHash> m_pending;
  bool m_finished = false;
};

}  // namespace tachyon::etlstat
