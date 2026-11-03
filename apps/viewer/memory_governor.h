// Memory ceiling of the viewer (WP-64): keeps the data of the open trace within a byte ceiling by degrading
// gracefully instead of failing.
//
// What counts: the resident model (samples, tables, events), the timeline index (LOD levels) and the analysis
// query caches. The ceiling is Settings::memoryCeilingPercent of the physical RAM (default 75%, 0 = none).
//
// Degradation, in this order, one stage per update() that finds the total above the ceiling:
//   1. LodReduced     the timeline's sample mipmap data is freed (the fine strip level, then the finest LOD levels
//                     one by one, always keeping the coarsest); the zooms those levels served are answered from
//                     the next coarser level or straight from the samples - exact, but slower at busy rows.
//   2. CachesLimited  the analysis query caches are cleared and their budget shrinks to what the ceiling leaves
//                     (to nothing): queries are recomputed instead of cached.
//   3. ModelOverCeiling  the trace itself does not fit; nothing more can be freed. Only reported.
// Nothing is restored automatically (that would make the memory use flap); re-opening the trace rebuilds
// everything with the then current ceiling. The timeline LOD also receives the ceiling up front, as a byte
// budget for its levels, so that a trace which cannot afford the full index is built coarse in the first place.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace tachyon::viewer {

// Thread-safe: pure function. Physical memory of the machine in bytes (0 when unknown).
uint64_t physicalMemoryBytes();

// Thread-safe: pure function. percent of `physical`; 0 percent or an unknown size mean "no ceiling" (0).
uint64_t memoryCeilingBytes(uint32_t percent, uint64_t physical);

// "1.5 GiB" / "320 MiB".
std::string formatBytes(uint64_t bytes);

enum class MemoryStage : uint8_t { Normal, LodReduced, CachesLimited, ModelOverCeiling };

// What the governor can measure and free. All callbacks run on the calling (main) thread.
struct MemorySources {
  std::function<uint64_t()> modelBytes;  // TraceModel::residentBytes() (the caller caches it: it is O(strings))
  std::function<uint64_t()> lodBytes;    // timeline index bytes
  std::function<uint64_t()> cacheBytes;  // analysis caches
  // Frees at least `bytes` of timeline index; returns the bytes freed (0 = nothing left to free).
  std::function<uint64_t(uint64_t bytes)> trimLod;
  // Clears the analysis caches and limits their budget to `budgetBytes` (0 = cache nothing).
  std::function<void(uint64_t budgetBytes)> limitCaches;
};

struct MemoryStatus {
  MemoryStage stage = MemoryStage::Normal;
  uint64_t usedBytes = 0;     // at the last update(), after degrading
  uint64_t ceilingBytes = 0;  // 0 = none
  uint64_t lodFreedBytes = 0;
  uint64_t cachesFreedBytes = 0;
  std::string message;  // what was degraded (empty in Normal); for the toast / banner
};

class MemoryGovernor {
 public:
  // Main-thread only. A new document: forget the degradation.
  void reset() { m_status = {}; }

  // Main-thread only. Measures the sources against `ceilingBytes` (0 = none) and degrades as described above
  // when the total is over. Returns true when the stage advanced in this call (the caller shows `message`).
  bool update(const MemorySources& sources, uint64_t ceilingBytes);

  // Main-thread only.
  const MemoryStatus& status() const { return m_status; }

 private:
  MemoryStatus m_status;
};

}  // namespace tachyon::viewer
