// LiveSymbols (WP-65b): symbol resolution for the successive snapshots of a live capture.
//
// A SymbolService resolves ONE model (it keeps raw pointers into the model's frame table). A live capture
// produces a new snapshot model every ~500 ms, so a service per snapshot would locate and load every symbol
// file again each time. The design that avoids it:
//
//   * The live model's tables (modules, frames, callstack nodes) are append-only with stable indices, so a
//     FrameIdx means the same thing in every snapshot of one capture. A service that resolved the tables of
//     snapshot N answers lookup(frame) correctly for snapshot N+1 too, for every frame it already knew.
//   * LiveSymbols therefore keeps an "epoch": a service plus a slim private model (just the tables, no
//     samples) it was started on. While a snapshot has the same tables (the steady state of a running game
//     after the first seconds), serviceFor() returns the epoch's service unchanged, no work at all.
//   * When the tables grew (new frames, a new module, a frame that became a non-leaf), a new epoch is
//     started: a new service that shares the string table of the old one and ADOPTS its finished run
//     (SymbolService::adoptRun, WP-68), so only modules that are new are located and loaded; everything the
//     old run finished is reused (provider shared, no reload). While the old run still has modules loading,
//     the old service keeps serving (its lookups return "unresolved" for frames it has not seen) and the new
//     epoch starts at the first snapshot after the run finished, so a slow symbol server never restarts a
//     load that is in progress.
//   * Module results are published to the UI through the `onModule` callback (as for a file document), which
//     bumps the viewer's symbol generation; panels then re-query with the new function keys.
//
// Thread safety: serviceFor() / reset() / the destructor belong to ONE thread (the live snapshot worker); the
// services it returns are used by any thread through their lock-free const API (lookup, moduleState, ...).
#pragma once

#include "model/model.h"
#include "symbols/symbol_service.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace tachyon::viewer {

struct LiveSymbolsConfig {
  std::vector<std::string> servers;
  std::filesystem::path cacheDir;
  std::vector<std::string> searchPaths;  // Settings::symbolSearchPaths (applied to every new epoch's service)
  // Called with each new service after the viewer configured it and before resolution starts.
  std::function<void(symbols::SymbolService&)> configure;
  // Called from the symbol workers at each module state change; thread-safe.
  std::function<void(ModuleIdx, symbols::SymbolState)> onModule;
  // A new epoch for frames / nodes that grew without a change of the module table starts at most this often
  // (a new or changed module starts one at once); 0 = at every snapshot with new frames.
  int frameEpochIntervalMs = 3000;
};

struct LiveSymbolStats {
  uint32_t epochs = 0;         // services started so far
  uint32_t modulesLoaded = 0;  // provider loads by all epochs (a module adopted by a later epoch counts once)
  uint32_t epochsAlive = 0;  // symbol services (epochs) still alive: the current one and those documents hold
  uint32_t modulesAdopted =
      0;  // adoptions by later epochs (a module carried through N epochs counts N times)
};

class LiveSymbols {
 public:
  // Thread-safe (constructor).
  explicit LiveSymbols(LiveSymbolsConfig config);
  // Worker thread. Cancels the current run (joins its workers).
  ~LiveSymbols();
  LiveSymbols(const LiveSymbols&) = delete;
  LiveSymbols& operator=(const LiveSymbols&) = delete;

  // Worker thread only. The service to attach to the document of `snapshot` (null for an empty snapshot):
  // the current epoch's, or a new one when the tables changed and the current run is finished. Never blocks
  // on symbol loading. The returned pointer keeps the epoch (service and its slim model) alive.
  std::shared_ptr<symbols::SymbolService> serviceFor(const model::TraceModel& snapshot);

  // Worker thread only. Forgets every epoch and uses `config` from the next serviceFor() on, resolving from
  // scratch (a new symbol path or Retry): nothing is adopted.
  void reset(LiveSymbolsConfig config);

  // Thread-safe.
  LiveSymbolStats stats() const;

 private:
  struct Epoch;
  bool sameModules(const Epoch& epoch, const model::TraceModel& snapshot) const;
  bool sameTables(const Epoch& epoch, const model::TraceModel& snapshot,
                  const std::vector<uint8_t>& nonLeaf) const;
  std::shared_ptr<Epoch> startEpoch(const model::TraceModel& snapshot, std::vector<uint8_t> nonLeaf,
                                    const std::shared_ptr<Epoch>& previous);

  LiveSymbolsConfig m_config;
  std::shared_ptr<Epoch> m_epoch;
  mutable std::mutex m_statsMutex;
  LiveSymbolStats m_stats;
};

}  // namespace tachyon::viewer
