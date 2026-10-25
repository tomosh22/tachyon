#include "symbols/symbol_service.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace tachyon::symbols {

struct SymbolService::Job {
  model::TraceModel* model = nullptr;
  std::vector<Module> modules;  // snapshot: workers never read the live model's modules
  std::unique_ptr<std::atomic<uint8_t>[]> state;
  std::vector<std::string> errors;  // written by the owning worker before the final state's release store
  std::vector<std::unique_ptr<ISymbolProvider>> providers;
  std::vector<ResolvedAddress> memo;  // indexed by FrameIdx; slot written before its module's release store
  std::vector<uint32_t> referenced;
  std::vector<std::vector<uint32_t>> framesOfModule;
  std::atomic<uint32_t> next{0};
  std::atomic<uint32_t> done{0};
  std::atomic<bool> cancel{false};
  std::function<void(ModuleIdx, SymbolState)> onModule;
  std::vector<std::thread> threads;
  bool applied = false;
};

namespace {

bool isFinal(SymbolState s) {
  return s == SymbolState::Resolved || s == SymbolState::NotFound || s == SymbolState::Failed;
}

}  // namespace

SymbolService::SymbolService(std::vector<std::string> servers, std::filesystem::path cacheDir)
    : m_servers(std::move(servers)), m_cacheDir(std::move(cacheDir)), m_locator(m_servers, m_cacheDir) {
  m_locate = [this](const Module& m) { return m_locator.locate(m); };
  m_factory = [this](const Module&, const std::filesystem::path& file) -> std::unique_ptr<ISymbolProvider> {
    if (file.extension() == ".json") return std::make_unique<JsonSymbolProvider>(m_strings);
    return nullptr;
  };
}

SymbolService::~SymbolService() {
  if (m_job) {
    m_job->cancel.store(true, std::memory_order_relaxed);
    for (std::thread& t : m_job->threads) {
      if (t.joinable()) t.join();
    }
  }
}

void SymbolService::setLocate(LocateFn locate) {
  m_locate = std::move(locate);
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

void SymbolService::resolveAllAsync(model::TraceModel& model,
                                    std::function<void(ModuleIdx, SymbolState)> onModule) {
  waitAll();

  auto job = std::make_unique<Job>();
  job->model = &model;
  job->modules = model.modules;
  const size_t moduleCount = job->modules.size();
  job->state.reset(new std::atomic<uint8_t>[moduleCount]);
  for (size_t i = 0; i < moduleCount; ++i) {
    job->state[i].store(static_cast<uint8_t>(SymbolState::Pending), std::memory_order_relaxed);
  }
  job->errors.resize(moduleCount);
  job->providers.resize(moduleCount);
  job->memo.resize(model.frames.size());
  job->framesOfModule.resize(moduleCount);
  for (size_t f = 0; f < model.frames.size(); ++f) {
    const ModuleIdx m = model.frames[f].module;
    if (m.valid() && m.get() < moduleCount) job->framesOfModule[m.get()].push_back(static_cast<uint32_t>(f));
  }
  for (size_t m = 0; m < moduleCount; ++m) {
    if (!job->framesOfModule[m].empty()) job->referenced.push_back(static_cast<uint32_t>(m));
  }
  job->onModule = std::move(onModule);

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
  if (!found && m_locate) located = m_locate(module);
  if (!located.ok()) {
    setState(job, idx, SymbolState::NotFound, located.error().toString());
    return;
  }

  std::unique_ptr<ISymbolProvider> provider = m_factory ? m_factory(module, located.value()) : nullptr;
  if (!provider) {
    setState(job, idx, SymbolState::Failed, "no symbol provider for " + located.value().string());
    return;
  }
  Result<void> loaded = provider->load(module, located.value());
  if (!loaded.ok()) {
    const SymbolState s =
        loaded.error().code == ErrorCode::NotFound ? SymbolState::NotFound : SymbolState::Failed;
    setState(job, idx, s, loaded.error().toString());
    return;
  }

  // Memoize every frame of this module before publishing the Resolved state.
  for (const uint32_t f : job.framesOfModule[idx]) job.memo[f] = provider->resolve(m_frames[f].addr_or_rva);
  job.providers[idx] = std::move(provider);
  setState(job, idx, SymbolState::Resolved, {});
}

void SymbolService::waitAll() {
  if (!m_job) return;
  for (std::thread& t : m_job->threads) {
    if (t.joinable()) t.join();
  }
  if (m_job->applied) return;
  m_job->applied = true;
  for (const uint32_t m : m_job->referenced) {
    if (m < m_job->model->modules.size()) {
      m_job->model->modules[m].state =
          static_cast<SymbolState>(m_job->state[m].load(std::memory_order_acquire));
    }
  }
}

SymbolProgress SymbolService::progress() const {
  if (!m_job) return {};
  return SymbolProgress{m_job->done.load(std::memory_order_acquire),
                        static_cast<uint32_t>(m_job->referenced.size())};
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

model::Frame SymbolService::frame(FrameIdx frameIdx) const {
  if (!m_job || !frameIdx.valid() || frameIdx.get() >= m_frameCount) return model::Frame{};
  return m_frames[frameIdx.get()];
}

ISymbolProvider* SymbolService::provider(ModuleIdx module) const {
  if (moduleState(module) != SymbolState::Resolved) return nullptr;
  return m_job->providers[module.get()].get();
}

}  // namespace tachyon::symbols
