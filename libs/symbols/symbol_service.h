// SymbolService: loads symbol files for the modules of a TraceModel on a worker pool and memoizes
// FrameIdx -> ResolvedAddress (spec 3.6, WP-20).
//
// STRING TABLE: StringIdx values in every ResolvedAddress / InlineFrame the service returns index
// symbolStrings(), NOT TraceModel::strings.
#pragma once

#include "core/result.h"
#include "core/strong_id.h"
#include "model/model.h"
#include "symbols/json_symbol_provider.h"
#include "symbols/symbol_locator.h"
#include "symbols/symbol_provider.h"
#include "symbols/symbol_strings.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::symbols {

using LocateFn = std::function<Result<std::filesystem::path>(const Module&)>;
using ProviderFactory =
    std::function<std::unique_ptr<ISymbolProvider>(const Module&, const std::filesystem::path&)>;

struct SymbolProgress {
  uint32_t done = 0;   // modules that reached a final state
  uint32_t total = 0;  // modules referenced by at least one frame
};

class SymbolService {
 public:
  // Thread-safe (constructor). `servers` and `cacheDir` are kept for the WP-21 locator; until then the
  // default locate function only knows the symbol file overrides.
  SymbolService(std::vector<std::string> servers, std::filesystem::path cacheDir);
  // Cancels pending module loads, joins the workers. Does not touch the model.
  ~SymbolService();
  SymbolService(const SymbolService&) = delete;
  SymbolService& operator=(const SymbolService&) = delete;

  // Configuration. Not thread-safe; call before resolveAllAsync().
  // Number of worker threads (0 = min(hardware_concurrency, 8)).
  void setWorkerThreads(unsigned n) { m_workerThreads = n; }
  // Replaces the locate function consulted after the overrides. Default: SymbolLocator stub (NotFound).
  void setLocate(LocateFn locate);
  // Replaces the provider factory. Default: JsonSymbolProvider for *.json paths; nullptr result = Failed.
  // Providers must intern into stringSink() so their StringIdx values are valid for symbolStrings().
  void setProviderFactory(ProviderFactory factory);
  // Registers a tracegen `.symbols.json`. Modules whose ModuleId (or, lacking ids, path) matches an entry of
  // it get this file as their symbol file, ahead of the locate function. Not thread-safe.
  Result<void> addSymbolFileOverride(const std::filesystem::path& symbolsJson);

  // Starts resolution of every module referenced by a frame of `model` and returns immediately. Workers
  // transition each module Pending -> Loading -> Resolved / NotFound / Failed and call `onModule` from the
  // WORKER threads at each transition (Loading, then the final state); the callback must be thread-safe and
  // must not call back into resolveAllAsync/waitAll. The viewer marshals to its UI thread.
  // `model` must outlive the resolution (until waitAll() returns or the service is destroyed), and its frames
  // and modules vectors must not be resized or reordered meanwhile. The service never writes the model from a
  // worker; Module::state is written only by waitAll() on the calling thread. Calling it again waits for the
  // previous run first. Not thread-safe against other members except the const queries below, which may run
  // concurrently with the workers (not with this call's setup phase).
  void resolveAllAsync(model::TraceModel& model, std::function<void(ModuleIdx, SymbolState)> onModule = {});

  // Blocks until every worker finished, then copies the final states into Module::state of the model passed
  // to resolveAllAsync. Call from one thread (the one that owns the model). No-op if nothing is running.
  void waitAll();

  // Thread-safe, lock-free. Modules done / total (total = modules referenced by frames).
  SymbolProgress progress() const;
  // Thread-safe, lock-free. Current state; Pending for unknown modules and modules no frame references.
  SymbolState moduleState(ModuleIdx module) const;
  // Thread-safe. Error text of a NotFound / Failed module (empty otherwise).
  std::string moduleError(ModuleIdx module) const;

  // Thread-safe, lock-free, never blocks on loading; safe concurrently with resolveAllAsync workers.
  // Memoized: once a frame's module is Resolved the answer is a precomputed slot (computed by the worker that
  // loaded the module, published by the release store of the module state). Before that, for unmapped frames,
  // unresolvable modules and out-of-range indices, returns the shared static unresolved value. The reference
  // stays valid until the next resolveAllAsync() or destruction. StringIdx values index symbolStrings().
  const ResolvedAddress& lookup(FrameIdx frame) const;

  // Thread-safe, lock-free. Copy of the frame record as seen by resolveAllAsync (module invalid if unknown).
  model::Frame frame(FrameIdx frame) const;

  // The symbol string table that every StringIdx from lookup() / providers indexes (not the trace's table).
  // Guarantee: safe to read (get/find/size) concurrently with resolution; views stay valid for the lifetime
  // of the service (see SymbolStringTable).
  const SymbolStringTable& symbolStrings() const { return m_strings; }
  // Thread-safe. For provider factories (DIA / DWARF) that must intern into the service's table.
  SymbolStringTable& stringSink() { return m_strings; }

  // Thread-safe after the module reached Resolved; the provider of a resolved module (for lines /
  // function bounds queries), or nullptr.
  ISymbolProvider* provider(ModuleIdx module) const;

 private:
  struct Job;
  void workerLoop(Job* job);
  void processModule(Job& job, uint32_t moduleIdx);
  void setState(Job& job, uint32_t moduleIdx, SymbolState state, std::string error);

  struct Override {
    std::filesystem::path file;
    JsonModuleIdentity identity;
  };

  std::vector<std::string> m_servers;
  std::filesystem::path m_cacheDir;
  SymbolLocator m_locator;
  LocateFn m_locate;
  ProviderFactory m_factory;
  unsigned m_workerThreads = 0;
  std::vector<Override> m_overrides;

  SymbolStringTable m_strings;

  // Per-run state. Rebuilt by resolveAllAsync() while no workers run.
  std::unique_ptr<Job> m_job;
  const model::Frame* m_frames = nullptr;
  size_t m_frameCount = 0;
  size_t m_moduleCount = 0;
};

}  // namespace tachyon::symbols
