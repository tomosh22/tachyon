#include "viewer/live/live_session.h"

#include "analysis/lod.h"
#include "analysis/thread_states.h"
#include "core/log.h"
#include "trace/trace_format.h"
#include "viewer/timeline/timeline_data.h"

#include <algorithm>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace tachyon::viewer {

namespace {
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

}  // namespace

size_t pruneIdleThreads(model::TraceModel& m) {
  const size_t count = m.threads.size();
  std::vector<uint8_t> keep(count, 0);
  std::unordered_set<uint32_t> active;  // tids named by an event of the window
  for (const trace::ContextSwitchRec& cs : m.contextSwitches) {
    active.insert(cs.old_tid);
    active.insert(cs.new_tid);
  }
  for (const trace::ReadyRec& r : m.readyEvents) {
    active.insert(r.readied_tid);
    active.insert(r.readying_tid);
  }
  for (const trace::ScopeEventRec& e : m.scopeEvents) active.insert(e.tid);
  for (const model::FrameMark& f : m.frameMarks)
    if (f.thread.valid() && f.thread.get() < count) keep[f.thread.get()] = 1;
  for (const model::Marker& mk : m.markers)
    if (mk.thread.valid() && mk.thread.get() < count) keep[mk.thread.get()] = 1;
  size_t kept = 0;
  std::vector<uint32_t> newIndex(count, 0xFFFFFFFFu);
  for (size_t i = 0; i < count; ++i) {
    const bool hasSamples = i < m.samplesByThread.size() && !m.samplesByThread[i].ts.empty();
    if (hasSamples || m.threads[i].is_target || active.count(m.threads[i].tid) != 0) keep[i] = 1;
    if (keep[i]) newIndex[i] = static_cast<uint32_t>(kept++);
  }
  if (kept == count) return 0;
  std::vector<model::Thread> threads;
  std::vector<model::SampleColumns> samples;
  threads.reserve(kept);
  samples.reserve(kept);
  for (size_t i = 0; i < count; ++i) {
    if (!keep[i]) continue;
    threads.push_back(std::move(m.threads[i]));
    samples.push_back(i < m.samplesByThread.size() ? std::move(m.samplesByThread[i])
                                                   : model::SampleColumns{});
  }
  m.threads = std::move(threads);
  m.samplesByThread = std::move(samples);
  m.statesByThread.assign(kept, {});
  m.scopesByThread.assign(kept, {});
  for (auto it = m.tidToThread.begin(); it != m.tidToThread.end();) {
    const uint32_t old = it->second.get();
    if (old < count && newIndex[old] != 0xFFFFFFFFu) {
      it->second = ThreadIdx(newIndex[old]);
      ++it;
    } else {
      it = m.tidToThread.erase(it);
    }
  }
  auto remap = [&](ThreadIdx t) {
    return t.valid() && t.get() < count && newIndex[t.get()] != 0xFFFFFFFFu ? ThreadIdx(newIndex[t.get()])
                                                                            : ThreadIdx();
  };
  for (model::FrameMark& f : m.frameMarks) f.thread = remap(f.thread);
  for (model::Marker& mk : m.markers) mk.thread = remap(mk.thread);
  return count - kept;
}

namespace {

uint64_t newestSample(const model::TraceModel& m) {
  uint64_t newest = 0;
  for (const model::SampleColumns& s : m.samplesByThread)
    if (!s.ts.empty()) newest = std::max(newest, s.ts.back());
  return newest;
}

}  // namespace

LiveSession::LiveSession(LiveSessionOptions options) : m_options(std::move(options)) {}

LiveSession::~LiveSession() {
  stopWorker();
}

void LiveSession::stopWorker() {
  {
    std::lock_guard lock(m_mutex);
    m_stop = true;
  }
  m_cancel.store(true);
  m_cv.notify_all();
  if (m_worker.joinable()) m_worker.join();
}

std::shared_ptr<model::LiveModel> LiveSession::begin(LiveSymbolsConfig symbolsConfig) {
  stopWorker();
  model::LiveModelOptions modelOptions;
  modelOptions.windowNs = m_options.windowNs;
  auto model = std::make_shared<model::LiveModel>(modelOptions);
  auto symbols = std::make_shared<LiveSymbols>(std::move(symbolsConfig));
  {
    std::lock_guard lock(m_mutex);
    m_model = model;
    m_symbols = symbols;
    m_mailbox.reset();
    m_pendingSymbols.reset();
    m_stats = {};
    m_stop = false;
  }
  m_cancel.store(false);
  m_ended.store(false);
  m_paused.store(false);
  m_active.store(true);
  m_worker = std::thread([this, model, symbols] { workerMain(model, symbols); });
  return model;
}

void LiveSession::close() {
  stopWorker();
  {
    std::lock_guard lock(m_mutex);
    if (m_symbols) m_stats.symbols = m_symbols->stats();  // the numbers outlive the symbol services
    m_model.reset();
    m_mailbox.reset();
    m_symbols.reset();
  }
  m_active.store(false);
  m_ended.store(false);
  m_paused.store(false);
}

std::shared_ptr<model::LiveModel> LiveSession::model() const {
  std::lock_guard lock(m_mutex);
  return m_model;
}

void LiveSession::notify() {
  m_cv.notify_one();
}

void LiveSession::markEnded() {
  m_ended.store(true);
  m_cv.notify_all();
}

void LiveSession::setPaused(bool paused) {
  m_paused.store(paused);
  m_cv.notify_all();
}

std::shared_ptr<LivePublished> LiveSession::take() {
  if (m_paused.load()) return nullptr;
  std::lock_guard lock(m_mutex);
  if (!m_mailbox) return nullptr;
  ++m_stats.taken;
  return std::move(m_mailbox);
}

bool LiveSession::hasPublished() const {
  if (m_paused.load()) return false;
  std::lock_guard lock(m_mutex);
  return m_mailbox != nullptr;
}

void LiveSession::reconfigureSymbols(LiveSymbolsConfig config) {
  {
    std::lock_guard lock(m_mutex);
    m_pendingSymbols = std::move(config);
  }
  m_cv.notify_all();
}

LiveSessionStats LiveSession::stats() const {
  std::shared_ptr<LiveSymbols> symbols;
  LiveSessionStats s;
  {
    std::lock_guard lock(m_mutex);
    s = m_stats;
    symbols = m_symbols;
  }
  if (symbols) s.symbols = symbols->stats();
  return s;
}

std::optional<uint64_t> LiveSession::localTraceTicks(const model::TraceModel& model) {
#if defined(_WIN32)
  if (model.header.platform != trace::kPlatformWindows) return std::nullopt;
  LARGE_INTEGER freq, now;
  if (!QueryPerformanceFrequency(&freq) ||
      static_cast<uint64_t>(freq.QuadPart) != model.header.clock_frequency_hz)
    return std::nullopt;
  QueryPerformanceCounter(&now);
  return static_cast<uint64_t>(now.QuadPart);
#else
  if (model.header.platform != trace::kPlatformLinux || model.header.clock_frequency_hz != 1'000'000'000ull)
    return std::nullopt;
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) return std::nullopt;
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

void LiveSession::workerMain(std::shared_ptr<model::LiveModel> model,
                             std::shared_ptr<LiveSymbols> symbolsPtr) {
  LiveSymbols& symbols = *symbolsPtr;
  uint64_t lastGeneration = ~0ull;
  bool finalDone = false;
  Clock::time_point lastStart{};
  Clock::time_point lastEnd{};
  uint64_t serial = 0;
  for (;;) {
    bool ended = false;
    std::optional<LiveSymbolsConfig> newSymbols;
    {
      std::unique_lock lock(m_mutex);
      m_cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return m_stop; });
      if (m_stop) break;
      newSymbols = std::move(m_pendingSymbols);
      m_pendingSymbols.reset();
    }
    if (newSymbols) {
      symbols.reset(std::move(*newSymbols));
      lastGeneration = ~0ull;  // snapshot again even if nothing arrived: the symbols changed
      finalDone = false;
    }
    if (m_paused.load()) continue;
    ended = m_ended.load();
    const uint64_t generation = model->generation();
    if (generation == 0) continue;  // nothing received yet
    const bool final = ended && !finalDone;
    if (generation == lastGeneration && !final) continue;
    const auto now = Clock::now();
    if (!final && lastStart != Clock::time_point{} &&
        (now - lastStart < std::chrono::milliseconds(m_options.refreshMs) ||
         now - lastEnd < std::chrono::milliseconds(100)))
      continue;
    if (ended && finalDone) continue;

    if (m_options.beforeSnapshot) m_options.beforeSnapshot();
    lastStart = Clock::now();
    auto bundle = std::make_shared<LivePublished>();
    // 1. The snapshot (blocks the LiveModel's apply() while it copies).
    auto t = Clock::now();
    std::shared_ptr<const model::TraceModel> snap = model->snapshot();
    bundle->snapshotMs = msSince(t);
    const model::LiveModelStats stats = model->stats();
    // 2. Thread states. The snapshot was created by this call and nobody else holds it yet, so it is ours to
    // complete before it is shared.
    t = Clock::now();
    {
      auto mutableModel = std::const_pointer_cast<model::TraceModel>(snap);
      pruneIdleThreads(*mutableModel);
      analysis::BuildOptions options;
      options.threads = m_options.buildThreads;
      const Result<void> states = analysis::buildThreadStates(*mutableModel, options);
      if (!states.ok()) TACHYON_LOG_WARN("live: thread states: {}", states.error().toString());
    }
    bundle->statesMs = msSince(t);
    // 3. Symbols (never blocks on loading).
    t = Clock::now();
    bundle->symbols = symbols.serviceFor(*snap);
    bundle->symbolsMs = msSince(t);
    // 4. The timeline index, so the swap on the UI thread does not show a row without one.
    t = Clock::now();
    if (m_options.buildTimelineIndex) {
      auto lod = std::make_shared<timeline::LodShared>(snap);
      analysis::LodOptions lodOptions;
      lodOptions.threads = m_options.buildThreads;
      lodOptions.maxBytes = m_options.lodMaxBytes;
      timeline::buildLodShared(*lod, lodOptions, &m_cancel, {});
      lod->finished.store(true, std::memory_order_release);
      bundle->lod = std::move(lod);
    }
    bundle->lodMs = msSince(t);
    if (m_cancel.load()) break;

    bundle->model = snap;
    bundle->info.generation = snap->loadStatus.generation;
    bundle->info.nowTs = stats.nowTs;
    bundle->info.newestSampleTs = newestSample(*snap);
    bundle->info.droppedLive = stats.droppedLive;
    bundle->info.samplesInWindow = stats.samplesInWindow;
    bundle->info.ended = ended;
    bundle->serial = ++serial;
    bundle->publishedAt = Clock::now();
    lastGeneration = snap->loadStatus.generation;
    lastEnd = bundle->publishedAt;
    if (ended) finalDone = true;
    const double total = msSince(lastStart);
    {
      std::lock_guard lock(m_mutex);
      m_mailbox = bundle;
      ++m_stats.published;
      m_stats.lastSnapshotMs = bundle->snapshotMs;
      m_stats.lastStatesMs = bundle->statesMs;
      m_stats.lastLodMs = bundle->lodMs;
      m_stats.lastSymbolsMs = bundle->symbolsMs;
      m_stats.lastTotalMs = total;
      m_stats.maxTotalMs = std::max(m_stats.maxTotalMs, total);
    }
    if (m_options.wake) m_options.wake();
  }
}

}  // namespace tachyon::viewer
