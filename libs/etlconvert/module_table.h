// Loaded-module bookkeeping: identification from PE debug directories and the per-process address interval
// map used to turn absolute addresses into (module, RVA) frames.
#pragma once

#include "etlconvert/device_path.h"
#include "etlconvert/pe_info.h"
#include "trace/trace_format.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tachyon::etlconvert {

// Sentinel used internally for "still loaded" intervals; replaced by the trace end when written.
inline constexpr uint64_t kOpenEnded = ~uint64_t{0};

// Looks up the PE identity of the image at a (DOS) path. Empty function = read the real file.
using PeLookupFn = std::function<Result<PeInfo>(const std::string& dosPath)>;

// How a module was identified; counted in ConvertStats.
struct ModuleIdCounters {
  uint32_t rsds = 0;         // id_kind 1: PDB GUID + age from the CodeView record
  uint32_t fallback = 0;     // id_kind 0 with TimeDateStamp + SizeOfImage
  uint32_t fileMissing = 0;  // subset of fallback: file could not be read
  uint32_t noCodeView = 0;   // subset of fallback: PE has no RSDS record
  uint32_t mismatch = 0;     // subset of fallback: file on disk differs from the loaded image
};

struct LoadedModule {
  trace::ModuleInfo rec;  // naturally aligned copy of ModuleRec; path (StringIdx) is filled in when written
  std::string path;       // DOS path when conversion succeeded, else as logged
};

// One interval of the address space of a process: [base, base + size) valid during [loadTs, unloadTs).
struct ModuleInterval {
  uint64_t base = 0;
  uint64_t size = 0;
  uint64_t loadTs = 0;
  uint64_t unloadTs = kOpenEnded;
  uint32_t moduleIdx = 0;
};

// Intervals of one address space, sorted by (base, loadTs).
class ModuleBucket {
 public:
  // Not thread-safe.
  void insert(const ModuleInterval& interval);
  // Thread-safe for const access. Finds the interval containing `addr` at time `ts`. `hint` is a per-caller
  // cache (the index of the last hit); it makes runs of frames in one module O(1).
  const ModuleInterval* find(uint64_t addr, uint64_t ts, size_t& hint) const;
  // Not thread-safe. Marks the open interval starting at `base` as unloaded at `ts`.
  bool unload(uint64_t base, uint64_t ts);
  // Thread-safe for const access.
  const std::vector<ModuleInterval>& intervals() const { return m_intervals; }
  // Not thread-safe.
  ModuleInterval* findOpen(uint64_t base, uint64_t size);

 private:
  std::vector<ModuleInterval> m_intervals;
};

class ModuleTable {
 public:
  // Not thread-safe. `lookup` empty = read files from disk.
  ModuleTable(PeLookupFn lookup, DevicePathMapper mapper);

  // Not thread-safe. Registers an image mapped by `pid`; `loadTs` is 0 for modules loaded before the trace
  // (rundown). A second registration of the same (pid, base, size) while it is mapped is ignored. Returns the
  // module index. Kernel-space images (base >= kKernelAddressBase) go to the shared kernel address space.
  uint32_t load(uint32_t pid, uint64_t base, uint64_t size, const std::string& loggedPath, uint32_t checksum,
                uint32_t timeDateStamp, uint64_t loadTs);
  // Not thread-safe. Marks the module at (pid, base) unloaded at `ts`.
  void unload(uint32_t pid, uint64_t base, uint64_t ts);

  // Thread-safe for const access. nullptr when the process has no known modules.
  const ModuleBucket* bucketFor(uint32_t pid) const;
  // Thread-safe for const access.
  const ModuleBucket& kernelBucket() const { return m_kernel; }
  // Thread-safe for const access. Indexed by ModuleIdx.
  const std::vector<LoadedModule>& modules() const { return m_modules; }
  // Thread-safe for const access.
  const ModuleIdCounters& counters() const { return m_counters; }

 private:
  LoadedModule identify(uint32_t pid, uint64_t base, uint64_t size, const std::string& loggedPath,
                        uint32_t timeDateStamp);

  PeLookupFn m_lookup;
  DevicePathMapper m_mapper;
  std::vector<LoadedModule> m_modules;
  std::unordered_map<uint32_t, ModuleBucket> m_processes;
  ModuleBucket m_kernel;
  ModuleIdCounters m_counters;
  struct CachedPe {
    bool ok = false;
    bool missing = false;
    PeInfo info;
  };
  std::unordered_map<std::string, CachedPe> m_peCache;
};

}  // namespace tachyon::etlconvert
