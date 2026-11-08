#include "capture/mock/mock_backend.h"

#include "core/platform.h"
#include "model/model.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>
#include <unordered_map>

namespace tachyon::capture {

namespace fs = std::filesystem;

MockBackend::MockBackend(MockBackendConfig config) : m_config(std::move(config)) {}

MockBackend::~MockBackend() {
  stopLive();
}

Result<void> MockBackend::checkPrivileges() {
  return ok();
}

Result<fs::path> MockBackend::resolveFixture() const {
  fs::path fixture = m_config.fixture;
  if (fixture.empty()) {
#if TACHYON_PLATFORM_WINDOWS
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "TACHYON_MOCK_FIXTURE") == 0 && env != nullptr) {
      fixture = env;
      free(env);
    }
#else
    const char* env = std::getenv("TACHYON_MOCK_FIXTURE");
    if (env != nullptr) fixture = env;
#endif
  }
  if (fixture.empty())
    return makeError(ErrorCode::InvalidArgument, "mock backend has no fixture (set TACHYON_MOCK_FIXTURE)");
  std::error_code ec;
  if (!fs::is_regular_file(fixture, ec))
    return makeError(ErrorCode::NotFound, "mock fixture not found: " + fixture.string());
  return fixture;
}

Result<void> MockBackend::start(const CaptureOptions& options) {
  {
    std::lock_guard lock(m_mutex);
    if (m_state == CaptureState::Starting || m_state == CaptureState::Capturing ||
        m_state == CaptureState::Converting)
      return makeError(ErrorCode::Busy, "mock capture already running");
    if (options.outPath.empty()) return makeError(ErrorCode::InvalidArgument, "outPath is required");
    auto fixture = resolveFixture();
    if (!fixture.ok()) return fixture.error();
    if (options.live && !m_sink)
      return makeError(ErrorCode::InvalidArgument, "a live capture needs a live sink (setLiveSink)");
    m_options = options;
    m_startTime = std::chrono::steady_clock::now();
  }
  if (options.live) {
    auto live = startLive(options);
    if (!live.ok()) return live;
  }
  std::lock_guard lock(m_mutex);
  m_state = CaptureState::Capturing;
  return ok();
}

void MockBackend::notify(CaptureState state, float progress) {
  std::function<void(CaptureState, float)> cb;
  {
    std::lock_guard lock(m_mutex);
    cb = m_callback;
  }
  if (cb) cb(state, progress);
}

Result<CaptureResult> MockBackend::stop() {
  CaptureOptions options;
  double durationS = 0.0;
  {
    std::lock_guard lock(m_mutex);
    if (m_state != CaptureState::Capturing)
      return makeError(ErrorCode::InvalidArgument, "mock backend is not capturing");
    m_state = CaptureState::Converting;
    options = m_options;
    durationS = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();
  }
  stopLive();
  const uint32_t steps = m_config.progressSteps == 0 ? 1 : m_config.progressSteps;
  notify(CaptureState::Converting, 0.0f);
  for (uint32_t i = 1; i <= steps; ++i) {
    if (m_config.captureDuration.count() > 0) std::this_thread::sleep_for(m_config.captureDuration / steps);
    if (i < steps) notify(CaptureState::Converting, static_cast<float>(i) / static_cast<float>(steps));
  }

  Result<CaptureResult> result = [&]() -> Result<CaptureResult> {
    auto fixture = resolveFixture();
    if (!fixture.ok()) return fixture.error();
    std::error_code ec;
    if (options.outPath.has_parent_path()) fs::create_directories(options.outPath.parent_path(), ec);
    fs::copy_file(*fixture, options.outPath, fs::copy_options::overwrite_existing, ec);
    if (ec) return makeError(ErrorCode::IoError, "copy to " + options.outPath.string() + ": " + ec.message());
    CaptureResult r;
    r.tracePath = options.outPath;
    r.durationS = durationS;
    return r;
  }();

  {
    std::lock_guard lock(m_mutex);
    m_state = result.ok() ? CaptureState::Done : CaptureState::Idle;
  }
  if (result.ok()) notify(CaptureState::Done, 1.0f);
  return result;
}

Result<CaptureResult> MockBackend::flush(std::string name, uint32_t seconds) {
  (void)seconds;
  CaptureOptions options;
  double durationS = 0.0;
  {
    std::lock_guard lock(m_mutex);
    if (m_state != CaptureState::Capturing)
      return makeError(ErrorCode::InvalidArgument, "mock backend is not capturing");
    if (m_options.circularMb == 0)
      return makeError(ErrorCode::InvalidArgument, "flush requires circular mode (circularMb > 0)");
    options = m_options;
    durationS = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_startTime).count();
  }
  if (name.empty() || name.find_first_of("/\\") != std::string::npos || name == "." || name == "..")
    return makeError(ErrorCode::InvalidArgument, "invalid flush name");
  auto fixture = resolveFixture();
  if (!fixture.ok()) return fixture.error();
  const fs::path out = options.outPath.parent_path() / (name + ".tachyon");
  std::error_code ec;
  fs::copy_file(*fixture, out, fs::copy_options::overwrite_existing, ec);
  if (ec) return makeError(ErrorCode::IoError, "copy to " + out.string() + ": " + ec.message());
  CaptureResult r;
  r.tracePath = out;
  r.durationS = durationS;
  return r;
}

CaptureState MockBackend::state() const {
  std::lock_guard lock(m_mutex);
  return m_state;
}

void MockBackend::setProgressCallback(std::function<void(CaptureState, float)> callback) {
  std::lock_guard lock(m_mutex);
  m_callback = std::move(callback);
}

// ---- Live source (WP-65a) ----

struct MockBackend::LiveReplay {
  enum Kind : uint8_t { kSample, kSwitch, kReady, kScope, kFrameMark, kMarker, kPlot };
  struct Item {
    uint64_t ts;
    uint8_t kind;
    uint32_t a;  // thread / series index
    uint32_t b;  // index inside it
  };

  std::shared_ptr<ILiveSink> sink;
  model::TraceModel model;
  std::vector<Item> items;
  size_t cursor = 0;
  uint64_t startTs = 0, endTs = 0;
  uint64_t clockHz = 1'000'000'000ull;
  std::unordered_map<uint32_t, uint32_t> pidOfTid;
  std::mutex emitMutex;  // one caller at a time into the sink
  std::vector<uint64_t> ips;

  std::thread thread;
  std::atomic<bool> stop{false};

  // The addresses of a callstack, leaf first: module base + RVA, or the absolute address.
  void stackIps(CallstackIdx leaf) {
    ips.clear();
    for (CallstackIdx n = leaf; n.valid() && n.get() != 0 && n.get() < model.nodes.size() && ips.size() < 256;
         n = model.nodes[n.get()].parent) {
      const model::Frame& f = model.frames[model.nodes[n.get()].frame.get()];
      if (f.module.valid() && f.module.get() < model.modules.size())
        ips.push_back(model.modules[f.module.get()].base + f.addr_or_rva);
      else
        ips.push_back(f.addr_or_rva);
    }
  }
  uint32_t pidOf(uint32_t tid) const {
    auto it = pidOfTid.find(tid);
    return it == pidOfTid.end() ? 0 : it->second;
  }

  uint64_t emitUntil(uint64_t untilTs) {
    std::lock_guard lock(emitMutex);
    uint64_t emitted = 0;
    for (; cursor < items.size() && items[cursor].ts <= untilTs; ++cursor, ++emitted) {
      const Item& it = items[cursor];
      switch (it.kind) {
        case kSample: {
          const model::Thread& t = model.threads[it.a];
          const model::SampleColumns& c = model.samplesByThread[it.a];
          stackIps(c.callstack[it.b]);
          sink->onSample(c.ts[it.b], t.pid, t.tid, c.cpu[it.b], c.flags[it.b], ips);
          break;
        }
        case kSwitch: {
          const trace::ContextSwitchRec r = model.contextSwitches[it.b];
          LiveContextSwitch cs;
          cs.ts = r.ts;
          cs.cpu = r.cpu;
          cs.oldTid = r.old_tid;
          cs.oldPid = pidOf(r.old_tid);
          cs.newTid = r.new_tid;
          cs.newPid = pidOf(r.new_tid);
          cs.oldState = r.old_state;
          cs.oldWaitReason = r.old_wait_reason;
          cs.oldWaitMode = r.old_wait_mode;
          cs.newPriority = r.new_priority;
          cs.flags = r.reserved;
          if (r.old_thread_stack.valid()) {
            stackIps(r.old_thread_stack);
            cs.oldStack = ips;
          }
          sink->onContextSwitch(cs);
          break;
        }
        case kReady: {
          const trace::ReadyRec r = model.readyEvents[it.b];
          LiveReady rd;
          rd.ts = r.ts;
          rd.cpu = r.cpu;
          rd.readiedTid = r.readied_tid;
          rd.readiedPid = pidOf(r.readied_tid);
          rd.readyingTid = r.readying_tid;
          rd.readyingPid = pidOf(r.readying_tid);
          rd.adjustReason = r.adjust_reason;
          rd.flags = r.flags;
          if (r.readying_stack.valid()) {
            stackIps(r.readying_stack);
            rd.readyingStack = ips;
          }
          sink->onReady(rd);
          break;
        }
        case kScope: {
          const trace::ScopeEventRec r = model.scopeEvents[it.b];
          LiveScopeEvent e;
          e.ts = r.ts;
          e.tid = r.tid;
          e.pid = pidOf(r.tid);
          e.kind = r.kind;
          e.id = model.strings.get(r.id);
          e.data = model.strings.get(r.data);
          e.colorRgba = r.color_rgba;
          e.callerAddr = r.caller_addr;
          sink->onScopeEvent(e);
          break;
        }
        case kFrameMark: {
          const model::FrameMark& r = model.frameMarks[it.b];
          const uint32_t tid = r.thread.valid() ? model.threads[r.thread.get()].tid : 0;
          sink->onFrameMark({r.ts, pidOf(tid), tid, model.strings.get(r.name), r.frame_number});
          break;
        }
        case kMarker: {
          const model::Marker& r = model.markers[it.b];
          const uint32_t tid = r.thread.valid() ? model.threads[r.thread.get()].tid : 0;
          sink->onMarker({r.ts, pidOf(tid), tid, model.strings.get(r.name), model.strings.get(r.data), r.color});
          break;
        }
        case kPlot: {
          const model::PlotSeries& s = model.plots[it.a];
          sink->onPlot({s.ts[it.b], model.strings.get(s.name), s.value[it.b]});
          break;
        }
      }
    }
    sink->onTimeAdvance(untilTs);
    return emitted;
  }
};

void MockBackend::setLiveSink(std::shared_ptr<ILiveSink> sink) {
  std::lock_guard lock(m_mutex);
  m_sink = std::move(sink);
}

Result<void> MockBackend::startLive(const CaptureOptions&) {
  fs::path source = m_config.live.source;
  if (source.empty()) {
    auto fixture = resolveFixture();
    if (!fixture.ok()) return fixture.error();
    source = *fixture;
  }
  auto loaded = model::TraceModel::load(source);
  if (!loaded.ok()) return makeError(loaded.error().code, "mock live source: " + loaded.error().message);
  auto replay = std::make_unique<LiveReplay>();
  {
    std::lock_guard lock(m_mutex);
    replay->sink = m_sink;
  }
  replay->model = std::move(*loaded);
  model::TraceModel& m = replay->model;
  replay->startTs = m.header.start_ts;
  replay->endTs = m.header.end_ts;
  if (m.header.clock_frequency_hz != 0) replay->clockHz = m.header.clock_frequency_hz;
  for (const model::Thread& t : m.threads) replay->pidOfTid.emplace(t.tid, t.pid);

  // The script: every event in timestamp order (stable: the trace's order breaks ties).
  auto& items = replay->items;
  for (size_t t = 0; t < m.samplesByThread.size(); ++t)
    for (size_t i = 0; i < m.samplesByThread[t].ts.size(); ++i)
      items.push_back({m.samplesByThread[t].ts[i], LiveReplay::kSample, static_cast<uint32_t>(t), static_cast<uint32_t>(i)});
  auto tsOf = [](const auto& rec) {
    uint64_t ts;
    std::memcpy(&ts, &rec, sizeof ts);
    return ts;
  };
  for (size_t i = 0; i < m.contextSwitches.size(); ++i)
    items.push_back({tsOf(m.contextSwitches[i]), LiveReplay::kSwitch, 0, static_cast<uint32_t>(i)});
  for (size_t i = 0; i < m.readyEvents.size(); ++i)
    items.push_back({tsOf(m.readyEvents[i]), LiveReplay::kReady, 0, static_cast<uint32_t>(i)});
  for (size_t i = 0; i < m.scopeEvents.size(); ++i)
    items.push_back({tsOf(m.scopeEvents[i]), LiveReplay::kScope, 0, static_cast<uint32_t>(i)});
  for (size_t i = 0; i < m.frameMarks.size(); ++i)
    items.push_back({m.frameMarks[i].ts, LiveReplay::kFrameMark, 0, static_cast<uint32_t>(i)});
  for (size_t i = 0; i < m.markers.size(); ++i)
    items.push_back({m.markers[i].ts, LiveReplay::kMarker, 0, static_cast<uint32_t>(i)});
  for (size_t s = 0; s < m.plots.size(); ++s)
    for (size_t i = 0; i < m.plots[s].ts.size(); ++i)
      items.push_back({m.plots[s].ts[i], LiveReplay::kPlot, static_cast<uint32_t>(s), static_cast<uint32_t>(i)});
  std::stable_sort(items.begin(), items.end(),
                   [](const LiveReplay::Item& a, const LiveReplay::Item& b) { return a.ts < b.ts; });

  // Session facts and the tables go out before any event.
  ILiveSink& sink = *replay->sink;
  LiveSessionInfo info;
  info.clockFrequencyHz = replay->clockHz;
  info.platform = m.header.platform;
  info.samplingHz = static_cast<uint32_t>(m.captureInfo.sampling_hz);
  info.startTs = replay->startTs;
  sink.onSessionInfo(info);
  for (const model::Process& p : m.processes) {
    LiveProcess lp;
    lp.pid = p.pid;
    lp.ppid = p.parent_pid;
    lp.startTs = p.start_ts;
    lp.name = p.name;
    lp.commandLine = p.command_line;
    sink.onProcess(lp);
  }
  for (const model::Module& mod : m.modules) {
    LiveModule lm;
    lm.pid = mod.pid;
    lm.base = mod.base;
    lm.size = mod.size;
    lm.path = mod.path;
    lm.idKind = mod.id.kind;
    lm.idLen = mod.id.len;
    std::memcpy(lm.id.data(), mod.id.bytes.data(), 32);
    lm.age = mod.id.age;
    lm.loadTs = mod.load_ts;
    sink.onModule(lm);
  }
  for (const model::Thread& t : m.threads) {
    LiveThread lt;
    lt.tid = t.tid;
    lt.pid = t.pid;
    lt.startTs = t.first_ts;
    lt.name = t.name;
    lt.isTarget = t.is_target;
    sink.onThread(lt);
  }

  LiveReplay* raw = replay.get();
  const MockLiveConfig cfg = m_config.live;
  {
    std::lock_guard lock(m_mutex);
    m_replay = std::move(replay);
  }
  if (cfg.speed > 0.0) {
    raw->thread = std::thread([raw, cfg] {
      const auto t0 = std::chrono::steady_clock::now();
      while (!raw->stop.load()) {
        std::this_thread::sleep_for(cfg.tick);
        const double elapsedNs =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        const double ticks = elapsedNs * cfg.speed * static_cast<double>(raw->clockHz) / 1e9;
        raw->emitUntil(raw->startTs + static_cast<uint64_t>(ticks));
      }
    });
  }
  return ok();
}

void MockBackend::stopLive() {
  std::unique_ptr<LiveReplay> replay;
  {
    std::lock_guard lock(m_mutex);
    replay = std::move(m_replay);
  }
  if (!replay) return;
  replay->stop.store(true);
  if (replay->thread.joinable()) replay->thread.join();
}

uint64_t MockBackend::pumpLive(uint64_t untilTs) {
  LiveReplay* replay = nullptr;
  {
    std::lock_guard lock(m_mutex);
    replay = m_replay.get();
  }
  return replay == nullptr ? 0 : replay->emitUntil(untilTs);
}

uint64_t MockBackend::liveStartTs() const {
  std::lock_guard lock(m_mutex);
  return m_replay ? m_replay->startTs : 0;
}

uint64_t MockBackend::liveEndTs() const {
  std::lock_guard lock(m_mutex);
  return m_replay ? m_replay->endTs : 0;
}

}  // namespace tachyon::capture
