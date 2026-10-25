// Lazy per-thread timeline: yields the thread's Running / Waiting / Ready segments in order, from the
// explicit interval list and/or the generated pattern. The final segment is extended to the end of the trace.
#pragma once

#include "prng.h"
#include "scenario.h"

#include <cstdint>

namespace tachyon::tracegen {

struct Segment {
  SegState state = SegState::Run;
  uint64_t start = 0, end = 0;  // ticks since trace start
  uint8_t waitReason = 0, waitMode = 0, priority = 8;
  int32_t blockingStack = -1;
  uint64_t readyAt = 0;  // ticks since trace start, 0 = none
  uint32_t wakerTid = 0;
  int32_t wakerStack = -1;
  int32_t mix = -1;
  bool fromStart = false;  // Run segment without a switch-in record
};

class Timeline {
 public:
  // Not thread-safe (one instance per thread).
  Timeline(const Scenario& scenario, const ThreadSpec& thread);

  // Not thread-safe. Next segment in time order; false after the last one.
  bool next(Segment& out);

 private:
  bool produce(Segment& seg);

  const ThreadSpec& m_thread;
  Prng m_rng;
  uint64_t m_t = 0;
  uint64_t m_end = 0;
  size_t m_idx = 0;
  bool m_nextIsRun = true;
  Segment m_cur;
  bool m_have = false;
  bool m_done = false;
};

}  // namespace tachyon::tracegen
