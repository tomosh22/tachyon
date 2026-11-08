#include "model/live_model.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <numeric>
#include <unordered_map>

namespace tachyon::model {

namespace {

using live_wire::RecordType;

template <typename T>
uint64_t tsOf(const T& rec) {
  uint64_t ts;
  std::memcpy(&ts, &rec, sizeof ts);
  return ts;
}

// Aligned local copy of a packed record (see the ALIGNMENT RULE in docs/specs/trace-format.md): a member of a
// packed struct that sits at an arbitrary address must not have a method called on it or a reference bound to
// it, so records are read through this holder, whose storage is 8-aligned.
template <typename T>
struct Aligned {
  alignas(8) T v;
  explicit Aligned(const std::byte* p) { std::memcpy(static_cast<void*>(&v), p, sizeof(T)); }
  explicit Aligned(const T& src) {
    std::memcpy(static_cast<void*>(&v), static_cast<const void*>(&src), sizeof(T));
  }
};

template <typename T>
size_t vecBytes(const std::vector<T>& v) {
  return v.capacity() * sizeof(T);
}

// Events of one kind, kept sorted by timestamp. head: index of the first record inside the window.
template <typename T>
struct EventStore {
  std::vector<T> v;
  size_t head = 0;

  size_t size() const { return v.size() - head; }
  void clear() {
    v.clear();
    head = 0;
  }
  void add(const T& r) {
    const uint64_t t = tsOf(r);
    if (v.size() == head || t >= tsOf(v.back())) {
      v.push_back(r);
      return;
    }
    // Rings of different cpus are drained at different moments: a record can arrive a little late.
    auto it = std::upper_bound(v.begin() + static_cast<std::ptrdiff_t>(head), v.end(), t,
                               [](uint64_t ts, const T& x) { return ts < tsOf(x); });
    v.insert(it, r);
  }
  void trim(uint64_t cutoff) {
    auto it = std::lower_bound(v.begin() + static_cast<std::ptrdiff_t>(head), v.end(), cutoff,
                               [](const T& x, uint64_t ts) { return tsOf(x) < ts; });
    head = static_cast<size_t>(it - v.begin());
    if (head > 4096 && head > v.size() / 2) {
      v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(head));
      head = 0;
    }
  }
};

struct SampleStore {
  std::vector<uint64_t> ts;
  std::vector<CallstackIdx> node;
  std::vector<uint16_t> cpu, flags;
  size_t head = 0;

  size_t size() const { return ts.size() - head; }
  void clear() {
    ts.clear();
    node.clear();
    cpu.clear();
    flags.clear();
    head = 0;
  }
  void add(uint64_t t, CallstackIdx n, uint16_t c, uint16_t f) {
    if (ts.size() == head || t >= ts.back()) {
      ts.push_back(t);
      node.push_back(n);
      cpu.push_back(c);
      flags.push_back(f);
      return;
    }
    const size_t pos = static_cast<size_t>(
        std::upper_bound(ts.begin() + static_cast<std::ptrdiff_t>(head), ts.end(), t) - ts.begin());
    const auto at = static_cast<std::ptrdiff_t>(pos);
    ts.insert(ts.begin() + at, t);
    node.insert(node.begin() + at, n);
    cpu.insert(cpu.begin() + at, c);
    flags.insert(flags.begin() + at, f);
  }
  void trim(uint64_t cutoff) {
    head = static_cast<size_t>(
        std::lower_bound(ts.begin() + static_cast<std::ptrdiff_t>(head), ts.end(), cutoff) - ts.begin());
    if (head > 4096 && head > ts.size() / 2) {
      const auto h = static_cast<std::ptrdiff_t>(head);
      ts.erase(ts.begin(), ts.begin() + h);
      node.erase(node.begin(), node.begin() + h);
      cpu.erase(cpu.begin(), cpu.begin() + h);
      flags.erase(flags.begin(), flags.begin() + h);
      head = 0;
    }
  }
};

struct ThreadSlot {
  Thread t;
  bool defined = false;
};

}  // namespace

struct LiveModel::Impl {
  LiveModelOptions options;
  mutable std::mutex mutex;
  std::atomic<uint64_t> generation{0};

  // Stream facts.
  uint64_t clockHz = 0, startTs = 0;
  uint32_t platform = 0, samplingHz = 0;
  bool haveInfo = false;

  // Tables (kept until the next keyframe).
  trace::StringTable strings;
  uint64_t stringBytes = 0;
  std::vector<Process> processes;
  std::vector<ThreadSlot> threads;
  std::vector<Module> modules;
  std::vector<Frame> frames;
  std::vector<CallstackNode> nodes;

  // Window events.
  std::vector<SampleStore> samples;  // by thread idx
  EventStore<trace::ContextSwitchRec> switches;
  EventStore<trace::ReadyRec> readies;
  EventStore<trace::ScopeEventRec> scopes;
  EventStore<trace::FrameMarkRec> frameMarks;
  EventStore<trace::MarkerRec> markers;
  EventStore<trace::PlotSampleRec> plots;

  uint64_t now = 0;
  uint64_t lastTrimNow = 0;
  LiveModelStats stats;
  std::vector<std::byte> stringScratch;

  Impl() { reset(); }

  uint64_t windowTicks() const {
    const uint64_t hz = clockHz != 0 ? clockHz : 1'000'000'000ull;
    return static_cast<uint64_t>(static_cast<double>(options.windowNs) * static_cast<double>(hz) / 1e9);
  }

  void reset() {
    clockHz = startTs = 0;
    platform = samplingHz = 0;
    haveInfo = false;
    strings = trace::StringTable();
    stringBytes = 0;
    processes.clear();
    threads.clear();
    modules.clear();
    frames.clear();
    nodes.clear();
    nodes.emplace_back();  // the root
    samples.clear();
    switches.clear();
    readies.clear();
    scopes.clear();
    frameMarks.clear();
    markers.clear();
    plots.clear();
    now = lastTrimNow = 0;
    const uint64_t batches = stats.batches, keyframes = stats.keyframes;
    stats = LiveModelStats{};
    stats.batches = batches;
    stats.keyframes = keyframes;
  }

  void defineString(uint32_t idx, std::string_view s) {
    if (idx < strings.size()) return;  // strings are immutable
    stringScratch.clear();
    auto put = [&](std::string_view v) {
      const auto len = static_cast<uint32_t>(v.size());
      const auto* lp = reinterpret_cast<const std::byte*>(&len);
      stringScratch.insert(stringScratch.end(), lp, lp + 4);
      const auto* bp = reinterpret_cast<const std::byte*>(v.data());
      stringScratch.insert(stringScratch.end(), bp, bp + v.size());
    };
    uint64_t count = 0;
    for (size_t i = strings.size(); i < idx; ++i, ++count) put({});  // gap: undefined strings read as ""
    put(s);
    ++count;
    stringBytes += s.size();
    (void)strings.deserializeAppend(stringScratch, count);
  }

  std::string str(StringIdx i) const { return std::string(strings.get(i)); }

  CallstackIdx checkNode(CallstackIdx n) const {
    return n.valid() && n.get() >= nodes.size() ? CallstackIdx::invalid() : n;
  }

  void noteTs(uint64_t ts) {
    if (ts > now) now = ts;
  }

  void trim() {
    const uint64_t window = windowTicks();
    if (now <= window) return;
    const uint64_t cutoff = now - window;
    if (cutoff == lastTrimNow) return;
    lastTrimNow = cutoff;
    for (SampleStore& s : samples) s.trim(cutoff);
    switches.trim(cutoff);
    readies.trim(cutoff);
    scopes.trim(cutoff);
    frameMarks.trim(cutoff);
    markers.trim(cutoff);
    plots.trim(cutoff);
  }

  Result<void> applyLocked(std::span<const std::byte> data, bool& applied);
  std::shared_ptr<const TraceModel> buildSnapshot() const;
  uint64_t residentBytes() const;
};

namespace {

Error corrupt(const std::string& what) {
  return makeError(ErrorCode::CorruptData, "live batch: " + what);
}

}  // namespace

Result<void> LiveModel::Impl::applyLocked(std::span<const std::byte> data, bool& applied) {
  using namespace live_wire;
  size_t pos = 0;
  while (pos < data.size()) {
    if (data.size() - pos < sizeof(RecordHeader)) return corrupt("truncated record header");
    const Aligned<RecordHeader> ha(data.data() + pos);
    const RecordHeader& h = ha.v;
    pos += sizeof(RecordHeader);
    if (h.byte_len > data.size() - pos) return corrupt("record runs past the end of the batch");
    const std::byte* p = data.data() + pos;
    const size_t n = h.byte_len;
    pos += n;
    applied = true;

    auto u32At = [&](size_t off) {
      uint32_t v;
      std::memcpy(&v, p + off, 4);
      return v;
    };
    switch (static_cast<RecordType>(h.type)) {
      case RecordType::StringDef: {
        if (n < 4) return corrupt("short StringDef");
        const uint32_t idx = u32At(0);
        if (idx >= kMaxStrings) return corrupt("string index over the cap");
        if (stringBytes + (n - 4) > kMaxStringBytes) return corrupt("string table over the cap");
        defineString(idx, std::string_view(reinterpret_cast<const char*>(p + 4), n - 4));
        break;
      }
      case RecordType::ProcessDef: {
        if (n != 4 + sizeof(trace::ProcessRec)) return corrupt("bad ProcessDef size");
        const uint32_t idx = u32At(0);
        if (idx >= kMaxProcesses) return corrupt("process index over the cap");
        const Aligned<trace::ProcessRec> ra(p + 4);
        const auto& r = ra.v;
        if (idx >= processes.size()) processes.resize(idx + 1);
        Process& pr = processes[idx];
        pr.pid = r.pid;
        pr.parent_pid = r.parent_pid;
        pr.start_ts = r.start_ts;
        pr.end_ts = r.end_ts;
        pr.name = str(r.name);
        pr.command_line = str(r.command_line);
        noteTs(r.start_ts);
        break;
      }
      case RecordType::ThreadDef: {
        if (n != 4 + sizeof(trace::ThreadRec)) return corrupt("bad ThreadDef size");
        const uint32_t idx = u32At(0);
        if (idx >= kMaxThreads) return corrupt("thread index over the cap");
        const Aligned<trace::ThreadRec> ra(p + 4);
        const auto& r = ra.v;
        if (idx >= threads.size()) threads.resize(idx + 1);
        if (idx >= samples.size()) samples.resize(idx + 1);
        ThreadSlot& slot = threads[idx];
        slot.defined = true;
        slot.t.tid = r.tid;
        slot.t.pid = r.pid;
        slot.t.first_ts = r.start_ts;
        slot.t.last_ts = r.end_ts;
        slot.t.is_target = (r.flags & trace::kThreadFlagTargetProcess) != 0;
        slot.t.name = str(r.name);
        break;
      }
      case RecordType::ThreadName: {
        if (n != sizeof(ThreadNamePayload)) return corrupt("bad ThreadName size");
        const uint32_t thread = u32At(0), name = u32At(4);
        if (thread < threads.size()) threads[thread].t.name = str(StringIdx(name));
        break;
      }
      case RecordType::ModuleDef: {
        if (n != 4 + sizeof(trace::ModuleRec)) return corrupt("bad ModuleDef size");
        const uint32_t idx = u32At(0);
        if (idx >= kMaxModules) return corrupt("module index over the cap");
        const Aligned<trace::ModuleRec> ra(p + 4);
        const auto& r = ra.v;
        if (idx >= modules.size()) modules.resize(idx + 1);
        Module& m = modules[idx];
        m.pid = r.pid;
        m.base = r.base;
        m.size = r.size;
        m.path = str(r.path);
        m.id.kind = r.id_kind;
        m.id.len = std::min<uint8_t>(r.id_len, 32);
        std::memcpy(m.id.bytes.data(), r.id, 32);
        m.id.age = r.age;
        m.load_ts = r.load_ts;
        m.unload_ts = r.unload_ts;
        break;
      }
      case RecordType::FrameDef: {
        if (n != 4 + sizeof(trace::FrameRec)) return corrupt("bad FrameDef size");
        const uint32_t idx = u32At(0);
        if (idx >= kMaxFrames) return corrupt("frame index over the cap");
        const Aligned<trace::FrameRec> ra(p + 4);
        const auto& r = ra.v;
        if (idx >= frames.size()) frames.resize(idx + 1);
        frames[idx] = Frame{r.module, r.addr_or_rva};
        break;
      }
      case RecordType::NodeDef: {
        if (n != 4 + sizeof(trace::CallstackNodeRec)) return corrupt("bad NodeDef size");
        const uint32_t idx = u32At(0);
        if (idx >= kMaxNodes) return corrupt("node index over the cap");
        const Aligned<trace::CallstackNodeRec> ra(p + 4);
        const auto& r = ra.v;
        if (idx == 0) break;  // the root is implicit
        if (r.parent.valid() && r.parent.get() >= idx) return corrupt("node parent does not precede it");
        if (!r.parent.valid()) return corrupt("non-root node without a parent");
        if (idx >= nodes.size()) nodes.resize(idx + 1);
        nodes[idx] = CallstackNode{r.parent, r.frame};
        break;
      }
      case RecordType::Samples: {
        if (n < 4) return corrupt("short Samples");
        const uint32_t count = u32At(0);
        if (n != 4 + static_cast<size_t>(count) * 20) return corrupt("bad Samples size");
        const std::byte* tsCol = p + 4;
        const std::byte* thCol = tsCol + static_cast<size_t>(count) * 8;
        const std::byte* nodeCol = thCol + static_cast<size_t>(count) * 4;
        const std::byte* cpuCol = nodeCol + static_cast<size_t>(count) * 4;
        const std::byte* flagCol = cpuCol + static_cast<size_t>(count) * 2;
        uint64_t maxTs = 0;
        for (uint32_t i = 0; i < count; ++i) {
          uint64_t ts;
          uint32_t th, nd;
          uint16_t cpu, fl;
          std::memcpy(&ts, tsCol + i * 8, 8);
          std::memcpy(&th, thCol + i * 4, 4);
          std::memcpy(&nd, nodeCol + i * 4, 4);
          std::memcpy(&cpu, cpuCol + i * 2, 2);
          std::memcpy(&fl, flagCol + i * 2, 2);
          if (th >= samples.size() || !threads[th].defined) continue;  // a sample of an unknown thread
          samples[th].add(ts, checkNode(CallstackIdx(nd)), cpu, fl);
          maxTs = std::max(maxTs, ts);
        }
        noteTs(maxTs);
        break;
      }
      case RecordType::ContextSwitches: {
        if (n % sizeof(trace::ContextSwitchRec) != 0) return corrupt("bad ContextSwitches size");
        for (size_t off = 0; off < n; off += sizeof(trace::ContextSwitchRec)) {
          Aligned<trace::ContextSwitchRec> ra(p + off);
          auto& r = ra.v;
          r.old_thread_stack = checkNode(r.old_thread_stack);
          switches.add(r);
          noteTs(r.ts);
        }
        break;
      }
      case RecordType::Ready: {
        if (n % sizeof(trace::ReadyRec) != 0) return corrupt("bad Ready size");
        for (size_t off = 0; off < n; off += sizeof(trace::ReadyRec)) {
          Aligned<trace::ReadyRec> ra(p + off);
          auto& r = ra.v;
          r.readying_stack = checkNode(r.readying_stack);
          readies.add(r);
          noteTs(r.ts);
        }
        break;
      }
      case RecordType::Scopes: {
        if (n % sizeof(trace::ScopeEventRec) != 0) return corrupt("bad Scopes size");
        for (size_t off = 0; off < n; off += sizeof(trace::ScopeEventRec)) {
          const Aligned<trace::ScopeEventRec> ra(p + off);
          const auto& r = ra.v;
          scopes.add(r);
          noteTs(r.ts);
        }
        break;
      }
      case RecordType::FrameMarks: {
        if (n % sizeof(trace::FrameMarkRec) != 0) return corrupt("bad FrameMarks size");
        for (size_t off = 0; off < n; off += sizeof(trace::FrameMarkRec)) {
          const Aligned<trace::FrameMarkRec> ra(p + off);
          const auto& r = ra.v;
          frameMarks.add(r);
          noteTs(r.ts);
        }
        break;
      }
      case RecordType::Markers: {
        if (n % sizeof(trace::MarkerRec) != 0) return corrupt("bad Markers size");
        for (size_t off = 0; off < n; off += sizeof(trace::MarkerRec)) {
          const Aligned<trace::MarkerRec> ra(p + off);
          const auto& r = ra.v;
          markers.add(r);
          noteTs(r.ts);
        }
        break;
      }
      case RecordType::Plots: {
        if (n % sizeof(trace::PlotSampleRec) != 0) return corrupt("bad Plots size");
        for (size_t off = 0; off < n; off += sizeof(trace::PlotSampleRec)) {
          const Aligned<trace::PlotSampleRec> ra(p + off);
          const auto& r = ra.v;
          plots.add(r);
          noteTs(r.ts);
        }
        break;
      }
      case RecordType::TimeAdvance: {
        if (n != 8) return corrupt("bad TimeAdvance size");
        uint64_t ts;
        std::memcpy(&ts, p, 8);
        noteTs(ts);
        break;
      }
      case RecordType::Dropped: {
        if (n != 8) return corrupt("bad Dropped size");
        uint64_t events;
        std::memcpy(&events, p, 8);
        stats.droppedLive += events;
        break;
      }
      case RecordType::Info: {
        if (n < sizeof(InfoPayload)) return corrupt("short Info");
        const Aligned<InfoPayload> ra(p);
        const auto& r = ra.v;
        clockHz = r.clock_frequency_hz;
        platform = r.platform;
        samplingHz = r.sampling_hz;
        startTs = r.start_ts;
        haveInfo = true;
        noteTs(r.start_ts);
        break;
      }
      default: ++stats.unknownRecords; break;
    }
  }
  return ok();
}

uint64_t LiveModel::Impl::residentBytes() const {
  uint64_t total = 0;
  total += vecBytes(processes) + vecBytes(threads) + vecBytes(modules) + vecBytes(frames) + vecBytes(nodes);
  total += stringBytes + strings.size() * 16;
  for (const SampleStore& s : samples)
    total += vecBytes(s.ts) + vecBytes(s.node) + vecBytes(s.cpu) + vecBytes(s.flags);
  total += vecBytes(samples);
  total += vecBytes(switches.v) + vecBytes(readies.v) + vecBytes(scopes.v) + vecBytes(frameMarks.v) +
           vecBytes(markers.v) + vecBytes(plots.v);
  return total;
}

LiveModel::LiveModel(LiveModelOptions options) : m_impl(std::make_unique<Impl>()) {
  m_impl->options = options;
}
LiveModel::~LiveModel() = default;

Result<void> LiveModel::apply(std::span<const std::byte> data, bool keyframe) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (keyframe) {
    m.reset();
    ++m.stats.keyframes;
  }
  bool applied = false;
  auto result = m.applyLocked(data, applied);
  if (applied || keyframe) {
    m.trim();
    ++m.stats.batches;
    m.generation.fetch_add(1, std::memory_order_release);
  }
  return result;
}

uint64_t LiveModel::generation() const {
  return m_impl->generation.load(std::memory_order_acquire);
}

uint64_t LiveModel::clockFrequencyHz() const {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->clockHz;
}

LiveModelStats LiveModel::stats() const {
  std::lock_guard lock(m_impl->mutex);
  const Impl& m = *m_impl;
  LiveModelStats s = m.stats;
  s.generation = m.generation.load();
  s.nowTs = m.now;
  const uint64_t window = m.windowTicks();
  s.windowStartTs = m.now > window ? m.now - window : 0;
  for (const SampleStore& st : m.samples) s.samplesInWindow += st.size();
  s.residentBytes = m.residentBytes();
  return s;
}

std::shared_ptr<const TraceModel> LiveModel::snapshot() const {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->buildSnapshot();
}

std::shared_ptr<const TraceModel> LiveModel::Impl::buildSnapshot() const {
  auto out = std::make_shared<TraceModel>();
  TraceModel& m = *out;

  const uint64_t window = windowTicks();
  const uint64_t windowStart = now > window ? now - window : 0;
  uint64_t start = std::max(windowStart, haveInfo && startTs <= now ? startTs : 0);
  uint64_t end = now;
  if (end <= start) end = start + 1;

  m.header = trace::FileHeader{};
  m.header.flags = trace::kFlagPartial;
  m.header.clock_frequency_hz = clockHz != 0 ? clockHz : 1'000'000'000ull;
  m.header.start_ts = start;
  m.header.end_ts = end;
  m.header.platform = platform;
  m.isPartial = true;

  // Tables.
  m.strings = strings;
  m.processes = processes;
  const size_t threadCount = threads.size();
  m.threads.reserve(threadCount);
  m.tidToThread.reserve(threadCount);
  for (size_t i = 0; i < threadCount; ++i) {
    Thread t = threads[i].t;
    const SampleStore& s = samples[i];
    if (s.size() != 0) {
      if (t.first_ts == 0 || t.first_ts < start) t.first_ts = std::max(s.ts[s.head], start);
      if (t.last_ts == 0) t.last_ts = s.ts.back();
    }
    if (t.first_ts == 0) t.first_ts = start;
    if (t.last_ts == 0) t.last_ts = end;
    if (t.last_ts < t.first_ts) t.last_ts = t.first_ts;
    t.role = classifyThreadRole(t.name);
    if (threads[i].defined && t.tid != 0) m.tidToThread.emplace(t.tid, ThreadIdx(static_cast<uint32_t>(i)));
    m.threads.push_back(std::move(t));
  }
  m.modules = modules;
  const auto moduleCount = static_cast<uint32_t>(modules.size());
  m.frames = frames;
  for (Frame& f : m.frames)
    if (f.module.valid() && f.module.get() >= moduleCount) f.module = ModuleIdx::invalid();
  m.nodes = nodes;
  const auto frameCount = static_cast<uint32_t>(frames.size());
  for (CallstackNode& n : m.nodes)
    if (n.frame.valid() && n.frame.get() >= frameCount) n.frame = FrameIdx::invalid();

  // Samples.
  m.samplesByThread.resize(threadCount);
  uint64_t sampleTotal = 0;
  for (size_t i = 0; i < threadCount; ++i) {
    const SampleStore& s = samples[i];
    const size_t cnt = s.size();
    if (cnt == 0) continue;
    SampleColumns& c = m.samplesByThread[i];
    const auto h = static_cast<std::ptrdiff_t>(s.head);
    c.ts.assign(s.ts.begin() + h, s.ts.end());
    c.callstack.assign(s.node.begin() + h, s.node.end());
    c.cpu.assign(s.cpu.begin() + h, s.cpu.end());
    c.flags.assign(s.flags.begin() + h, s.flags.end());
    sampleTotal += cnt;
  }
  m.statesByThread.resize(threadCount);
  m.scopesByThread.resize(threadCount);

  // Events.
  m.contextSwitches.assign(switches.v.begin() + static_cast<std::ptrdiff_t>(switches.head), switches.v.end());
  m.readyEvents.assign(readies.v.begin() + static_cast<std::ptrdiff_t>(readies.head), readies.v.end());
  m.scopeEvents.assign(scopes.v.begin() + static_cast<std::ptrdiff_t>(scopes.head), scopes.v.end());
  auto threadOf = [&](uint32_t tid) {
    auto it = m.tidToThread.find(tid);
    return it == m.tidToThread.end() ? ThreadIdx::invalid() : it->second;
  };
  m.frameMarks.reserve(frameMarks.size());
  for (size_t i = frameMarks.head; i < frameMarks.v.size(); ++i) {
    const Aligned<trace::FrameMarkRec> ra(frameMarks.v[i]);
    const auto& r = ra.v;
    m.frameMarks.push_back(FrameMark{r.ts, threadOf(r.tid), r.name, r.frame_number});
  }
  m.markers.reserve(markers.size());
  for (size_t i = markers.head; i < markers.v.size(); ++i) {
    const Aligned<trace::MarkerRec> ra(markers.v[i]);
    const auto& r = ra.v;
    m.markers.push_back(Marker{r.ts, threadOf(r.tid), r.name, r.data, r.color_rgba});
  }
  {
    std::unordered_map<uint32_t, size_t> seriesOf;
    for (size_t i = plots.head; i < plots.v.size(); ++i) {
      const Aligned<trace::PlotSampleRec> ra(plots.v[i]);
      const auto& r = ra.v;
      auto [it, inserted] = seriesOf.emplace(r.name.get(), m.plots.size());
      if (inserted) {
        m.plots.emplace_back();
        m.plots.back().name = r.name;
      }
      PlotSeries& ps = m.plots[it->second];
      ps.ts.push_back(static_cast<uint64_t>(r.ts));
      ps.value.push_back(static_cast<double>(r.value));
    }
  }
  if (!m.scopeEvents.empty() || !m.frameMarks.empty() || !m.markers.empty() || !m.plots.empty())
    m.header.flags |= trace::kFlagHasInstrumentation;

  // Facts.
  m.captureInfo.sampling_hz = samplingHz;
  m.captureInfo.events_lost = stats.droppedLive;
  m.captureInfo.raw_json =
      "{\"live\":true,\"window_s\":" + std::to_string(static_cast<double>(options.windowNs) / 1e9) +
      ",\"sampling_hz\":" + std::to_string(samplingHz) +
      ",\"dropped_live\":" + std::to_string(stats.droppedLive) + "}";
  if (m.header.clock_frequency_hz != 0 && samplingHz != 0) {
    m.sampleIntervalTicks = static_cast<uint64_t>(
        static_cast<double>(m.header.clock_frequency_hz) / static_cast<double>(samplingHz) + 0.5);
  } else {
    m.sampleIntervalTicks = std::max<uint64_t>(m.header.clock_frequency_hz / 1000, 1);
  }

  m.loadStatus.complete = false;
  m.loadStatus.generation = static_cast<uint32_t>(generation.load());
  m.loadStatus.sampleChunksLoaded = 1;
  m.loadStatus.sampleChunksTotal = 1;
  m.loadStatus.fullEndTs = end;
  m.loadStatus.samplesLoaded = sampleTotal;
  return out;
}

}  // namespace tachyon::model
