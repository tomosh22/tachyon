#include "etlconvert/module_table.h"

#include "etlconvert/etl_events.h"

#include <algorithm>
#include <cstring>

namespace tachyon::etlconvert {

void ModuleBucket::insert(const ModuleInterval& interval) {
  auto it = std::upper_bound(m_intervals.begin(), m_intervals.end(), interval,
                             [](const ModuleInterval& a, const ModuleInterval& b) {
                               return a.base != b.base ? a.base < b.base : a.loadTs < b.loadTs;
                             });
  m_intervals.insert(it, interval);
}

const ModuleInterval* ModuleBucket::find(uint64_t addr, uint64_t ts, size_t& hint) const {
  auto contains = [&](const ModuleInterval& e) {
    return addr >= e.base && addr - e.base < e.size && ts >= e.loadTs && ts < e.unloadTs;
  };
  if (hint < m_intervals.size() && contains(m_intervals[hint])) return &m_intervals[hint];
  // Last interval whose base is <= addr, then walk back a few entries (same base reloaded over time, or
  // a short earlier interval shadowing nothing).
  auto it = std::upper_bound(m_intervals.begin(), m_intervals.end(), addr,
                             [](uint64_t a, const ModuleInterval& e) { return a < e.base; });
  size_t idx = static_cast<size_t>(it - m_intervals.begin());
  for (int tries = 0; idx > 0 && tries < 8; --idx, ++tries) {
    const ModuleInterval& e = m_intervals[idx - 1];
    if (contains(e)) {
      hint = idx - 1;
      return &e;
    }
  }
  return nullptr;
}

ModuleInterval* ModuleBucket::findOpen(uint64_t base, uint64_t size) {
  for (ModuleInterval& e : m_intervals) {
    if (e.base == base && e.unloadTs == kOpenEnded && (size == 0 || e.size == size)) return &e;
  }
  return nullptr;
}

bool ModuleBucket::unload(uint64_t base, uint64_t ts) {
  ModuleInterval* e = findOpen(base, 0);
  if (e == nullptr) return false;
  e->unloadTs = std::max(ts, e->loadTs);
  return true;
}

ModuleTable::ModuleTable(PeLookupFn lookup, DevicePathMapper mapper)
    : m_lookup(std::move(lookup)), m_mapper(std::move(mapper)) {}

LoadedModule ModuleTable::identify(uint32_t pid, uint64_t base, uint64_t size, const std::string& loggedPath,
                                   uint32_t timeDateStamp) {
  LoadedModule info;
  info.path = m_mapper.toDosPath(loggedPath);
  trace::ModuleInfo& rec = info.rec;
  rec.pid = pid;
  rec.base = base;
  rec.size = size;
  rec.load_ts = 0;
  rec.unload_ts = kOpenEnded;

  auto it = m_peCache.find(info.path);
  if (it == m_peCache.end()) {
    CachedPe cached;
    Result<PeInfo> parsed = m_lookup ? m_lookup(info.path) : parsePeFile(info.path);
    if (parsed.ok()) {
      cached.ok = true;
      cached.info = std::move(parsed).value();
    } else {
      cached.missing = true;
    }
    it = m_peCache.emplace(info.path, std::move(cached)).first;
  }
  const CachedPe& pe = it->second;

  // The file on disk is only trusted when it is the build that was loaded.
  bool mismatch = false;
  if (pe.ok) {
    if (timeDateStamp != 0 && pe.info.timeDateStamp != 0 && pe.info.timeDateStamp != timeDateStamp)
      mismatch = true;
    if (size != 0 && pe.info.sizeOfImage != 0 && pe.info.sizeOfImage != size) mismatch = true;
  }

  if (pe.ok && !mismatch && pe.info.hasCodeView) {
    rec.id_kind = trace::kModuleIdPdb;
    rec.id_len = 16;
    std::memcpy(rec.id.data(), pe.info.guid, 16);
    rec.age = pe.info.age;
    ++m_counters.rsds;
    return info;
  }

  // Fallback: id_kind 3 (kModuleIdPeTimestampSize) with TimeDateStamp (u32) and SizeOfImage (u32),
  // little-endian, in the id bytes. Prefer the loaded image's own values (from the event) over the file's.
  const uint32_t stamp = timeDateStamp != 0 ? timeDateStamp : (pe.ok ? pe.info.timeDateStamp : 0);
  const uint32_t imageSize = size != 0 ? static_cast<uint32_t>(size) : (pe.ok ? pe.info.sizeOfImage : 0);
  rec.id_kind = trace::kModuleIdPeTimestampSize;
  rec.id_len = 8;
  std::memcpy(rec.id.data(), &stamp, 4);
  std::memcpy(rec.id.data() + 4, &imageSize, 4);
  rec.age = 0;
  ++m_counters.fallback;
  if (!pe.ok) {
    ++m_counters.fileMissing;
  } else if (mismatch) {
    ++m_counters.mismatch;
  } else {
    ++m_counters.noCodeView;
  }
  return info;
}

uint32_t ModuleTable::load(uint32_t pid, uint64_t base, uint64_t size, const std::string& loggedPath,
                           uint32_t /*checksum*/, uint32_t timeDateStamp, uint64_t loadTs) {
  const bool kernel = base >= kKernelAddressBase;
  ModuleBucket& bucket = kernel ? m_kernel : m_processes[pid];
  if (const ModuleInterval* open = bucket.findOpen(base, size)) return open->moduleIdx;
  LoadedModule info = identify(kernel ? 0 : pid, base, size, loggedPath, timeDateStamp);
  info.rec.load_ts = loadTs;
  const auto idx = static_cast<uint32_t>(m_modules.size());
  m_modules.push_back(std::move(info));
  bucket.insert(ModuleInterval{base, size, loadTs, kOpenEnded, idx});
  return idx;
}

void ModuleTable::unload(uint32_t pid, uint64_t base, uint64_t ts) {
  ModuleBucket* bucket = &m_kernel;
  if (base < kKernelAddressBase) {
    auto it = m_processes.find(pid);
    if (it == m_processes.end()) return;
    bucket = &it->second;
  }
  const ModuleInterval* open = bucket->findOpen(base, 0);
  if (open == nullptr) return;
  m_modules[open->moduleIdx].rec.unload_ts = std::max(ts, open->loadTs);
  bucket->unload(base, ts);
}

const ModuleBucket* ModuleTable::bucketFor(uint32_t pid) const {
  auto it = m_processes.find(pid);
  return it == m_processes.end() ? nullptr : &it->second;
}

}  // namespace tachyon::etlconvert
