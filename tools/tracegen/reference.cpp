#include "reference.h"

#include "generator.h"
#include "trace/trace_reader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace tachyon::tracegen {

using json = nlohmann::json;
using namespace tachyon::trace;

namespace {

struct SymFn {
  uint64_t rva = 0, size = 0;
  std::string name;
};
struct SymModule {
  std::string name;
  std::vector<SymFn> fns;  // sorted by rva
};

// Frame resolved through the sidecar symbol table.
struct RStack {
  std::vector<FnKey> keys;                            // root to leaf
  std::vector<std::string> namesLeafFirst;            // display names
  std::vector<std::pair<uint64_t, uint64_t>> ranges;  // [lo, hi) address range per frame, root to leaf
};

enum class Kind : uint8_t { Unknown, Running, Waiting, Ready };

struct Acc {
  uint64_t samples = 0, ticks = 0;
};

struct ThreadInfo {
  Kind kind = Kind::Unknown;
  bool seen = false;
  uint64_t since = 0;
  uint32_t blockingCs = kInvalidIdx;
  bool hasReady = false;
  uint64_t readyTs = 0;
  uint32_t waker = 0;
  uint32_t wakerCs = kInvalidIdx;
  std::vector<std::pair<uint64_t, uint64_t>> runs;  // closed running intervals
  size_t cursor = 0;
  std::vector<Acc> acc;  // per callstack index
};

template <typename T>
class RecordCursor {
 public:
  RecordCursor(TraceReader& r, ChunkType type) : m_r(r), m_type(type) {}
  const T* peek() {
    while (m_i >= m_buf.size())
      if (!loadNext()) return nullptr;
    return &m_buf[m_i];
  }
  void pop() { ++m_i; }
  bool failed() const { return !m_error.empty(); }
  const std::string& error() const { return m_error; }

 private:
  bool loadNext() {
    m_buf.clear();
    m_i = 0;
    while (m_chunk < m_r.chunks().size()) {
      const FooterRec c = m_r.chunks()[m_chunk++];
      if (c.type != static_cast<uint32_t>(m_type)) continue;
      auto rr = m_r.readChunk(c, m_payload);
      if (!rr.ok()) {
        m_error = rr.error().toString();
        return false;
      }
      auto dr = TraceReader::decodeRecords<T>(c, m_payload, m_buf);
      if (!dr.ok()) {
        m_error = dr.error().toString();
        return false;
      }
      if (!m_buf.empty()) return true;
    }
    return false;
  }
  TraceReader& m_r;
  ChunkType m_type;
  size_t m_chunk = 0;
  size_t m_i = 0;
  std::vector<T> m_buf;
  std::vector<std::byte> m_payload;
  std::string m_error;
};

// Non-throwing JSON accessors: a missing or wrong-typed field yields the default.
const json* member(const json& o, const char* key) {
  if (!o.is_object()) return nullptr;
  const auto it = o.find(key);
  return it == o.end() ? nullptr : &*it;
}
uint64_t getU64(const json& o, const char* key, uint64_t def) {
  const json* v = member(o, key);
  if (!v) return def;
  if (v->is_number_unsigned()) return v->get<uint64_t>();
  if (v->is_number_integer() && v->get<int64_t>() >= 0) return static_cast<uint64_t>(v->get<int64_t>());
  return def;
}
std::string getStr(const json& o, const char* key, const std::string& def) {
  const json* v = member(o, key);
  return v && v->is_string() ? v->get<std::string>() : def;
}
bool getBool(const json& o, const char* key, bool def) {
  const json* v = member(o, key);
  return v && v->is_boolean() ? v->get<bool>() : def;
}

std::string hexName(const std::string& module, uint64_t rva) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "!0x%" PRIx64, rva);
  return module + buf;
}

class Reference {
 public:
  Reference(const std::filesystem::path& t, const std::filesystem::path& s) : m_tpath(t), m_spath(s) {}

  Result<Tally> run();

 private:
  Result<void> loadSymbols();
  const RStack& stackOf(uint32_t cs);
  Result<void> passStates(TraceReader& r);
  Result<void> passSamples(TraceReader& r);
  Result<void> passScopes(TraceReader& r);
  void blend();
  void frames(const std::vector<FrameMarkRec>& marks);
  Result<void> plots(TraceReader& r);

  void closeSegment(uint32_t tid, ThreadInfo& ti, uint64_t end);
  void logInterval(uint32_t tid, StateIdx st, uint64_t a, uint64_t b, const ThreadInfo* withStacks,
                   bool withWaker);

  std::filesystem::path m_tpath, m_spath;
  Tally m_t;
  FileHeader m_h{};
  uint64_t m_interval = 0;
  std::vector<SymModule> m_sym;
  std::vector<ModuleInfo> m_mods;
  StringTable m_strings;
  std::vector<FrameRec> m_frames;
  std::vector<CallstackNodeRec> m_nodes;
  std::vector<int32_t> m_stackSlot;
  std::vector<RStack> m_stackStore;
  std::unordered_map<uint32_t, ThreadInfo> m_threads;

  struct SampleLog {
    uint64_t ts;
    uint32_t tid, cs;
    uint64_t weight;
  };
  std::vector<SampleLog> m_log;
  bool m_needLog = false;

  struct Inst {
    uint32_t tid;
    std::string id;
    uint64_t begin, end, callerAddr;
    uint32_t depth;
  };
  std::vector<Inst> m_insts;
};

Result<void> Reference::loadSymbols() {
  std::ifstream f(m_spath, std::ios::binary);
  if (!f) return makeError(ErrorCode::IoError, "cannot open " + m_spath.string());
  std::stringstream ss;
  ss << f.rdbuf();
  const json doc = json::parse(ss.str(), nullptr, false);
  const json* mods = doc.is_discarded() ? nullptr : member(doc, "modules");
  if (!mods || !mods->is_array())
    return makeError(ErrorCode::CorruptData, "bad symbols file " + m_spath.string());
  for (const json& mj : *mods) {
    SymModule m;
    m.name = getStr(mj, "name", "");
    const json* fns = member(mj, "functions");
    if (!fns || !fns->is_array())
      return makeError(ErrorCode::CorruptData, "symbols file: module without a function array");
    for (const json& fj : *fns) {
      SymFn fn;
      fn.name = getStr(fj, "name", "");
      fn.rva = getU64(fj, "start_rva", 0);
      fn.size = getU64(fj, "size", 0);
      m.fns.push_back(std::move(fn));
    }
    std::sort(m.fns.begin(), m.fns.end(), [](const SymFn& a, const SymFn& b) { return a.rva < b.rva; });
    m_sym.push_back(std::move(m));
  }
  return ok();
}

const RStack& Reference::stackOf(uint32_t cs) {
  if (m_stackSlot.size() < m_nodes.size()) m_stackSlot.resize(m_nodes.size(), -1);
  if (cs < m_stackSlot.size() && m_stackSlot[cs] >= 0)
    return m_stackStore[static_cast<size_t>(m_stackSlot[cs])];
  RStack st;
  std::vector<FrameIdx> leafFirst;
  uint32_t node = cs;
  while (node != kInvalidIdx && node != 0 && node < m_nodes.size()) {
    leafFirst.push_back(m_nodes[node].frame);
    node = m_nodes[node].parent.get();
  }
  for (auto it = leafFirst.rbegin(); it != leafFirst.rend(); ++it) {
    // Read packed fields by value (FrameRec::addr_or_rva is misaligned inside the 12-byte record).
    const uint32_t frModule = m_frames[it->get()].module.get();
    const uint64_t frAddr = m_frames[it->get()].addr_or_rva;
    FnKey key;
    std::string name;
    std::pair<uint64_t, uint64_t> range;
    if (frModule == kInvalidIdx) {
      key.kind = 1;
      key.module = kInvalidIdx;
      key.startRva = frAddr;
      name = unmappedDisplayName(frAddr);
      range = {frAddr, frAddr + 1};
    } else {
      const SymModule& sm = m_sym[frModule];
      const uint64_t base = m_mods[frModule].base;
      const SymFn* hit = nullptr;
      for (const SymFn& f : sm.fns)  // naive linear scan
        if (frAddr >= f.rva && frAddr < f.rva + f.size) hit = &f;
      key.kind = 0;
      key.module = frModule;
      if (hit) {
        key.startRva = hit->rva;
        name = sm.name + "!" + hit->name;
        range = {base + hit->rva, base + hit->rva + hit->size};
      } else {
        key.startRva = frAddr;
        name = hexName(sm.name, frAddr);
        range = {base + frAddr, base + frAddr + 1};
      }
    }
    m_t.names[key] = name;
    st.keys.push_back(key);
    st.ranges.push_back(range);
  }
  for (auto it = st.keys.rbegin(); it != st.keys.rend(); ++it) st.namesLeafFirst.push_back(m_t.names[*it]);
  if (cs >= m_stackSlot.size()) m_stackSlot.resize(cs + 1, -1);
  m_stackSlot[cs] = static_cast<int32_t>(m_stackStore.size());
  m_stackStore.push_back(std::move(st));
  return m_stackStore.back();
}

void Reference::logInterval(uint32_t tid, StateIdx st, uint64_t a, uint64_t b, const ThreadInfo* withStacks,
                            bool withWaker) {
  ThreadTally& tt = m_t.threads[tid];
  tt.stateTicks[st] += b - a;
  if (!m_t.detail) return;
  IntervalLog iv;
  iv.state = st;
  iv.start = a;
  iv.end = b;
  if (withStacks && withStacks->blockingCs != kInvalidIdx)
    iv.blocking = stackOf(withStacks->blockingCs).namesLeafFirst;
  if (withStacks && withWaker) {
    if (withStacks->wakerCs != kInvalidIdx) iv.unblocking = stackOf(withStacks->wakerCs).namesLeafFirst;
    iv.wokeBy = withStacks->waker;
  }
  tt.intervals.push_back(std::move(iv));
}

// Closes the current state interval of `ti` at `end` (not called with end < since).
void Reference::closeSegment(uint32_t tid, ThreadInfo& ti, uint64_t end) {
  if (end <= ti.since) return;
  switch (ti.kind) {
    case Kind::Unknown: logInterval(tid, kStateUnknown, ti.since, end, nullptr, false); break;
    case Kind::Running:
      logInterval(tid, kStateRunning, ti.since, end, nullptr, false);
      ti.runs.push_back({ti.since, end});
      break;
    case Kind::Waiting:
      if (ti.hasReady) {
        logInterval(tid, kStateWaiting, ti.since, ti.readyTs, &ti, true);
        logInterval(tid, kStateReady, ti.readyTs, end, &ti, true);
      } else {
        logInterval(tid, kStateWaiting, ti.since, end, &ti, false);
      }
      break;
    case Kind::Ready: logInterval(tid, kStateReady, ti.since, end, &ti, false); break;
  }
}

Result<void> Reference::passStates(TraceReader& r) {
  RecordCursor<ContextSwitchRec> sw(r, ChunkType::ContextSwitches);
  RecordCursor<ReadyRec> rd(r, ChunkType::ReadyEvents);
  auto info = [&](uint32_t tid) -> ThreadInfo& {
    auto [it, inserted] = m_threads.try_emplace(tid);
    if (inserted) it->second.since = m_h.start_ts;
    return it->second;
  };
  for (;;) {
    const ContextSwitchRec* s = sw.peek();
    const ReadyRec* y = rd.peek();
    if (!s && !y) break;
    if (s && (!y || s->ts <= y->ts)) {
      const ContextSwitchRec c = *s;
      sw.pop();
      m_t.nSwitches += 1;
      if (c.old_tid != 0) {
        ThreadInfo& ti = info(c.old_tid);
        // A thread whose first event is a switch-out is NOT inferred to have been running: its state stays
        // Unknown from the trace start until this first event (attribution-rules.md clarifications).
        if (ti.kind != Kind::Running && ti.kind != Kind::Unknown)
          return makeError(ErrorCode::CorruptData, "switch-out of a thread that is not running");
        ti.seen = true;
        closeSegment(c.old_tid, ti, c.ts);
        ti.kind = (c.old_state == 1 || c.old_state == 3) ? Kind::Ready : Kind::Waiting;
        ti.since = c.ts;
        ti.blockingCs = c.old_thread_stack.get();
        ti.hasReady = false;
        m_t.threads[c.old_tid].switchesOut += 1;
      }
      if (c.new_tid != 0) {
        ThreadInfo& ti = info(c.new_tid);
        if (ti.kind == Kind::Running)
          return makeError(ErrorCode::CorruptData, "switch-in of a thread that is already running");
        closeSegment(c.new_tid, ti, c.ts);
        ti.kind = Kind::Running;
        ti.since = c.ts;
        ti.seen = true;
        m_t.threads[c.new_tid].switchesIn += 1;
      }
    } else {
      const ReadyRec e = *y;
      rd.pop();
      m_t.nReady += 1;
      ThreadInfo& ti = info(e.readied_tid);
      m_t.threads[e.readied_tid].readyEvents += 1;
      if (ti.kind == Kind::Waiting && !ti.hasReady) {
        ti.hasReady = true;
        ti.readyTs = e.ts;
        ti.waker = e.readying_tid;
        ti.wakerCs = e.readying_stack.get();
      }
    }
  }
  if (sw.failed()) return makeError(ErrorCode::CorruptData, sw.error());
  if (rd.failed()) return makeError(ErrorCode::CorruptData, rd.error());
  for (auto& [tid, ti] : m_threads) closeSegment(tid, ti, m_h.end_ts);
  return ok();
}

Result<void> Reference::passSamples(TraceReader& r) {
  uint32_t lastTid = kInvalidIdx;
  ThreadInfo* last = nullptr;
  std::vector<std::byte> payload;
  for (const FooterRec c : std::vector<FooterRec>(r.chunks().begin(), r.chunks().end())) {
    if (c.type != static_cast<uint32_t>(ChunkType::Samples)) continue;
    TACHYON_TRY(r.readChunk(c, payload));
    SampleBatch b;
    TACHYON_TRY(TraceReader::decodeSamples(c, payload, b));
    for (size_t i = 0; i < b.size(); ++i) {
      const uint64_t ts = b.ts[i];
      const uint32_t tid = b.tid[i];
      const uint32_t cs = b.callstack[i].get();
      m_t.nSamples += 1;
      if (tid != lastTid) {
        const auto it = m_threads.find(tid);
        last = it == m_threads.end() ? nullptr : &it->second;
        lastTid = tid;
      }
      if (!last) continue;
      ThreadInfo& ti = *last;
      // Rule 1: weight = interval clipped to the end of the enclosing Running interval, else 0.
      while (ti.cursor < ti.runs.size() && ti.runs[ti.cursor].second <= ts) ++ti.cursor;
      uint64_t weight = 0;
      ThreadTally& tt = m_t.threads[tid];
      tt.samples += 1;
      if (ti.cursor < ti.runs.size() && ti.runs[ti.cursor].first <= ts) {
        const uint64_t room = ti.runs[ti.cursor].second - ts;
        weight = std::min<uint64_t>(m_interval, room);
        if (room < m_interval) tt.clipped += 1;
      } else {
        tt.zeroWeight += 1;
      }
      if (cs >= ti.acc.size()) ti.acc.resize(static_cast<size_t>(cs) + 1);
      ti.acc[cs].samples += 1;
      ti.acc[cs].ticks += weight;
      tt.sampleTicks += weight;
      if (m_needLog) m_log.push_back(SampleLog{ts, tid, cs, weight});
    }
  }
  for (auto& [tid, ti] : m_threads) {
    for (size_t cs = 0; cs < ti.acc.size(); ++cs) {
      if (ti.acc[cs].samples == 0) continue;
      m_t.addStack(tid, stackOf(static_cast<uint32_t>(cs)).keys, false, ti.acc[cs].ticks, ti.acc[cs].samples);
    }
  }
  return ok();
}

Result<void> Reference::passScopes(TraceReader& r) {
  TACHYON_TRY_ASSIGN(auto scopeEvents, r.readScopeEvents());
  TACHYON_TRY_ASSIGN(auto fiberEvents, r.readFiberEvents());
  m_t.nScopeEvents = scopeEvents.size();
  m_t.nFiberEvents = fiberEvents.size();
  struct Open {
    std::string id;
    uint64_t begin, segStart, callerAddr;
    uint32_t depth;
  };
  std::map<std::pair<uint32_t, uint64_t>, std::vector<Open>> stacks;  // (tid, fiber) -> open scopes
  std::map<uint32_t, uint64_t> currentFiber;
  const bool hasFibers = !fiberEvents.empty();
  size_t si = 0, fi = 0;
  while (si < scopeEvents.size() || fi < fiberEvents.size()) {
    const bool takeFiber =
        fi < fiberEvents.size() && (si >= scopeEvents.size() || fiberEvents[fi].ts <= scopeEvents[si].ts);
    if (takeFiber) {
      const FiberEventRec& e = fiberEvents[fi++];
      if (e.kind == kFiberRegister) {
        m_t.fibers[e.fiber_id];
      } else if (e.kind == kFiberSwitchBegin) {
        auto& st = stacks[{e.tid, e.fiber_id}];
        for (Open& o : st) {
          if (e.ts > o.segStart)
            m_t.fibers[e.fiber_id].push_back(FiberSegment{e.tid, o.id, o.depth, o.begin, o.segStart, e.ts});
          o.segStart = e.ts;
        }
        currentFiber[e.tid] = 0;
      } else if (e.kind == kFiberSwitchEnd) {
        currentFiber[e.tid] = e.fiber_id;
        for (Open& o : stacks[{e.tid, e.fiber_id}]) o.segStart = e.ts;
      }
      continue;
    }
    const ScopeEventRec& e = scopeEvents[si++];
    const uint64_t fiber = currentFiber.count(e.tid) ? currentFiber[e.tid] : 0;
    auto& st = stacks[{e.tid, fiber}];
    if (e.kind == kScopeBegin) {
      st.push_back(Open{std::string(m_strings.get(e.id)), e.ts, e.ts, e.caller_addr,
                        static_cast<uint32_t>(st.size())});
    } else {
      if (st.empty()) return makeError(ErrorCode::CorruptData, "scope end without begin");
      const Open o = st.back();
      st.pop_back();
      m_t.scopeDurations[o.id].push_back(e.ts - o.begin);
      if (hasFibers && fiber != 0 && e.ts > o.segStart)
        m_t.fibers[fiber].push_back(FiberSegment{e.tid, o.id, o.depth, o.begin, o.segStart, e.ts});
      m_insts.push_back(Inst{e.tid, o.id, o.begin, e.ts, o.callerAddr, o.depth});
    }
  }
  for (const auto& entry : stacks)
    if (!entry.second.empty()) return makeError(ErrorCode::CorruptData, "scope begin without end");
  for (const Inst& in : m_insts) {
    FnKey k;
    k.kind = 2;
    k.scope = in.id;
    m_t.names[k] = "scope:" + in.id;
  }
  return ok();
}

// Rule 5, applied by scanning every instance for every sample.
void Reference::blend() {
  for (const SampleLog& s : m_log) {
    const RStack& st = stackOf(s.cs);
    std::vector<const Inst*> open;
    for (const Inst& in : m_insts)
      if (in.tid == s.tid && in.begin <= s.ts && s.ts < in.end) open.push_back(&in);
    std::sort(open.begin(), open.end(), [](const Inst* a, const Inst* b) { return a->depth < b->depth; });
    std::vector<size_t> slot(open.size());
    std::vector<bool> matched(open.size());
    size_t prev = 0;
    for (size_t i = 0; i < open.size(); ++i) {
      size_t found = st.ranges.size();
      for (size_t j = st.ranges.size(); j-- > 0;) {
        if (open[i]->callerAddr >= st.ranges[j].first && open[i]->callerAddr < st.ranges[j].second) {
          found = j;
          break;
        }
      }
      matched[i] = found != st.ranges.size();
      slot[i] = std::max(matched[i] ? found : size_t{0}, prev);
      prev = slot[i];
    }
    std::vector<FnKey> out;
    for (size_t j = 0; j < st.keys.size(); ++j) {
      out.push_back(st.keys[j]);
      for (size_t i = 0; i < open.size(); ++i) {
        if (slot[i] != j) continue;
        PlacementKey pk;
        pk.scope = open[i]->id;
        pk.matched = matched[i];
        pk.parent = out.back();
        Placement& p = m_t.placements[pk];
        p.samples += 1;
        p.ticks += s.weight;
        FnKey sk;
        sk.kind = 2;
        sk.scope = open[i]->id;
        out.push_back(sk);
      }
    }
    m_t.addStack(s.tid, out, true, s.weight, 1);
  }
}

void Reference::frames(const std::vector<FrameMarkRec>& marks) {
  if (marks.empty()) return;
  m_t.nFrameMarks = marks.size();
  std::map<std::pair<uint32_t, std::string>, std::vector<FrameMarkRec>> groups;
  for (const FrameMarkRec& f : marks) groups[{f.tid, std::string(m_strings.get(f.name))}].push_back(f);
  for (auto& [key, ms] : groups) {
    std::stable_sort(ms.begin(), ms.end(),
                     [](const FrameMarkRec& a, const FrameMarkRec& b) { return a.ts < b.ts; });
    for (size_t i = 0; i + 1 < ms.size(); ++i) {
      FrameTally f;
      f.tid = key.first;
      f.name = key.second;
      f.number = ms[i].frame_number;
      f.start = ms[i].ts;
      f.end = ms[i + 1].ts;
      for (const auto& [tid, ti] : m_threads) {
        FrameThreadTally ft;
        for (const auto& run : ti.runs) {
          const uint64_t lo = std::max(run.first, f.start), hi = std::min(run.second, f.end);
          if (hi > lo) ft.runningTicks += hi - lo;
        }
        f.threads[tid] = ft;
      }
      for (const SampleLog& s : m_log)
        if (s.ts >= f.start && s.ts < f.end && f.threads.count(s.tid))
          f.threads[s.tid].sampleTicks += s.weight;
      m_t.frames.push_back(std::move(f));
    }
  }
}

Result<void> Reference::plots(TraceReader& r) {
  TACHYON_TRY_ASSIGN(auto recs, r.readPlotSamples());
  m_t.nPlots = recs.size();
  for (const PlotSampleRec& p : recs) {
    // Copy packed fields before use: they may be misaligned and must not be bound to references.
    const uint64_t ts = p.ts;
    const double value = p.value;
    const StringIdx nameIdx = p.name;
    auto [it, inserted] = m_t.plots.try_emplace(std::string(m_strings.get(nameIdx)));
    PlotTally& pt = it->second;
    if (inserted) {
      pt.min = pt.max = value;
      pt.firstTs = ts;
    }
    pt.count += 1;
    pt.min = std::min(pt.min, value);
    pt.max = std::max(pt.max, value);
    pt.firstTs = std::min(pt.firstTs, ts);
    pt.lastTs = std::max(pt.lastTs, ts);
  }
  return ok();
}

Result<Tally> Reference::run() {
  TACHYON_TRY(loadSymbols());
  TACHYON_TRY_ASSIGN(TraceReader reader, TraceReader::open(m_tpath));
  m_h = reader.header();
  TACHYON_TRY_ASSIGN(std::string infoText, reader.readCaptureInfo());
  const json info = json::parse(infoText, nullptr, false);
  if (info.is_discarded() || !info.is_object())
    return makeError(ErrorCode::CorruptData, "bad CaptureInfo JSON");
  const uint64_t samplingHz = getU64(info, "sampling_hz", 0);
  if (samplingHz == 0 || m_h.clock_frequency_hz % samplingHz != 0)
    return makeError(ErrorCode::CorruptData,
                     "CaptureInfo sampling_hz is missing or does not divide the clock");
  m_interval = m_h.clock_frequency_hz / samplingHz;

  const json* tg = member(info, "tracegen");
  const json* opts = member(info, "capture_options");
  if (!tg || !opts || !tg->is_object() || !opts->is_object())
    return makeError(ErrorCode::CorruptData, "CaptureInfo lacks the tracegen section");
  m_t.scenario = getStr(*tg, "scenario", "");
  m_t.generator = getStr(info, "generator", "");
  m_t.seed = getU64(*opts, "seed", 0);
  m_t.clockHz = m_h.clock_frequency_hz;
  m_t.samplingHz = samplingHz;
  m_t.sampleInterval = m_interval;
  m_t.startTs = m_h.start_ts;
  m_t.endTs = m_h.end_ts;
  m_t.partial = reader.isPartial();
  m_t.blend = getBool(*tg, "blend", false);
  m_t.detail = getBool(*tg, "detail", false);

  TACHYON_TRY_ASSIGN(m_strings, reader.readStrings());
  TACHYON_TRY_ASSIGN(m_mods, reader.readModuleInfos());
  TACHYON_TRY_ASSIGN(m_frames, reader.readFrames());
  TACHYON_TRY_ASSIGN(m_nodes, reader.readCallstackNodes());
  if (m_sym.size() != m_mods.size())
    return makeError(ErrorCode::CorruptData, "symbols file / module table mismatch");
  for (size_t i = 0; i < m_mods.size(); ++i) m_t.modules.push_back({m_sym[i].name, m_mods[i].base});

  TACHYON_TRY_ASSIGN(auto threads, reader.readThreads());
  for (const ThreadRec& t : threads) {
    m_t.threads[t.tid].name = std::string(m_strings.get(t.name));
    m_threads.try_emplace(t.tid).first->second.since = m_h.start_ts;
  }
  TACHYON_TRY_ASSIGN(auto marks, reader.readFrameMarks());
  m_needLog = m_t.blend || !marks.empty();
  TACHYON_TRY(passStates(reader));
  TACHYON_TRY(passSamples(reader));
  TACHYON_TRY(passScopes(reader));
  if (m_t.blend) blend();
  frames(marks);
  TACHYON_TRY(plots(reader));
  TACHYON_TRY_ASSIGN(auto markers, reader.readMarkers());
  TACHYON_TRY_ASSIGN(auto renames, reader.readThreadNames());
  m_t.nMarkers = markers.size();
  m_t.nThreadNames = renames.size();
  return std::move(m_t);
}

}  // namespace

Result<Tally> computeReference(const std::filesystem::path& tachyonPath,
                               const std::filesystem::path& symbolsPath) {
  Reference ref(tachyonPath, symbolsPath);
  return ref.run();
}

}  // namespace tachyon::tracegen
