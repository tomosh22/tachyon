#include "generator.h"

#include "prng.h"
#include "reference.h"
#include "timeline.h"
#include "trace/trace_writer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <memory>
#include <tuple>

namespace tachyon::tracegen {

using json = nlohmann::json;
using namespace tachyon::trace;

std::string unmappedDisplayName(uint64_t address) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "<unmapped>!0x%" PRIx64, address);
  return buf;
}

std::string functionDisplayName(const Scenario& s, uint32_t module, uint32_t function) {
  return s.modules[module].name + "!" + s.modules[module].functions[function].name;
}

std::string buildCaptureInfo(const Scenario& s) {
  json j;
  j["generator"] = std::string("tracegen ") + kGeneratorVersion;
  j["agent_version"] = std::string("tracegen-") + kGeneratorVersion;
  j["sampling_hz"] = s.samplingHz;
  j["events_lost"] = s.eventsLost;
  j["host"] = json{{"os", s.platform == 1 ? "windows" : "linux"}, {"synthetic", true}};
  j["capture_options"] = json{{"scenario", s.name}, {"seed", s.seed}};
  j["tracegen"] = json{{"detail", s.detail}, {"blend", s.blend}, {"scenario", s.name}};
  return j.dump();
}

Result<void> writeJsonFile(const std::filesystem::path& path, const json& doc, int indent) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return makeError(ErrorCode::IoError, "cannot create " + path.string());
  const std::string text = doc.dump(indent) + "\n";
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  f.flush();
  if (!f) return makeError(ErrorCode::IoError, "write failed for " + path.string());
  return ok();
}

json buildSymbolsJson(const Scenario& s) {
  json mods = json::array();
  for (size_t mi = 0; mi < s.modules.size(); ++mi) {
    const ModuleSpec& m = s.modules[mi];
    json fns = json::array();
    for (const FunctionSpec& f : m.functions) {
      json lines = json::array();
      for (const LineSpec& l : f.lines)
        lines.push_back(json{{"rva", l.rva}, {"size", l.size}, {"line", l.line}});
      json inlines = json::array();
      for (const InlineSpec& in : f.inlines) {
        json chain = json::array();
        for (const InlineFrameSpec& c : in.chain)
          chain.push_back(json{{"function", c.function}, {"file", c.file}, {"line", c.line}});
        inlines.push_back(json{{"rva", in.rva}, {"size", in.size}, {"chain", std::move(chain)}});
      }
      fns.push_back(json{{"name", f.name},
                         {"start_rva", f.rva},
                         {"size", f.size},
                         {"file", f.file},
                         {"line", f.line},
                         {"lines", std::move(lines)},
                         {"inlines", std::move(inlines)}});
    }
    static const char* kHex = "0123456789abcdef";
    std::string idHex;
    for (const uint8_t b : m.id) {
      idHex.push_back(kHex[b >> 4]);
      idHex.push_back(kHex[b & 15]);
    }
    mods.push_back(json{{"module_idx", mi},
                        {"name", m.name},
                        {"path", m.path},
                        {"base", m.base},
                        {"size", m.size},
                        {"id_kind", m.idKind == 1 ? "pdb" : "gnu-build-id"},
                        {"id", idHex},
                        {"age", m.age},
                        {"functions", std::move(fns)}});
  }
  return json{{"schema", "tachyon-symbols-1"}, {"scenario", s.name}, {"modules", std::move(mods)}};
}

namespace {

constexpr uint8_t kOldStateReady = 1;
constexpr uint8_t kOldStateWaiting = 5;

struct StackInfo {
  CallstackIdx cs;
  std::vector<FnKey> keys;  // root to leaf
  std::vector<std::string> namesLeafFirst;
};

// Weighted stack picker. A Weyl sequence over the per-thread counter gives a low-discrepancy, exactly
// reproducible choice whose long-run shares match the weights.
class MixTable {
 public:
  MixTable() = default;
  explicit MixTable(const std::vector<MixEntry>& mix) {
    uint64_t total = 0;
    for (const MixEntry& e : mix) total += e.weight;
    uint64_t cum = 0;
    for (const MixEntry& e : mix) {
      cum += e.weight;
      m_stacks.push_back(e.stack);
      m_thr.push_back((cum << 32) / total);
    }
  }
  bool empty() const { return m_stacks.empty(); }
  uint32_t pick(uint64_t counter) const {
    const uint64_t p = (counter * 0x9e3779b97f4a7c15ull) >> 32;
    size_t i = 0;
    while (i + 1 < m_thr.size() && p >= m_thr[i]) ++i;
    return m_stacks[i];
  }

 private:
  std::vector<uint32_t> m_stacks;
  std::vector<uint64_t> m_thr;
};

struct SampleRow {
  uint64_t ts;
  uint32_t tid;
  uint32_t cs;
  uint16_t cpu;
};

struct SampleLogRec {
  uint64_t ts;  // absolute
  uint32_t tid;
  uint32_t stack;
  uint64_t weight;
};

struct WindowOut {
  std::vector<SampleRow> samples;
  std::vector<ContextSwitchRec> switches;
  std::vector<ReadyRec> ready;
};

class Generator;

struct ThreadGen {
  ThreadGen(const Scenario& s, const ThreadSpec& t, size_t stackCount)
      : spec(t), tl(s, t), accSamples(stackCount, 0), accTicks(stackCount, 0) {
    phase = streamSeed(s.seed, "phase", t.tid) % s.sampleInterval;
    counter = streamSeed(s.seed, "weyl", t.tid);
    nextSample = phase;
  }
  const ThreadSpec& spec;
  Timeline tl;
  Segment cur;
  bool have = false, done = false, startEmitted = false, readyEmitted = false, anySegment = false;
  uint64_t phase = 0, nextSample = 0, counter = 0;
  size_t extraIdx = 0;
  MixTable defMix;
  std::vector<MixTable> overrides;
  std::vector<uint64_t> accSamples, accTicks;  // per scenario stack
  uint64_t zeroSamples = 0, clippedSamples = 0, totalSamples = 0;
  std::array<uint64_t, 4> stateTicks{};
  uint64_t switchesOut = 0, switchesIn = 0, readyEvents = 0;
  std::vector<IntervalLog> intervals;
};

class Generator {
 public:
  explicit Generator(const Scenario& s) : m_s(s) {}

  Result<void> run(const std::filesystem::path& path, Tally& tally, uint64_t& sampleCount);

 private:
  uint64_t abs(uint64_t rel) const { return m_s.startTs + rel; }
  void setup();
  void buildStacks();
  void emitStatic();
  void advance(ThreadGen& g, uint64_t w, WindowOut& out);
  void account(ThreadGen& g);
  void emitSample(ThreadGen& g, uint64_t rel, uint64_t weightEnd, WindowOut& out);
  void flushWindow(WindowOut& out, bool force);
  void buildTally(Tally& t);
  void buildScopeTally(Tally& t);
  void buildBlended(Tally& t);
  void buildFrames(Tally& t);
  void buildFibers(Tally& t);
  void buildPlots(Tally& t);

  const Scenario& m_s;
  std::unique_ptr<TraceWriter> m_w;
  std::vector<StackInfo> m_stacks;
  std::vector<std::unique_ptr<ThreadGen>> m_threads;
  std::vector<SampleLogRec> m_sampleLog;
  bool m_logSamples = false;
  bool m_logIntervals = false;
  SampleBatch m_batch;
  std::vector<ContextSwitchRec> m_csAcc;
  std::vector<ReadyRec> m_readyAcc;
};

void Generator::buildStacks() {
  m_stacks.resize(m_s.stacks.size());
  for (size_t i = 0; i < m_s.stacks.size(); ++i) {
    const StackSpec& st = m_s.stacks[i];
    StackInfo& info = m_stacks[i];
    std::vector<FrameIdx> leafFirst;
    for (auto it = st.rootToLeaf.rbegin(); it != st.rootToLeaf.rend(); ++it) {
      const StackFrame& f = *it;
      if (f.module == kUnmappedModule) {
        leafFirst.push_back(m_w->internFrame(ModuleIdx::invalid(), f.value));
        info.namesLeafFirst.push_back(unmappedDisplayName(f.value));
      } else {
        leafFirst.push_back(m_w->internFrame(ModuleIdx(f.module), f.value));
        info.namesLeafFirst.push_back(functionDisplayName(m_s, f.module, f.function));
      }
    }
    info.cs = m_w->internCallstack(leafFirst);
    for (const StackFrame& f : st.rootToLeaf) {
      FnKey k;
      if (f.module == kUnmappedModule) {
        k.kind = 1;
        k.module = kUnmappedModule;
        k.startRva = f.value;
      } else {
        k.kind = 0;
        k.module = f.module;
        k.startRva = m_s.modules[f.module].functions[f.function].rva;
      }
      info.keys.push_back(k);
    }
  }
}

// Process/thread/module tables, capture info, and every sparse stream (scopes, fibers, frame marks, ...).
void Generator::emitStatic() {
  TraceWriter& w = *m_w;
  w.appendCaptureInfo(buildCaptureInfo(m_s));
  const ProcessRec proc{m_s.pid,
                        m_s.parentPid,
                        m_s.startTs,
                        0,
                        w.internString(m_s.processName),
                        w.internString(m_s.commandLine)};
  w.append(std::span<const ProcessRec>(&proc, 1));
  std::vector<ThreadRec> threads;
  for (const ThreadSpec& t : m_s.threads)
    threads.push_back(ThreadRec{t.tid, m_s.pid, abs(t.fromStart ? 0 : t.startOffset), 0,
                                w.internString(t.name), kThreadFlagTargetProcess});
  w.append(std::span<const ThreadRec>(threads));
  std::vector<ModuleRec> mods;
  for (const ModuleSpec& m : m_s.modules) {
    ModuleRec r{};
    r.pid = m_s.pid;
    r.base = m.base;
    r.size = m.size;
    r.path = w.internString(m.path);
    r.id_kind = m.idKind;
    r.id_len = static_cast<uint8_t>(m.id.size());
    std::copy(m.id.begin(), m.id.end(), r.id);
    r.age = m.age;
    r.load_ts = m_s.startTs;
    r.unload_ts = 0;
    mods.push_back(r);
  }
  w.append(std::span<const ModuleRec>(mods));
  if (!m_s.renames.empty()) {
    std::vector<ThreadNameRec> rn;
    for (const ThreadRename& r : m_s.renames)
      rn.push_back(ThreadNameRec{abs(r.ts), r.tid, w.internString(r.name)});
    std::stable_sort(rn.begin(), rn.end(), [](const auto& a, const auto& b) { return a.ts < b.ts; });
    w.append(std::span<const ThreadNameRec>(rn));
  }

  if (!m_s.scopes.empty()) {
    struct Ev {
      uint64_t ts;
      int kind;  // 0 end, 1 begin
      int64_t order;
      size_t seq;
      ScopeEventRec rec;
    };
    std::vector<Ev> evs;
    size_t seq = 0;
    for (const ScopeInstance& si : m_s.scopes) {
      ScopeEventRec b{};
      b.ts = abs(si.begin);
      b.tid = si.tid;
      b.kind = kScopeBegin;
      b.id = w.internString(si.id);
      b.data = w.internString(si.data);
      b.color_rgba = si.color;
      b.caller_addr = si.callerAddr;
      ScopeEventRec e{};
      e.ts = abs(si.end);
      e.tid = si.tid;
      e.kind = kScopeEnd;
      e.id = StringIdx(0);
      e.data = StringIdx(0);
      // Ends: deeper scopes first; begins: shallower scopes first (keeps zero-gap nesting well formed).
      evs.push_back(Ev{b.ts, 1, static_cast<int64_t>(si.depth), seq++, b});
      evs.push_back(Ev{e.ts, 0, -static_cast<int64_t>(si.depth), seq++, e});
    }
    std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
      return std::tie(a.ts, a.kind, a.order, a.seq) < std::tie(b.ts, b.kind, b.order, b.seq);
    });
    std::vector<ScopeEventRec> recs;
    recs.reserve(evs.size());
    for (const Ev& e : evs) recs.push_back(e.rec);
    w.append(std::span<const ScopeEventRec>(recs));
  }
  if (!m_s.fiberEvents.empty()) {
    std::vector<FiberEventRec> recs;
    for (const FiberEventInst& e : m_s.fiberEvents) {
      FiberEventRec r{};
      r.ts = abs(e.ts);
      r.tid = e.tid;
      r.kind = e.kind;
      r.fiber_id = e.fiber;
      r.other_fiber_id = e.other;
      recs.push_back(r);
    }
    std::stable_sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.ts < b.ts; });
    w.append(std::span<const FiberEventRec>(recs));
  }
  if (!m_s.frameMarks.empty()) {
    std::vector<FrameMarkRec> recs;
    for (const FrameMarkInst& f : m_s.frameMarks)
      recs.push_back(FrameMarkRec{abs(f.ts), f.tid, w.internString(f.name), f.number});
    std::stable_sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.ts < b.ts; });
    w.append(std::span<const FrameMarkRec>(recs));
  }
  if (!m_s.markers.empty()) {
    std::vector<MarkerRec> recs;
    for (const MarkerInst& m : m_s.markers)
      recs.push_back(MarkerRec{abs(m.ts), m.tid, w.internString(m.name), w.internString(m.data), m.color});
    std::stable_sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.ts < b.ts; });
    w.append(std::span<const MarkerRec>(recs));
  }
  if (!m_s.plots.empty()) {
    std::vector<PlotSampleRec> recs;
    for (const PlotInst& p : m_s.plots)
      recs.push_back(PlotSampleRec{abs(p.ts), w.internString(p.name), p.value});
    std::stable_sort(recs.begin(), recs.end(), [](const auto& a, const auto& b) { return a.ts < b.ts; });
    w.append(std::span<const PlotSampleRec>(recs));
  }
}

// Records the state intervals of a freshly fetched segment (state totals, detail list).
void Generator::account(ThreadGen& g) {
  const Segment& s = g.cur;
  auto stackNames = [&](int32_t idx) {
    return idx >= 0 ? m_stacks[static_cast<size_t>(idx)].namesLeafFirst : std::vector<std::string>{};
  };
  auto add = [&](uint8_t state, uint64_t start, uint64_t end, bool withStacks, bool withWaker) {
    g.stateTicks[state] += end - start;
    if (!m_logIntervals) return;
    IntervalLog iv;
    iv.state = state;
    iv.start = abs(start);
    iv.end = abs(end);
    if (withStacks) iv.blocking = stackNames(s.blockingStack);
    if (withWaker) {
      iv.unblocking = stackNames(s.wakerStack);
      iv.wokeBy = s.wakerTid;
    }
    g.intervals.push_back(std::move(iv));
  };
  if (!g.anySegment) {
    g.anySegment = true;
    if (s.start > 0) add(kStateUnknown, 0, s.start, false, false);
  }
  switch (s.state) {
    case SegState::Run: add(kStateRunning, s.start, s.end, false, false); break;
    case SegState::Wait:
      if (s.readyAt != 0) {
        add(kStateWaiting, s.start, s.readyAt, true, true);
        add(kStateReady, s.readyAt, s.end, true, true);
      } else {
        add(kStateWaiting, s.start, s.end, true, false);
      }
      break;
    case SegState::Ready: add(kStateReady, s.start, s.end, true, false); break;
  }
}

void Generator::emitSample(ThreadGen& g, uint64_t rel, uint64_t weightEnd, WindowOut& out) {
  const MixTable& mix = g.cur.mix >= 0 ? g.overrides[static_cast<size_t>(g.cur.mix)] : g.defMix;
  const uint32_t stack = mix.pick(g.counter++);
  const uint64_t weight = std::min<uint64_t>(m_s.sampleInterval, weightEnd - rel);
  out.samples.push_back(
      SampleRow{abs(rel), g.spec.tid, m_stacks[stack].cs.get(), static_cast<uint16_t>(g.spec.cpu)});
  g.accSamples[stack] += 1;
  g.accTicks[stack] += weight;
  g.totalSamples += 1;
  if (weight < m_s.sampleInterval) g.clippedSamples += 1;
  if (m_logSamples) m_sampleLog.push_back(SampleLogRec{abs(rel), g.spec.tid, stack, weight});
}

// Emits every event / sample of thread `g` with ts < w (ticks since the trace start).
void Generator::advance(ThreadGen& g, uint64_t w, WindowOut& out) {
  const uint64_t interval = m_s.sampleInterval;
  const uint32_t tid = g.spec.tid;
  for (;;) {
    if (!g.have) {
      if (g.done) break;
      if (!g.tl.next(g.cur)) {
        g.done = true;
        if (!g.anySegment) {
          g.stateTicks[kStateUnknown] += m_s.duration;
          if (m_logIntervals)
            g.intervals.push_back(IntervalLog{kStateUnknown, abs(0), abs(m_s.duration), {}, {}, 0});
        }
        break;
      }
      g.have = true;
      g.startEmitted = false;
      g.readyEmitted = false;
      account(g);
      if (g.cur.state == SegState::Run) {
        const uint64_t x = g.cur.start;
        const uint64_t first =
            x <= g.phase ? g.phase : g.phase + ((x - g.phase + interval - 1) / interval) * interval;
        g.nextSample = first;
      }
    }
    if (g.cur.start >= w) break;
    if (!g.startEmitted) {
      g.startEmitted = true;
      if (g.cur.state == SegState::Run) {
        if (!g.cur.fromStart) {
          ContextSwitchRec r{};
          r.ts = abs(g.cur.start);
          r.cpu = g.spec.cpu;
          r.old_tid = 0;
          r.new_tid = tid;
          r.new_priority = g.cur.priority;
          r.old_thread_stack = CallstackIdx::invalid();
          out.switches.push_back(r);
          g.switchesIn += 1;
        }
      } else {
        ContextSwitchRec r{};
        r.ts = abs(g.cur.start);
        r.cpu = g.spec.cpu;
        r.old_tid = tid;
        r.new_tid = 0;
        r.old_state = g.cur.state == SegState::Ready ? kOldStateReady : kOldStateWaiting;
        r.old_wait_reason = g.cur.waitReason;
        r.old_wait_mode = g.cur.waitMode;
        r.new_priority = g.cur.priority;
        r.old_thread_stack = g.cur.blockingStack >= 0 ? m_stacks[static_cast<size_t>(g.cur.blockingStack)].cs
                                                      : CallstackIdx::invalid();
        out.switches.push_back(r);
        g.switchesOut += 1;
      }
    }
    if (g.cur.state == SegState::Run) {
      const uint64_t lim = std::min(g.cur.end, w);
      while (g.nextSample < lim) {
        emitSample(g, g.nextSample, g.cur.end, out);
        g.nextSample += interval;
      }
    } else if (g.cur.readyAt != 0 && !g.readyEmitted && g.cur.readyAt < w) {
      g.readyEmitted = true;
      ReadyRec r{};
      r.ts = abs(g.cur.readyAt);
      r.cpu = g.spec.cpu;
      r.readied_tid = tid;
      r.readying_tid = g.cur.wakerTid;
      r.readying_stack = g.cur.wakerStack >= 0 ? m_stacks[static_cast<size_t>(g.cur.wakerStack)].cs
                                               : CallstackIdx::invalid();
      out.ready.push_back(r);
      g.readyEvents += 1;
    }
    if (g.cur.end <= w)
      g.have = false;
    else
      break;
  }
  // Stray samples (outside any running interval by construction; the reference verifies that).
  while (g.extraIdx < g.spec.extraSamples.size() && g.spec.extraSamples[g.extraIdx].at < w) {
    const ExtraSample& e = g.spec.extraSamples[g.extraIdx++];
    out.samples.push_back(
        SampleRow{abs(e.at), tid, m_stacks[e.stack].cs.get(), static_cast<uint16_t>(g.spec.cpu)});
    g.accSamples[e.stack] += 1;
    g.totalSamples += 1;
    g.zeroSamples += 1;
    if (m_logSamples) m_sampleLog.push_back(SampleLogRec{abs(e.at), tid, e.stack, 0});
  }
}

void Generator::flushWindow(WindowOut& out, bool force) {
  std::stable_sort(out.samples.begin(), out.samples.end(), [](const SampleRow& a, const SampleRow& b) {
    return std::tie(a.ts, a.tid) < std::tie(b.ts, b.tid);
  });
  for (const SampleRow& r : out.samples) m_batch.push(r.ts, r.tid, CallstackIdx(r.cs), r.cpu);
  std::stable_sort(out.switches.begin(), out.switches.end(),
                   [](const ContextSwitchRec& a, const ContextSwitchRec& b) { return a.ts < b.ts; });
  m_csAcc.insert(m_csAcc.end(), out.switches.begin(), out.switches.end());
  std::stable_sort(out.ready.begin(), out.ready.end(),
                   [](const ReadyRec& a, const ReadyRec& b) { return a.ts < b.ts; });
  m_readyAcc.insert(m_readyAcc.end(), out.ready.begin(), out.ready.end());
  out.samples.clear();
  out.switches.clear();
  out.ready.clear();
  if (force || m_batch.size() >= m_s.chunkRecords) {
    if (m_batch.size() > 0) m_w->append(m_batch);
    m_batch = SampleBatch{};
  }
  if (force || m_csAcc.size() >= m_s.chunkRecords) {
    if (!m_csAcc.empty()) m_w->append(std::span<const ContextSwitchRec>(m_csAcc));
    m_csAcc.clear();
  }
  if (force || m_readyAcc.size() >= m_s.chunkRecords) {
    if (!m_readyAcc.empty()) m_w->append(std::span<const ReadyRec>(m_readyAcc));
    m_readyAcc.clear();
  }
}

Result<void> Generator::run(const std::filesystem::path& path, Tally& tally, uint64_t& sampleCount) {
  FileHeader h;
  h.clock_frequency_hz = m_s.clockHz;
  h.start_ts = m_s.startTs;
  h.end_ts = m_s.startTs + m_s.duration;
  h.platform = m_s.platform;
  if (!m_s.scopes.empty()) h.flags |= kFlagHasInstrumentation;
  auto opened = TraceWriter::open(path, h);
  if (!opened.ok()) return std::move(opened).error();
  m_w = std::make_unique<TraceWriter>(std::move(opened).value());

  m_logSamples = m_s.blend || !m_s.frameMarks.empty();
  m_logIntervals = m_s.detail || !m_s.frameMarks.empty();
  buildStacks();
  emitStatic();

  for (const ThreadSpec& t : m_s.threads) {
    auto g = std::make_unique<ThreadGen>(m_s, t, m_s.stacks.size());
    g->defMix = MixTable(t.mix);
    for (const auto& ov : t.mixOverrides) g->overrides.emplace_back(ov);
    m_threads.push_back(std::move(g));
  }

  const uint64_t window = std::max<uint64_t>(1, m_s.clockHz / 20);
  WindowOut out;
  for (uint64_t w = window;; w += window) {
    const uint64_t wc = std::min(w, m_s.duration);
    for (auto& g : m_threads) advance(*g, wc, out);
    flushWindow(out, wc >= m_s.duration);
    if (wc >= m_s.duration) break;
  }
  if (!m_s.partial) {
    auto r = m_w->finish();
    if (!r.ok()) return r;
  }
  m_w.reset();  // an unfinished writer leaves a partial (recoverable) file on disk

  buildTally(tally);
  sampleCount = tally.nSamples;
  return ok();
}

void Generator::buildTally(Tally& t) {
  t.scenario = m_s.name;
  t.generator = std::string("tracegen ") + kGeneratorVersion;
  t.seed = m_s.seed;
  t.clockHz = m_s.clockHz;
  t.samplingHz = m_s.samplingHz;
  t.sampleInterval = m_s.sampleInterval;
  t.startTs = m_s.startTs;
  t.endTs = m_s.startTs + m_s.duration;
  t.partial = m_s.partial;
  t.blend = m_s.blend;
  t.detail = m_s.detail;
  for (const ModuleSpec& m : m_s.modules) t.modules.push_back({m.name, m.base});
  for (const StackSpec& st : m_s.stacks)
    for (const StackFrame& f : st.rootToLeaf) {
      FnKey k;
      if (f.module == kUnmappedModule) {
        k.kind = 1;
        k.module = kUnmappedModule;
        k.startRva = f.value;
        t.names[k] = unmappedDisplayName(f.value);
      } else {
        k.module = f.module;
        k.startRva = m_s.modules[f.module].functions[f.function].rva;
        t.names[k] = functionDisplayName(m_s, f.module, f.function);
      }
    }
  for (const ScopeInstance& si : m_s.scopes) {
    FnKey k;
    k.kind = 2;
    k.scope = si.id;
    t.names[k] = "scope:" + si.id;
  }
  for (const auto& gp : m_threads) {
    const ThreadGen& g = *gp;
    ThreadTally& tt = t.threads[g.spec.tid];
    tt.name = g.spec.name;
    tt.samples = g.totalSamples;
    tt.zeroWeight = g.zeroSamples;
    tt.clipped = g.clippedSamples;
    tt.stateTicks = g.stateTicks;
    tt.switchesOut = g.switchesOut;
    tt.switchesIn = g.switchesIn;
    tt.readyEvents = g.readyEvents;
    if (m_s.detail) tt.intervals = g.intervals;
    uint64_t ticks = 0;
    for (size_t s = 0; s < m_stacks.size(); ++s) {
      if (g.accSamples[s] == 0) continue;
      t.addStack(g.spec.tid, m_stacks[s].keys, false, g.accTicks[s], g.accSamples[s]);
      ticks += g.accTicks[s];
    }
    tt.sampleTicks = ticks;
    t.nSamples += g.totalSamples;
    t.nSwitches += g.switchesIn + g.switchesOut;
    t.nReady += g.readyEvents;
  }
  t.nScopeEvents = m_s.scopes.size() * 2;
  t.nFiberEvents = m_s.fiberEvents.size();
  t.nFrameMarks = m_s.frameMarks.size();
  t.nMarkers = m_s.markers.size();
  t.nPlots = m_s.plots.size();
  t.nThreadNames = m_s.renames.size();
  buildScopeTally(t);
  if (m_s.blend) buildBlended(t);
  buildFrames(t);
  buildFibers(t);
  buildPlots(t);
}

void Generator::buildScopeTally(Tally& t) {
  for (const ScopeInstance& si : m_s.scopes) t.scopeDurations[si.id].push_back(si.end - si.begin);
}

// Rule 5: splice scope frames into each sample's stack.
void Generator::buildBlended(Tally& t) {
  std::vector<std::vector<const ScopeInstance*>> perThread(m_s.threads.size());
  auto threadSlot = [&](uint32_t tid) -> size_t {
    for (size_t i = 0; i < m_s.threads.size(); ++i)
      if (m_s.threads[i].tid == tid) return i;
    return 0;
  };
  for (const ScopeInstance& si : m_s.scopes) perThread[threadSlot(si.tid)].push_back(&si);
  for (auto& v : perThread)
    std::stable_sort(v.begin(), v.end(), [](const ScopeInstance* a, const ScopeInstance* b) {
      return std::tie(a->begin, a->depth) < std::tie(b->begin, b->depth);
    });

  std::vector<SampleLogRec> log = m_sampleLog;
  std::stable_sort(log.begin(), log.end(), [](const SampleLogRec& a, const SampleLogRec& b) {
    return std::tie(a.tid, a.ts) < std::tie(b.tid, b.ts);
  });
  size_t curThread = SIZE_MAX;
  size_t pos = 0;
  std::vector<const ScopeInstance*> open;
  for (const SampleLogRec& s : log) {
    const size_t ti = threadSlot(s.tid);
    if (ti != curThread) {
      curThread = ti;
      pos = 0;
      open.clear();
    }
    const uint64_t rel = s.ts - m_s.startTs;
    const auto& list = perThread[ti];
    while (pos < list.size() && list[pos]->begin <= rel) open.push_back(list[pos++]);
    open.erase(
        std::remove_if(open.begin(), open.end(), [&](const ScopeInstance* p) { return p->end <= rel; }),
        open.end());
    std::sort(open.begin(), open.end(),
              [](const ScopeInstance* a, const ScopeInstance* b) { return a->depth < b->depth; });

    const StackSpec& st = m_s.stacks[s.stack];
    const std::vector<FnKey>& keys = m_stacks[s.stack].keys;
    // slot[i]: the original frame index after which open scope i is inserted.
    std::vector<size_t> slot(open.size());
    std::vector<bool> matched(open.size());
    size_t prev = 0;
    for (size_t i = 0; i < open.size(); ++i) {
      const ScopeInstance& sc = *open[i];
      size_t found = SIZE_MAX;
      for (size_t j = st.rootToLeaf.size(); j-- > 0;) {
        const StackFrame& f = st.rootToLeaf[j];
        const bool hit = sc.callerModule == kUnmappedModule
                             ? (f.module == kUnmappedModule && f.value == sc.callerAddr)
                             : (f.module == sc.callerModule && f.function == sc.callerFunction);
        if (hit) {
          found = j;
          break;
        }
      }
      matched[i] = found != SIZE_MAX;
      slot[i] = std::max(matched[i] ? found : size_t{0}, prev);
      prev = slot[i];
    }
    std::vector<FnKey> blended;
    for (size_t j = 0; j < keys.size(); ++j) {
      blended.push_back(keys[j]);
      for (size_t i = 0; i < open.size(); ++i) {
        if (slot[i] != j) continue;
        FnKey sk;
        sk.kind = 2;
        sk.scope = open[i]->id;
        PlacementKey pk;
        pk.scope = open[i]->id;
        pk.matched = matched[i];
        pk.parent = blended.back();
        Placement& pl = t.placements[pk];
        pl.samples += 1;
        pl.ticks += s.weight;
        blended.push_back(sk);
      }
    }
    t.addStack(s.tid, blended, true, s.weight, 1);
  }
}

void Generator::buildFrames(Tally& t) {
  if (m_s.frameMarks.empty()) return;
  std::map<std::pair<uint32_t, std::string>, std::vector<const FrameMarkInst*>> groups;
  for (const FrameMarkInst& f : m_s.frameMarks) groups[{f.tid, f.name}].push_back(&f);
  for (auto& [key, marks] : groups) {
    std::stable_sort(marks.begin(), marks.end(),
                     [](const FrameMarkInst* a, const FrameMarkInst* b) { return a->ts < b->ts; });
    for (size_t i = 0; i + 1 < marks.size(); ++i) {
      FrameTally f;
      f.tid = key.first;
      f.name = key.second;
      f.number = marks[i]->number;
      f.start = abs(marks[i]->ts);
      f.end = abs(marks[i + 1]->ts);
      for (const auto& gp : m_threads) {
        FrameThreadTally ft;
        for (const IntervalLog& iv : gp->intervals) {
          if (iv.state != kStateRunning) continue;
          const uint64_t lo = std::max(iv.start, f.start), hi = std::min(iv.end, f.end);
          if (hi > lo) ft.runningTicks += hi - lo;
        }
        f.threads[gp->spec.tid] = ft;
      }
      for (const SampleLogRec& s : m_sampleLog)
        if (s.ts >= f.start && s.ts < f.end) f.threads[s.tid].sampleTicks += s.weight;
      t.frames.push_back(std::move(f));
    }
  }
}

void Generator::buildFibers(Tally& t) {
  if (m_s.fiberEvents.empty()) return;
  // Periods during which each (tid, fiber) is the running fiber, from the switch events.
  std::map<std::pair<uint32_t, uint64_t>, std::vector<std::pair<uint64_t, uint64_t>>> periods;
  std::map<std::pair<uint32_t, uint64_t>, uint64_t> runningSince;
  std::vector<FiberEventInst> evs = m_s.fiberEvents;
  std::stable_sort(evs.begin(), evs.end(), [](const auto& a, const auto& b) { return a.ts < b.ts; });
  for (const FiberEventInst& e : evs) {
    if (e.kind == kFiberSwitchEnd) {
      runningSince[{e.tid, e.fiber}] = e.ts;
    } else if (e.kind == kFiberSwitchBegin) {
      const auto it = runningSince.find({e.tid, e.fiber});
      if (it != runningSince.end()) {
        periods[{e.tid, e.fiber}].push_back({it->second, e.ts});
        runningSince.erase(it);
      }
    }
  }
  for (const auto& [key, since] : runningSince) periods[key].push_back({since, m_s.duration});
  for (const ScopeInstance& si : m_s.scopes) {
    if (si.fiber == 0) continue;
    for (const auto& [a, b] : periods[{si.tid, si.fiber}]) {
      const uint64_t lo = std::max(a, si.begin), hi = std::min(b, si.end);
      if (hi > lo)
        t.fibers[si.fiber].push_back(FiberSegment{si.tid, si.id, si.depth, abs(si.begin), abs(lo), abs(hi)});
    }
  }
  for (const FiberEventInst& e : evs)
    if (e.kind == kFiberRegister) t.fibers[e.fiber];  // fibers without scopes still appear
}

void Generator::buildPlots(Tally& t) {
  for (const PlotInst& p : m_s.plots) {
    auto [it, inserted] = t.plots.try_emplace(p.name);
    PlotTally& pt = it->second;
    const uint64_t ts = abs(p.ts);
    if (inserted) {
      pt.min = pt.max = p.value;
      pt.firstTs = ts;
    }
    pt.count += 1;
    pt.min = std::min(pt.min, p.value);
    pt.max = std::max(pt.max, p.value);
    pt.firstTs = std::min(pt.firstTs, ts);
    pt.lastTs = std::max(pt.lastTs, ts);
  }
}

}  // namespace

Result<GenerateReport> generateFixture(const Scenario& scenario, const std::filesystem::path& outDir,
                                       const GenerateOptions& options) {
  using Clock = std::chrono::steady_clock;
  std::error_code ec;
  std::filesystem::create_directories(outDir, ec);
  GenerateReport rep;
  rep.tachyonPath = outDir / (scenario.name + ".tachyon");
  rep.symbolsPath = outDir / (scenario.name + ".symbols.json");
  rep.expectedPath = outDir / (scenario.name + ".expected.json");

  const auto t0 = Clock::now();
  TACHYON_TRY(writeJsonFile(rep.symbolsPath, buildSymbolsJson(scenario), 1));
  Tally analytic;
  {
    Generator gen(scenario);
    TACHYON_TRY(gen.run(rep.tachyonPath, analytic, rep.samples));
  }
  const auto t1 = Clock::now();
  rep.generateSeconds = std::chrono::duration<double>(t1 - t0).count();
  rep.tachyonBytes = std::filesystem::file_size(rep.tachyonPath, ec);

  const json expected = analytic.toJson();
  if (options.verify) {
    auto ref = computeReference(rep.tachyonPath, rep.symbolsPath);
    if (!ref.ok()) return makeError(ErrorCode::Internal, "reference failed: " + ref.error().toString());
    const std::string diff = compareJson(expected, ref->toJson());
    if (!diff.empty()) {
      // Keep both documents next to the output for debugging.
      (void)writeJsonFile(outDir / (scenario.name + ".analytic.json"), expected, 2);
      (void)writeJsonFile(outDir / (scenario.name + ".reference.json"), ref->toJson(), 2);
    }
    if (!diff.empty())
      return makeError(ErrorCode::Internal,
                       "analytic result and brute-force reference disagree in scenario '" + scenario.name +
                           "' at " + diff);
    rep.verifySeconds = std::chrono::duration<double>(Clock::now() - t1).count();
  }
  TACHYON_TRY(writeJsonFile(rep.expectedPath, expected, 2));
  return rep;
}

}  // namespace tachyon::tracegen
