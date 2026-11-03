#include "capture/linux_perf/retention.h"

#include <algorithm>

namespace tachyon::capture::perf {

uint64_t RetentionHorizon::note(uint64_t flushTs, uint32_t seconds) {
  if (seconds == 0) return 0;
  // Flushes older than the memory no longer hold the horizon up.
  while (!m_flushes.empty() &&
         (flushTs < m_flushes.front().first || flushTs - m_flushes.front().first > m_memoryNs))
    m_flushes.pop_front();
  m_flushes.emplace_back(flushTs, seconds);
  uint32_t widest = 0;
  for (const auto& f : m_flushes) widest = std::max(widest, f.second);
  return static_cast<uint64_t>(widest) * 1'000'000'000ull;
}

std::vector<SealedSegment> selectReleasable(const std::vector<SealedSegment>& sealed, uint64_t flushTs,
                                            uint64_t horizonNs) {
  std::vector<SealedSegment> out;
  if (horizonNs == 0 || sealed.empty()) return out;
  const bool haveCutoff = flushTs > horizonNs;
  const uint64_t cutoff = haveCutoff ? flushTs - horizonNs : 0;
  // Index of the newest segment that is old by its own timestamps.
  size_t newestOld = sealed.size();
  bool anyTimed = false;
  for (size_t i = 0; i < sealed.size(); ++i) {
    if (sealed[i].lastTs == 0) continue;
    anyTimed = true;
    if (haveCutoff && sealed[i].lastTs < cutoff) newestOld = i;
  }
  for (size_t i = 0; i < sealed.size(); ++i) {
    const SealedSegment& seg = sealed[i];
    bool release = false;
    if (seg.lastTs != 0) {
      release = haveCutoff && seg.lastTs < cutoff;
    } else if (newestOld != sealed.size()) {
      release = i < newestOld;  // older by sequence than a segment that is itself past the horizon
    } else if (!anyTimed) {
      release = i + 1 < sealed.size();  // nothing is timed: keep only the newest
    }
    if (release) out.push_back(seg);
  }
  return out;
}

}  // namespace tachyon::capture::perf
