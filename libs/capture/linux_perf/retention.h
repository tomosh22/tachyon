// Retention rule of a circular perf capture (WP-60, hardened in WP-66): which sealed segments a flush may
// release. Pure functions and a small state object, so the rule is testable without a perf session.
//
// A flush(seconds) converts every sealed segment clipped to [flushTs - seconds, flushTs]. Afterwards it may
// delete the segments that no later flush could still need. A later flush may ask for a longer window than
// this one, so the horizon H of a flush is the largest `seconds` of the flushes made in the last
// `memoryNs` (default 5 minutes) including this one: one flush with a huge window raises H only for that
// long, not for the rest of the capture. (The circular budget still drops the oldest segments regardless.)
//
// A segment is released when
//   - its newest sample time is known and older than flushTs - H; or
//   - its newest sample time is unknown (0: no sample with a time was parsed in it) and a LATER sealed
//     segment is released by the rule above (it is older than the horizon by sequence); or
//   - its time is unknown and no sealed segment has a known time at all: all but the newest such segment
//     (without a single timestamp nothing can be placed in a window, and keeping them would leak disk).
// A flush with seconds == 0 releases nothing and does not raise H.
#pragma once

#include "capture/linux_perf/drain.h"

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace tachyon::capture::perf {

constexpr uint64_t kDefaultHorizonMemoryNs = 300ull * 1'000'000'000ull;

class RetentionHorizon {
 public:
  explicit RetentionHorizon(uint64_t memoryNs = kDefaultHorizonMemoryNs) : m_memoryNs(memoryNs) {}
  // Not thread-safe. Forgets all flushes (a new capture).
  void reset() { m_flushes.clear(); }
  // Not thread-safe. Records a flush at `flushTs` (ns) with window `seconds` and returns the horizon H in ns
  // that applies to it: 0 for seconds == 0 (release nothing).
  uint64_t note(uint64_t flushTs, uint32_t seconds);

 private:
  uint64_t m_memoryNs;
  std::deque<std::pair<uint64_t, uint32_t>> m_flushes;  // (flushTs, seconds), oldest first
};

// Thread-safe: pure function. The segments of `sealed` (oldest first) that the rule above releases for a
// flush at `flushTs` with horizon `horizonNs`; empty when horizonNs is 0.
std::vector<SealedSegment> selectReleasable(const std::vector<SealedSegment>& sealed, uint64_t flushTs,
                                            uint64_t horizonNs);

}  // namespace tachyon::capture::perf
