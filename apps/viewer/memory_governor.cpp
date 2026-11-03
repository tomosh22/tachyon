#include "viewer/memory_governor.h"

#include <algorithm>
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace tachyon::viewer {

uint64_t physicalMemoryBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof status;
  return GlobalMemoryStatusEx(&status) ? static_cast<uint64_t>(status.ullTotalPhys) : 0;
#else
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long pageSize = sysconf(_SC_PAGE_SIZE);
  return pages > 0 && pageSize > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize) : 0;
#endif
}

uint64_t memoryCeilingBytes(uint32_t percent, uint64_t physical) {
  if (percent == 0 || physical == 0) return 0;
  return physical * std::min<uint32_t>(percent, 100) / 100;
}

std::string formatBytes(uint64_t bytes) {
  char buf[48];
  const double v = static_cast<double>(bytes);
  if (bytes >= (1ull << 30)) {
    std::snprintf(buf, sizeof buf, "%.1f GiB", v / static_cast<double>(1ull << 30));
  } else {
    std::snprintf(buf, sizeof buf, "%.0f MiB", v / static_cast<double>(1ull << 20));
  }
  return buf;
}

bool MemoryGovernor::update(const MemorySources& src, uint64_t ceiling) {
  MemoryStatus& st = m_status;
  st.ceilingBytes = ceiling;
  const auto get = [](const std::function<uint64_t()>& f) { return f ? f() : uint64_t{0}; };
  const uint64_t model = get(src.modelBytes);
  uint64_t lod = get(src.lodBytes);
  uint64_t cache = get(src.cacheBytes);
  st.usedBytes = model + lod + cache;
  if (ceiling == 0 || st.usedBytes <= ceiling) return false;

  const MemoryStage before = st.stage;
  // 1. The timeline's mipmap data.
  if (src.trimLod && lod > 0) {
    const uint64_t freed = src.trimLod(st.usedBytes - ceiling);
    if (freed > 0) {
      st.lodFreedBytes += freed;
      lod = get(src.lodBytes);
      st.usedBytes = model + lod + cache;
      st.stage = std::max(st.stage, MemoryStage::LodReduced);
      st.message = "Memory ceiling (" + formatBytes(ceiling) + ") reached: freed " + formatBytes(freed) +
                   " of timeline detail levels. Zooming on busy rows may be slower; raise the limit under "
                   "View > Memory limit.";
    }
  }
  // 2. The analysis caches.
  if (st.usedBytes > ceiling && st.stage < MemoryStage::CachesLimited && src.limitCaches) {
    const uint64_t room = ceiling > model + lod ? ceiling - model - lod : 0;
    st.cachesFreedBytes += cache;
    src.limitCaches(room / 2);
    cache = get(src.cacheBytes);
    st.usedBytes = model + lod + cache;
    st.stage = MemoryStage::CachesLimited;
    st.message = "Memory ceiling (" + formatBytes(ceiling) +
                 ") reached: " + (st.lodFreedBytes ? "timeline detail levels were freed and " : "") +
                 "analysis results are no longer cached (queries are recomputed). Raise the limit under "
                 "View > Memory limit.";
  }
  // 3. Nothing left to give.
  if (st.usedBytes > ceiling && st.stage < MemoryStage::ModelOverCeiling && model + lod > ceiling) {
    st.stage = MemoryStage::ModelOverCeiling;
    st.message = "The trace needs " + formatBytes(model) + " in memory, more than the memory ceiling (" +
                 formatBytes(ceiling) +
                 "); everything optional was freed. Raise the limit under View > Memory limit.";
  }
  return st.stage != before;
}

}  // namespace tachyon::viewer
