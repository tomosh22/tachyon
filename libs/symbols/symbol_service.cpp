#include "symbols/symbol_service.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace tachyon::symbols {

struct SymbolService::Job {
  model::TraceModel* mutableModel =
      nullptr;                  // set by the non-const resolveAllAsync overload (waitAll target)
  std::vector<Module> modules;  // snapshot: workers never read the live model's modules
  std::unique_ptr<std::atomic<uint8_t>[]> state;
  // Cache files pinned in the service's SymbolLocator per module (empty = none); released together.
  std::vector<std::filesystem::path> pinned;
  std::vector<std::string> errors;  // written by the owning worker before the final state's release store
  std::vector<std::shared_ptr<ISymbolProvider>> providers;  // shared: an adopting service keeps them alive
  // Per module: 0 = resolve normally, 1 = providers[i] was adopted (finished), 2 = adopted failure (state in
  // adoptedState, error in errors). Written before the workers start.
  std::vector<uint8_t> adopt;
  std::vector<uint8_t> adoptedState;
  std::atomic<uint32_t> adoptedCount{0};
  std::atomic<uint32_t> loadedCount{0};
  std::vector<ResolvedAddress> memo;  // indexed by FrameIdx; slot written before its module's release store
  // Return-address variant (rva - 1), only for frames that occur in a non-leaf stack position. callerSlot is
  // indexed by FrameIdx (kNoSlot = never non-leaf); memoCaller is indexed by the slot. Written like `memo`.
  std::vector<uint32_t> callerSlot;
  std::vector<ResolvedAddress> memoCaller;
  std::vector<uint32_t> referenced;
  std::vector<std::vector<uint32_t>> framesOfModule;
  std::atomic<uint32_t> next{0};
  std::atomic<uint32_t> done{0};
  std::atomic<bool> cancel{false};
  std::function<void(ModuleIdx, SymbolState)> onModule;
  std::vector<std::thread> threads;
  bool applied = false;
};

// What adoptRun() takes from the previous service, indexed by that run's module index.
struct SymbolService::Adopted {
  std::vector<Module> modules;
  std::vector<uint8_t> state;
  std::vector<std::shared_ptr<ISymbolProvider>> providers;
  std::vector<std::string> errors;
  std::vector<std::filesystem::path> pinned;  // pinned in THIS service's locator (empty = none)
};

namespace {

constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

bool sameModule(const Module& a, const Module& b) {
  return a.pid == b.pid && a.base == b.base && a.size == b.size && a.path == b.path && a.id == b.id;
}

bool isFinal(SymbolState s) {
  return s == SymbolState::Resolved || s == SymbolState::NotFound || s == SymbolState::Failed;
}

}  // namespace

SymbolService::SymbolService(std::vector<std::string> servers, std::filesystem::path cacheDir)
    : SymbolService(std::move(servers), std::move(cacheDir), nullptr) {}

SymbolService::SymbolService(std::vector<std::string> servers, std::filesystem::path cacheDir,
                             std::shared_ptr<SymbolStringTable> strings)
    : m_servers(std::move(servers)),
      m_cacheDir(std::move(cacheDir)),
      m_locator(m_servers, m_cacheDir),
      m_strings(strings ? std::move(strings) : std::make_shared<SymbolStringTable>()) {
  m_locate = [this](const Module& m, const std::atomic<bool>* cancel) { return m_locator.locate(m, cancel); };
  m_factory = [this](const Module&, const std::filesystem::path& file) -> std::unique_ptr<ISymbolProvider> {
    if (file.extension() == ".json") return std::make_unique<JsonSymbolProvider>(*m_strings);
    return nullptr;
  };
}

SymbolService::~SymbolService() {
  cancel();
  if (m_adopted) {  // adopted but never used: give the pins back
    for (const std::filesystem::path& p : m_adopted->pinned)
      if (!p.empty()) m_locator.unpin(p);
  }
}

void SymbolService::cancel() {
  if (!m_job) return;
  m_job->cancel.store(true, std::memory_order_release);
  for (std::thread& t : m_job->threads) {
    if (t.joinable()) t.join();
  }
  releasePins();
}

void SymbolService::releasePins() {
  if (!m_job) return;
  for (std::filesystem::path& p : m_job->pinned) {
    if (p.empty()) continue;
    m_locator.unpin(p);
    p.clear();
  }
}

void SymbolService::setLocate(LocateFn locate) {
  m_defaultLocate = false;
  if (!locate) {
    m_locate = nullptr;
    return;
  }
  m_locate = [fn = std::move(locate)](const Module& m, const std::atomic<bool>*) { return fn(m); };
}

void SymbolService::setLocate(CancellableLocateFn locate) {
  m_defaultLocate = false;
  m_locate = std::move(locate);
}

void SymbolService::configureLocator(SymbolLocatorConfig config) {
  m_locator = SymbolLocator(std::move(config));
}

void SymbolService::setProviderFactory(ProviderFactory factory) {
  m_factory = std::move(factory);
}

Result<void> SymbolService::addSymbolFileOverride(const std::filesystem::path& symbolsJson) {
  TACHYON_TRY_ASSIGN(std::vector<JsonModuleIdentity> identities,
                     JsonSymbolProvider::readIdentities(symbolsJson));
  for (JsonModuleIdentity& id : identities) m_overrides.push_back(Override{symbolsJson, std::move(id)});
  return ok();
}

size_t SymbolService::adoptRun(SymbolService& previous) {
  if (&previous == this || !previous.m_job || previous.m_strings != m_strings) return 0;
  // Stop the previous run (its workers abandon unfinished modules back to Pending), then take what it
  // finished.
  previous.m_job->cancel.store(true, std::memory_order_release);
  for (std::thread& t : previous.m_job->threads)
    if (t.joinable()) t.join();
  Job& pj = *previous.m_job;
  if (m_adopted) {  // an earlier adoption that was never used
    for (const std::filesystem::path& p : m_adopted->pinned)
      if (!p.empty()) m_locator.unpin(p);
  }
  auto adopted = std::make_unique<Adopted>();
  adopted->modules = pj.modules;
  const size_t n = pj.modules.size();
  adopted->state.resize(n);
  adopted->errors = pj.errors;
  adopted->providers = pj.providers;
  adopted->pinned.resize(n);
  size_t count = 0;
  for (size_t i = 0; i < n; ++i) {
    const SymbolState st = static_cast<SymbolState>(pj.state[i].load(std::memory_order_acquire));
    adopted->state[i] = static_cast<uint8_t>(isFinal(st) ? st : SymbolState::Pending);
    if (!isFinal(st)) continue;
    ++count;
    if (st == SymbolState::Resolved && !pj.pinned[i].empty()) {
      m_locator.pin(pj.pinned[i]);  // this service now holds the file for the adopted provider
      adopted->pinned[i] = pj.pinned[i];
    }
  }
  m_adopted = std::move(adopted);
  previous.releasePins();
  return count;
}

void SymbolService::resolveAllAsync(model::TraceModel& model,
                                    std::function<void(ModuleIdx, SymbolState)> onModule) {
  resolveAllAsync(static_cast<const model::TraceModel&>(model), std::move(onModule));
  m_job->mutableModel = &model;  // workers never read this
}

void SymbolService::resolveAllAsync(const model::TraceModel& model,
                                    std::function<void(ModuleIdx, SymbolState)> onModule) {
  waitAll();
  releasePins();  // the previous run's providers are destroyed with its job below

  auto job = std::make_unique<Job>();
  job->modules = model.modules;
  const size_t moduleCount = job->modules.size();
  job->state.reset(new std::atomic<uint8_t>[moduleCount]);
  for (size_t i = 0; i < moduleCount; ++i) {
    job->state[i].store(static_cast<uint8_t>(SymbolState::Pending), std::memory_order_relaxed);
  }
  job->errors.resize(moduleCount);
  job->providers.resize(moduleCount);
  job->pinned.resize(moduleCount);
  job->adopt.assign(moduleCount, 0);
  job->adoptedState.assign(moduleCount, 0);
  job->memo.resize(model.frames.size());
  job->framesOfModule.resize(moduleCount);
  // A frame is non-leaf when some callstack node referencing it has a child: the child's parent is that node.
  job->callerSlot.assign(model.frames.size(), kNoSlot);
  {
    uint32_t slots = 0;
    for (const model::CallstackNode& n : model.nodes) {
      if (!n.parent.valid() || n.parent.get() == 0 || n.parent.get() >= model.nodes.size()) continue;
      const FrameIdx pf = model.nodes[n.parent.get()].frame;
      if (!pf.valid() || pf.get() >= model.frames.size() || job->callerSlot[pf.get()] != kNoSlot) continue;
      job->callerSlot[pf.get()] = slots++;
    }
    job->memoCaller.resize(slots);
  }
  for (size_t f = 0; f < model.frames.size(); ++f) {
    const ModuleIdx m = model.frames[f].module;
    if (m.valid() && m.get() < moduleCount) job->framesOfModule[m.get()].push_back(static_cast<uint32_t>(f));
  }
  for (size_t m = 0; m < moduleCount; ++m) {
    if (!job->framesOfModule[m].empty()) job->referenced.push_back(static_cast<uint32_t>(m));
  }
  job->onModule = std::move(onModule);

  // Adopt the finished modules of the previous run whose record is identical (adoptRun).
  if (m_adopted) {
    Adopted& a = *m_adopted;
    for (const uint32_t m : job->referenced) {
      if (m >= a.modules.size() || a.state[m] == static_cast<uint8_t>(SymbolState::Pending) ||
          !sameModule(a.modules[m], job->modules[m]))
        continue;
      const SymbolState st = static_cast<SymbolState>(a.state[m]);
      if (st == SymbolState::Resolved) {
        if (!a.providers[m]) continue;
        job->providers[m] = std::move(a.providers[m]);
        job->pinned[m] = std::move(a.pinned[m]);
        a.pinned[m].clear();
        job->adopt[m] = 1;
      } else {
        job->errors[m] = a.errors[m];
        job->adoptedState[m] = a.state[m];
        job->adopt[m] = 2;
      }
    }
    for (const std::filesystem::path& p : a.pinned)  // pins of modules that were not adopted
      if (!p.empty()) m_locator.unpin(p);
    m_adopted.reset();
  }

  // Publish the per-run view for lookup(); no worker is running yet.
  m_frames = model.frames.data();
  m_frameCount = model.frames.size();
  m_moduleCount = moduleCount;
  m_job = std::move(job);

  Job* j = m_job.get();
  unsigned workers =
      m_workerThreads != 0 ? m_workerThreads : std::min(8u, std::thread::hardware_concurrency());
  workers = std::max(1u, std::min<unsigned>(workers, static_cast<unsigned>(j->referenced.size())));
  if (j->referenced.empty()) return;
  try {
    for (unsigned i = 0; i < workers; ++i) j->threads.emplace_back([this, j] { workerLoop(j); });
  } catch (const std::system_error&) {
    // Could not start (all) threads: fall back to doing the remaining work on the calling thread.
    if (j->threads.empty()) workerLoop(j);
  }
}

void SymbolService::workerLoop(Job* job) {
  for (;;) {
    if (job->cancel.load(std::memory_order_relaxed)) return;
    const uint32_t i = job->next.fetch_add(1, std::memory_order_relaxed);
    if (i >= job->referenced.size()) return;
    processModule(*job, job->referenced[i]);
  }
}

void SymbolService::setState(Job& job, uint32_t moduleIdx, SymbolState state, std::string error) {
  if (!error.empty()) job.errors[moduleIdx] = std::move(error);
  job.state[moduleIdx].store(static_cast<uint8_t>(state), std::memory_order_release);
  if (isFinal(state)) job.done.fetch_add(1, std::memory_order_acq_rel);
  if (job.onModule) job.onModule(ModuleIdx(moduleIdx), state);
}

void SymbolService::processModule(Job& job, uint32_t idx) {
  setState(job, idx, SymbolState::Loading, {});
  const Module& module = job.modules[idx];

  if (job.adopt[idx] == 2) {  // adopted failure: not retried
    job.adoptedCount.fetch_add(1, std::memory_order_relaxed);
    setState(job, idx, static_cast<SymbolState>(job.adoptedState[idx]), {});
    return;
  }
  if (job.adopt[idx] == 1) {  // adopted provider: only the new model's frames are resolved through it
    job.adoptedCount.fetch_add(1, std::memory_order_relaxed);
    ISymbolProvider& adopted = *job.providers[idx];
    for (const uint32_t f : job.framesOfModule[idx]) {
      const uint64_t addr = m_frames[f].addr_or_rva;
      job.memo[f] = adopted.resolve(addr);
      if (job.callerSlot[f] != kNoSlot)
        job.memoCaller[job.callerSlot[f]] = adopted.resolve(addr != 0 ? addr - 1 : 0);
    }
    setState(job, idx, SymbolState::Resolved, {});
    return;
  }

  // Overrides first (id match preferred over path match), then the locate function.
  Result<std::filesystem::path> located = makeError(ErrorCode::NotFound, "no symbol file for " + module.path);
  bool found = false;
  for (int pass = 0; pass < 2 && !found; ++pass) {
    for (const Override& o : m_overrides) {
      const bool idPass = module.id.len != 0 && o.identity.id.len != 0;
      if ((pass == 0) == idPass && moduleMatchesIdentity(module, o.identity)) {
        located = o.file;
        found = true;
        break;
      }
    }
  }
  auto abandon = [&] {  // cancelled: back to Pending without a callback
    job.state[idx].store(static_cast<uint8_t>(SymbolState::Pending), std::memory_order_release);
  };
  if (job.cancel.load(std::memory_order_acquire)) return abandon();
  const bool pinnedByLocator = !found && m_locate && m_defaultLocate;
  if (!found && m_locate) located = m_locate(module, &job.cancel);
  if (!located.ok()) {
    if (located.error().code == ErrorCode::Cancelled) return abandon();
    setState(job, idx, SymbolState::NotFound, located.error().toString());
    return;
  }
  // The default locate pins what it returns; keep it pinned while the provider lives (until the run ends).
  if (pinnedByLocator) job.pinned[idx] = located.value();
  auto releaseNow = [&] {
    if (pinnedByLocator) {
      m_locator.unpin(job.pinned[idx]);
      job.pinned[idx].clear();
    }
  };

  std::unique_ptr<ISymbolProvider> provider = m_factory ? m_factory(module, located.value()) : nullptr;
  if (!provider) {
    releaseNow();
    setState(job, idx, SymbolState::Failed, "no symbol provider for " + located.value().string());
    return;
  }
  job.loadedCount.fetch_add(1, std::memory_order_relaxed);
  Result<void> loaded = provider->load(module, located.value());
  if (!loaded.ok()) {
    const SymbolState s =
        loaded.error().code == ErrorCode::NotFound ? SymbolState::NotFound : SymbolState::Failed;
    provider.reset();
    releaseNow();
    setState(job, idx, s, loaded.error().toString());
    return;
  }

  // Memoize every frame of this module before publishing the Resolved state.
  for (const uint32_t f : job.framesOfModule[idx]) {
    const uint64_t addr = m_frames[f].addr_or_rva;
    job.memo[f] = provider->resolve(addr);
    // Return-address rule: a non-leaf frame is a return address; the call is at rva - 1.
    if (job.callerSlot[f] != kNoSlot)
      job.memoCaller[job.callerSlot[f]] = provider->resolve(addr != 0 ? addr - 1 : 0);
  }
  job.providers[idx] = std::move(provider);
  setState(job, idx, SymbolState::Resolved, {});
}

void SymbolService::waitAll() {
  if (!m_job) return;
  for (std::thread& t : m_job->threads) {
    if (t.joinable()) t.join();
  }
  if (m_job->mutableModel) applyStates(*m_job->mutableModel);
}

void SymbolService::waitAll(model::TraceModel& model) {
  if (!m_job) return;
  for (std::thread& t : m_job->threads) {
    if (t.joinable()) t.join();
  }
  applyStates(model);
}

void SymbolService::applyStates(model::TraceModel& model) {
  if (m_job->applied) return;
  m_job->applied = true;
  for (const uint32_t m : m_job->referenced) {
    if (m < model.modules.size()) {
      model.modules[m].state = static_cast<SymbolState>(m_job->state[m].load(std::memory_order_acquire));
    }
  }
}

SymbolProgress SymbolService::progress() const {
  if (!m_job) return {};
  return SymbolProgress{m_job->done.load(std::memory_order_acquire),
                        static_cast<uint32_t>(m_job->referenced.size())};
}

uint32_t SymbolService::modulesAdopted() const {
  return m_job ? m_job->adoptedCount.load(std::memory_order_acquire) : 0;
}

uint32_t SymbolService::modulesLoaded() const {
  return m_job ? m_job->loadedCount.load(std::memory_order_acquire) : 0;
}

SymbolState SymbolService::moduleState(ModuleIdx module) const {
  if (!m_job || !module.valid() || module.get() >= m_moduleCount) return SymbolState::Pending;
  return static_cast<SymbolState>(m_job->state[module.get()].load(std::memory_order_acquire));
}

std::string SymbolService::moduleError(ModuleIdx module) const {
  if (!isFinal(moduleState(module))) return {};
  return m_job->errors[module.get()];
}

const ResolvedAddress& SymbolService::lookup(FrameIdx frameIdx) const {
  if (!m_job || !frameIdx.valid() || frameIdx.get() >= m_frameCount) return unresolvedAddress();
  const ModuleIdx module = m_frames[frameIdx.get()].module;
  if (!module.valid() || module.get() >= m_moduleCount) return unresolvedAddress();
  if (m_job->state[module.get()].load(std::memory_order_acquire) !=
      static_cast<uint8_t>(SymbolState::Resolved)) {
    return unresolvedAddress();
  }
  return m_job->memo[frameIdx.get()];
}

const ResolvedAddress& SymbolService::lookupCaller(FrameIdx frameIdx) const {
  if (!m_job || !frameIdx.valid() || frameIdx.get() >= m_frameCount) return unresolvedAddress();
  const ModuleIdx module = m_frames[frameIdx.get()].module;
  if (!module.valid() || module.get() >= m_moduleCount) return unresolvedAddress();
  if (m_job->state[module.get()].load(std::memory_order_acquire) !=
      static_cast<uint8_t>(SymbolState::Resolved)) {
    return unresolvedAddress();
  }
  const uint32_t slot = m_job->callerSlot[frameIdx.get()];
  return slot == kNoSlot ? unresolvedAddress() : m_job->memoCaller[slot];
}

model::Frame SymbolService::frame(FrameIdx frameIdx) const {
  if (!m_job || !frameIdx.valid() || frameIdx.get() >= m_frameCount) return model::Frame{};
  return m_frames[frameIdx.get()];
}

ISymbolProvider* SymbolService::provider(ModuleIdx module) const {
  if (moduleState(module) != SymbolState::Resolved) return nullptr;
  return m_job->providers[module.get()].get();
}

}  // namespace tachyon::symbols
