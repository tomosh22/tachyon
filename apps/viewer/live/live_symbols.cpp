#include "viewer/live/live_symbols.h"

#include "core/log.h"
#include "core/platform.h"
#include "symbols/dia/dia_provider.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "viewer/status/symbol_paths.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

namespace tachyon::viewer {

namespace {
std::atomic<int> g_epochsAlive{0};  // all LiveSymbols objects together (tests run one at a time)
}  // namespace

struct LiveSymbols::Epoch {
  Epoch() { ++g_epochsAlive; }
  // Declared first: destroyed last, after the service joined its workers.
  std::shared_ptr<model::TraceModel> model;  // the tables only: modules, frames, nodes
  std::unique_ptr<symbols::SymbolService> service;
  std::vector<uint8_t> nonLeaf;  // by frame: some node referencing the frame has a child
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  ~Epoch() {
    if (service) service->cancel();
    service.reset();
    --g_epochsAlive;
  }
};

namespace {

bool sameModuleRecord(const model::Module& a, const model::Module& b) {
  return a.pid == b.pid && a.base == b.base && a.size == b.size && a.path == b.path && a.id == b.id;
}

std::vector<uint8_t> nonLeafFrames(const model::TraceModel& m) {
  std::vector<uint8_t> flags(m.frames.size(), 0);
  for (const model::CallstackNode& n : m.nodes) {
    if (!n.parent.valid() || n.parent.get() == 0 || n.parent.get() >= m.nodes.size()) continue;
    const FrameIdx pf = m.nodes[n.parent.get()].frame;
    if (pf.valid() && pf.get() < flags.size()) flags[pf.get()] = 1;
  }
  return flags;
}

void registerPlatformProviders(symbols::SymbolService& service) {
#if TACHYON_PLATFORM_WINDOWS && TACHYON_HAVE_DIA
  symbols::registerDiaProvider(service);
#endif
#if TACHYON_PLATFORM_LINUX && TACHYON_HAVE_LIBDW
  symbols::registerDwarfProvider(service);
#endif
  (void)service;
}

}  // namespace

LiveSymbols::LiveSymbols(LiveSymbolsConfig config) : m_config(std::move(config)) {}

LiveSymbols::~LiveSymbols() {
  std::shared_ptr<Epoch> old;
  {
    std::lock_guard lock(m_statsMutex);
    old = std::move(m_epoch);
  }
  // `old` is released here, outside the lock: its workers are joined (stats() must not wait for that).
}

LiveSymbolStats LiveSymbols::stats() const {
  std::lock_guard lock(m_statsMutex);
  LiveSymbolStats s = m_stats;
  s.epochsAlive = static_cast<uint32_t>(std::max(0, g_epochsAlive.load()));
  if (m_epoch && m_epoch->service) {
    s.modulesLoaded += m_epoch->service->modulesLoaded();
    s.modulesAdopted += m_epoch->service->modulesAdopted();
  }
  return s;
}

void LiveSymbols::reset(LiveSymbolsConfig config) {
  std::shared_ptr<Epoch> old;
  {
    std::lock_guard lock(m_statsMutex);
    m_config = std::move(config);
    old = std::move(m_epoch);
    m_stats = {};
  }
  // `old` is released here (outside the lock): when no document still uses it, its workers are joined.
}

bool LiveSymbols::sameModules(const Epoch& epoch, const model::TraceModel& snapshot) const {
  const model::TraceModel& m = *epoch.model;
  if (m.modules.size() != snapshot.modules.size()) return false;
  for (size_t i = 0; i < m.modules.size(); ++i)
    if (!sameModuleRecord(m.modules[i], snapshot.modules[i])) return false;
  return true;
}

bool LiveSymbols::sameTables(const Epoch& epoch, const model::TraceModel& snapshot,
                             const std::vector<uint8_t>& nonLeaf) const {
  const model::TraceModel& m = *epoch.model;
  if (m.frames.size() != snapshot.frames.size() || !sameModules(epoch, snapshot)) return false;
  for (size_t i = 0; i < m.frames.size(); ++i)
    if (m.frames[i].module != snapshot.frames[i].module ||
        m.frames[i].addr_or_rva != snapshot.frames[i].addr_or_rva)
      return false;
  // A frame that was only ever a leaf and now has a caller above it needs its return-address lookup.
  for (size_t i = 0; i < nonLeaf.size() && i < epoch.nonLeaf.size(); ++i)
    if (nonLeaf[i] && !epoch.nonLeaf[i]) return false;
  return true;
}

std::shared_ptr<LiveSymbols::Epoch> LiveSymbols::startEpoch(const model::TraceModel& snapshot,
                                                            std::vector<uint8_t> nonLeaf,
                                                            const std::shared_ptr<Epoch>& previous) {
  auto epoch = std::make_shared<Epoch>();
  epoch->model = std::make_shared<model::TraceModel>();
  epoch->model->modules = snapshot.modules;
  epoch->model->frames = snapshot.frames;
  epoch->model->nodes = snapshot.nodes;
  epoch->nonLeaf = std::move(nonLeaf);

  epoch->service = std::make_unique<symbols::SymbolService>(
      m_config.servers, m_config.cacheDir, previous ? previous->service->sharedStrings() : nullptr);
  symbols::SymbolService& service = *epoch->service;
  service.setWorkerThreads(2);  // a few workers, not one per core: the viewer shares the machine
  registerPlatformProviders(service);
  if (!m_config.searchPaths.empty()) {
    const SymbolPathReport report =
        applySymbolSearchPaths(service, m_config.searchPaths, m_config.servers, m_config.cacheDir);
    for (const std::string& problem : report.problems)
      TACHYON_LOG_WARN("live symbols: search path: {}", problem);
  }
  if (m_config.configure) m_config.configure(service);
  if (previous) service.adoptRun(*previous->service);
  service.resolveAllAsync(static_cast<const model::TraceModel&>(*epoch->model), m_config.onModule);
  return epoch;
}

std::shared_ptr<symbols::SymbolService> LiveSymbols::serviceFor(const model::TraceModel& snapshot) {
  std::vector<uint8_t> nonLeaf = nonLeafFrames(snapshot);
  if (m_epoch && sameTables(*m_epoch, snapshot, nonLeaf))
    return std::shared_ptr<symbols::SymbolService>(m_epoch, m_epoch->service.get());
  if (m_epoch) {
    // Only frames / nodes grew (new code was sampled, the module list is the same): a service resolves a
    // fixed set of frames, so the new ones wait for the next epoch. Epochs cost a thread start and a
    // re-resolution of every frame, so frame-only growth is batched; a new or changed module starts one at
    // once.
    if (m_config.frameEpochIntervalMs > 0 && sameModules(*m_epoch, snapshot) &&
        std::chrono::steady_clock::now() - m_epoch->started <
            std::chrono::milliseconds(m_config.frameEpochIntervalMs))
      return std::shared_ptr<symbols::SymbolService>(m_epoch, m_epoch->service.get());
    const symbols::SymbolProgress p = m_epoch->service->progress();
    // The old run is still loading modules: adopting now would cancel it and start those loads again, and a
    // slow module would never finish. Keep serving with the old service until it is done.
    if (p.done < p.total) return std::shared_ptr<symbols::SymbolService>(m_epoch, m_epoch->service.get());
  }
  std::shared_ptr<Epoch> previous = m_epoch;
  std::shared_ptr<Epoch> next = startEpoch(snapshot, std::move(nonLeaf), previous);
  {
    std::lock_guard lock(m_statsMutex);
    if (previous && previous->service) {
      m_stats.modulesLoaded += previous->service->modulesLoaded();
      m_stats.modulesAdopted += previous->service->modulesAdopted();
    }
    ++m_stats.epochs;
    m_epoch = next;
  }
  return std::shared_ptr<symbols::SymbolService>(next, next->service.get());
}

}  // namespace tachyon::viewer
