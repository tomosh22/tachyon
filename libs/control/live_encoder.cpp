#include "control/live_encoder.h"

#include "core/hash.h"
#include "core/log.h"
#include "trace/trace_format.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <tuple>
#include <unordered_map>

namespace tachyon::control {

namespace {

using Clock = std::chrono::steady_clock;
using model::live_wire::RecordType;

constexpr size_t kMaxStringLen = 16 * 1024;
constexpr uint64_t kKernelBase = 0xFFFF800000000000ull;
constexpr size_t kStackCacheSize = 1u << 14;
constexpr uint32_t kOverflowTid = 0xFFFFFFFEu;

// Open-addressing set of indices into an external array (4 bytes per slot, linear probing).
class IdxTable {
 public:
  IdxTable() : m_slots(1024, kInvalidIdx) {}

  template <typename Eq>
  uint32_t find(uint64_t hash, Eq&& eq) const {
    const size_t mask = m_slots.size() - 1;
    for (size_t i = hash & mask;; i = (i + 1) & mask) {
      const uint32_t v = m_slots[i];
      if (v == kInvalidIdx) return kInvalidIdx;
      if (eq(v)) return v;
    }
  }
  // `hashOf(idx)` recomputes the hash of an existing entry when the table grows.
  template <typename HashOf>
  void insert(uint64_t hash, uint32_t idx, HashOf&& hashOf) {
    if ((m_count + 1) * 2 > m_slots.size()) grow(hashOf);
    place(hash, idx);
    ++m_count;
  }
  size_t bytes() const { return m_slots.size() * sizeof(uint32_t); }

 private:
  void place(uint64_t hash, uint32_t idx) {
    const size_t mask = m_slots.size() - 1;
    size_t i = hash & mask;
    while (m_slots[i] != kInvalidIdx) i = (i + 1) & mask;
    m_slots[i] = idx;
  }
  template <typename HashOf>
  void grow(HashOf&& hashOf) {
    std::vector<uint32_t> old;
    old.swap(m_slots);
    m_slots.assign(old.size() * 2, kInvalidIdx);
    for (uint32_t v : old)
      if (v != kInvalidIdx) place(hashOf(v), v);
  }
  std::vector<uint32_t> m_slots;
  size_t m_count = 0;
};

uint64_t frameHash(uint32_t module, uint64_t addr) {
  return core::hashU64(addr ^ (static_cast<uint64_t>(module) * 0x9e3779b97f4a7c15ull));
}

template <typename T>
void appendRaw(std::string& out, const T& v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(T));
}

}  // namespace

namespace {
// Bytes of the whole records at the start of `blob` that fit `maxBytes` (at least one record; a torn tail is
// included in the last piece).
size_t recordPrefix(std::string_view blob, size_t maxBytes) {
  size_t pos = 0;
  while (pos < blob.size()) {
    if (blob.size() - pos < sizeof(model::live_wire::RecordHeader)) return blob.size();
    model::live_wire::RecordHeader h;
    std::memcpy(&h, blob.data() + pos, sizeof(h));
    const size_t recBytes = std::min(sizeof(h) + static_cast<size_t>(h.byte_len), blob.size() - pos);
    if (pos != 0 && pos + recBytes > maxBytes) break;
    pos += recBytes;
  }
  return pos;
}
}  // namespace

std::vector<std::string_view> splitRecords(std::string_view blob, size_t maxBytes) {
  std::vector<std::string_view> out;
  while (!blob.empty()) {
    const size_t n = recordPrefix(blob, maxBytes);
    out.push_back(blob.substr(0, n));
    blob.remove_prefix(n);
  }
  return out;
}

bool LiveKeyframe::next(std::string& chunk) {
  if (m_defIndex < m_defs.size()) {
    chunk = std::move(m_defs[m_defIndex++]);
    return true;
  }
  while (m_batchIndex < m_batches.size()) {
    const LiveBatch& b = m_batches[m_batchIndex];
    const std::string_view events = std::string_view(*b.data).substr(b.defsBytes);
    if (m_offset >= events.size()) {
      ++m_batchIndex;
      m_offset = 0;
      continue;
    }
    // One piece: whole records from m_offset up to kMaxEventBytes.
    const std::string_view rest = events.substr(m_offset);
    const size_t n = recordPrefix(rest, model::live_wire::kMaxEventBytes);
    chunk.assign(rest.substr(0, n));
    m_offset += n;
    return true;
  }
  return false;
}

struct LiveEncoder::Impl {
  LiveEncoderConfig config;
  mutable std::mutex mutex;
  std::condition_variable batchCv;
  std::condition_variable tickCv;
  std::thread ticker;
  bool stopTicker = false;
  bool ended = false;

  capture::LiveSessionInfo info;
  bool haveInfo = false;
  bool infoPending = false;  // the Info record has not gone out in a batch yet
  uint64_t windowTicks = 30ull * 1'000'000'000ull;

  // ---- tables ----
  std::deque<std::string> strings;
  std::unordered_map<std::string_view, uint32_t> stringIndex;
  uint64_t stringBytes = 0;
  size_t stringsEmitted = 0;

  struct ProcEntry {
    trace::ProcessRec rec{};
    bool dirty = false;
  };
  std::vector<ProcEntry> procs;
  std::unordered_map<uint32_t, uint32_t> procByPid;
  std::vector<uint32_t> dirtyProcs;

  struct ThreadEntry {
    trace::ThreadRec rec{};
    bool emitted = false;
    bool dirty = false;       // needs a full ThreadDef
    bool renamePending = false;
  };
  std::vector<ThreadEntry> threads;
  std::unordered_map<uint64_t, uint32_t> threadByKey;
  std::vector<uint32_t> dirtyThreads;
  std::vector<uint32_t> renamedThreads;
  uint32_t overflowThread = kInvalidIdx;
  uint32_t lastThreadIdx = kInvalidIdx;
  uint64_t lastThreadKey = 0;

  struct ModEntry {
    trace::ModuleRec rec{};
    bool dirty = false;
    bool loaded = false;
  };
  std::vector<ModEntry> modules;
  struct ModKey {
    uint32_t pid;
    uint64_t base, size;
    uint32_t path;
    bool operator<(const ModKey& o) const {
      return std::tie(pid, base, size, path) < std::tie(o.pid, o.base, o.size, o.path);
    }
  };
  std::map<ModKey, uint32_t> moduleByKey;
  std::vector<uint32_t> dirtyModules;
  struct Range {
    uint64_t base, end;
    uint32_t idx;
  };
  std::unordered_map<uint32_t, std::vector<Range>> rangesByPid;  // sorted by base
  uint32_t moduleEpoch = 1;

  std::vector<trace::FrameRec> frames;
  IdxTable frameTable;
  size_t framesEmitted = 0;
  uint32_t overflowFrame = kInvalidIdx;

  std::vector<trace::CallstackNodeRec> nodes;
  IdxTable nodeTable;
  size_t nodesEmitted = 0;

  struct StackCacheEntry {
    uint64_t hash = 0;
    uint32_t pid = 0, epoch = 0, count = 0, node = 0;
  };
  std::vector<StackCacheEntry> stackCache = std::vector<StackCacheEntry>(kStackCacheSize);

  // ---- pending batch ----
  std::vector<uint64_t> sTs;
  std::vector<uint32_t> sThread, sNode;
  std::vector<uint16_t> sCpu, sFlags;
  std::vector<trace::ContextSwitchRec> switches;
  std::vector<trace::ReadyRec> readies;
  std::vector<trace::ScopeEventRec> scopes;
  std::vector<trace::FrameMarkRec> frameMarks;
  std::vector<trace::MarkerRec> markers;
  std::vector<trace::PlotSampleRec> plots;
  uint64_t pendingDropped = 0;
  bool timeDirty = false;
  size_t pendingBytes = 0;
  uint64_t now = 0;
  uint64_t lastBatchNow = 0;
  Clock::time_point lastFlush = Clock::now();

  // ---- retained batches ----
  std::deque<LiveBatch> batches;
  uint64_t seq = 0;
  uint64_t retainedBytes = 0;
  LiveEncoderStats counters;

  explicit Impl(LiveEncoderConfig c) : config(std::move(c)) {
    windowTicks = static_cast<uint64_t>(config.windowSeconds) * 1'000'000'000ull;
    internStringLocked("");
    internStringLocked(model::live_wire::kOverflowName);  // index 1
    nodes.push_back(trace::CallstackNodeRec{CallstackIdx::invalid(), FrameIdx::invalid()});
    nodesEmitted = 1;  // the root is implicit in every model
  }

  // ---------------------------------------------------------------------------------------------------
  // interning

  uint32_t internStringLocked(std::string_view s) {
    if (s.size() > kMaxStringLen) s = s.substr(0, kMaxStringLen);
    auto it = stringIndex.find(s);
    if (it != stringIndex.end()) return it->second;
    if (strings.size() >= config.maxStrings || stringBytes + s.size() > model::live_wire::kMaxStringBytes) {
      ++counters.overflows;
      return model::live_wire::kOverflowString;
    }
    strings.emplace_back(s);
    const auto idx = static_cast<uint32_t>(strings.size() - 1);
    stringIndex.emplace(std::string_view(strings.back()), idx);
    stringBytes += s.size();
    pendingBytes += s.size() + 12;
    return idx;
  }

  uint32_t internThreadLocked(uint32_t pid, uint32_t tid) {
    const uint64_t key = (static_cast<uint64_t>(pid) << 32) | tid;
    if (lastThreadIdx != kInvalidIdx && key == lastThreadKey) return lastThreadIdx;
    auto it = threadByKey.find(key);
    uint32_t idx;
    if (it != threadByKey.end()) {
      idx = it->second;
    } else if (threads.size() + 1 >= config.maxThreads) {
      ++counters.overflows;
      if (overflowThread == kInvalidIdx) {
        ThreadEntry e;
        e.rec.tid = kOverflowTid;
        e.rec.name = StringIdx(model::live_wire::kOverflowString);
        e.dirty = true;
        threads.push_back(e);
        overflowThread = static_cast<uint32_t>(threads.size() - 1);
        dirtyThreads.push_back(overflowThread);
        pendingBytes += 40;
      }
      return overflowThread;
    } else {
      ThreadEntry e;
      e.rec.tid = tid;
      e.rec.pid = pid;
      e.rec.name = StringIdx(0);
      e.dirty = true;
      threads.push_back(e);
      idx = static_cast<uint32_t>(threads.size() - 1);
      threadByKey.emplace(key, idx);
      dirtyThreads.push_back(idx);
      pendingBytes += 40;
    }
    lastThreadIdx = idx;
    lastThreadKey = key;
    return idx;
  }

  uint32_t frameForLocked(uint32_t pid, uint64_t ip) {
    uint32_t module = kInvalidIdx;
    uint64_t value = ip;
    if (ip < kKernelBase) {
      auto it = rangesByPid.find(pid);
      if (it != rangesByPid.end()) {
        const auto& v = it->second;
        auto r = std::upper_bound(v.begin(), v.end(), ip, [](uint64_t a, const Range& x) { return a < x.base; });
        if (r != v.begin()) {
          --r;
          if (ip < r->end) {
            module = r->idx;
            value = ip - r->base;
          }
        }
      }
    }
    const uint64_t h = frameHash(module, value);
    const uint32_t found = frameTable.find(h, [&](uint32_t i) {
      return frames[i].module.get() == module && frames[i].addr_or_rva == value;
    });
    if (found != kInvalidIdx) return found;
    if (frames.size() + 1 >= config.maxFrames) {
      ++counters.overflows;
      if (overflowFrame == kInvalidIdx) {
        frames.push_back(trace::FrameRec{ModuleIdx::invalid(), ~0ull});
        overflowFrame = static_cast<uint32_t>(frames.size() - 1);
        pendingBytes += 24;
      }
      return overflowFrame;
    }
    frames.push_back(trace::FrameRec{ModuleIdx(module), value});
    const auto idx = static_cast<uint32_t>(frames.size() - 1);
    frameTable.insert(h, idx, [&](uint32_t i) { return frameHash(frames[i].module.get(), frames[i].addr_or_rva); });
    pendingBytes += 24;
    return idx;
  }

  uint32_t nodeForLocked(uint32_t parent, uint32_t frame) {
    const uint64_t h = core::hashU32Pair(parent, frame);
    const uint32_t found = nodeTable.find(h, [&](uint32_t i) {
      return nodes[i].parent.get() == parent && nodes[i].frame.get() == frame;
    });
    if (found != kInvalidIdx) return found;
    if (nodes.size() >= config.maxNodes) {
      ++counters.overflows;
      return parent;  // the stack is cut here
    }
    nodes.push_back(trace::CallstackNodeRec{CallstackIdx(parent), FrameIdx(frame)});
    const auto idx = static_cast<uint32_t>(nodes.size() - 1);
    nodeTable.insert(h, idx, [&](uint32_t i) { return core::hashU32Pair(nodes[i].parent.get(), nodes[i].frame.get()); });
    pendingBytes += 20;
    return idx;
  }

  uint32_t stackLocked(uint32_t pid, std::span<const uint64_t> ips) {
    if (ips.empty()) return 0;
    const uint64_t h = core::hashBytes(ips.data(), ips.size() * sizeof(uint64_t), pid);
    StackCacheEntry& ce = stackCache[(h >> 20) & (kStackCacheSize - 1)];
    if (ce.hash == h && ce.pid == pid && ce.epoch == moduleEpoch && ce.count == ips.size()) return ce.node;
    uint32_t node = 0;
    for (size_t i = ips.size(); i > 0; --i) node = nodeForLocked(node, frameForLocked(pid, ips[i - 1]));
    ce = StackCacheEntry{h, pid, moduleEpoch, static_cast<uint32_t>(ips.size()), node};
    return node;
  }

  void noteTsLocked(uint64_t ts) {
    if (ts > now) now = ts;
  }

  // ---------------------------------------------------------------------------------------------------
  // batches

  bool pendingAny() const {
    return infoPending || !sTs.empty() || !switches.empty() || !readies.empty() || !scopes.empty() || !frameMarks.empty() ||
           !markers.empty() || !plots.empty() || pendingDropped != 0 || !dirtyProcs.empty() ||
           !dirtyThreads.empty() || !renamedThreads.empty() || !dirtyModules.empty() ||
           stringsEmitted < strings.size() || framesEmitted < frames.size() || nodesEmitted < nodes.size();
  }

  template <typename T>
  static void putArray(std::string& out, RecordType type, std::vector<T>& v) {
    if (v.empty()) return;
    model::live_wire::putRecord(out, type, v.data(), v.size() * sizeof(T));
    v.clear();
  }

  static void putStringDef(std::string& out, uint32_t idx, std::string_view s) {
    model::live_wire::putIndexedRecord(out, RecordType::StringDef, idx, s.data(), s.size());
  }

  uint64_t flushLocked() {
    if (!pendingAny() && !(timeDirty && now != lastBatchNow)) {
      lastFlush = Clock::now();
      return 0;
    }
    using namespace model::live_wire;
    std::string defs;
    defs.reserve(pendingBytes + 64);
    if (infoPending) {
      InfoPayload p{info.clockFrequencyHz, info.platform, info.samplingHz, info.startTs};
      putRecord(defs, RecordType::Info, &p, sizeof(p));
      infoPending = false;
    }
    for (; stringsEmitted < strings.size(); ++stringsEmitted)
      putStringDef(defs, static_cast<uint32_t>(stringsEmitted), strings[stringsEmitted]);
    for (uint32_t i : dirtyProcs) {
      procs[i].dirty = false;
      putIndexedRecord(defs, RecordType::ProcessDef, i, &procs[i].rec, sizeof(trace::ProcessRec));
    }
    dirtyProcs.clear();
    for (uint32_t i : dirtyThreads) {
      ThreadEntry& t = threads[i];
      t.dirty = false;
      t.emitted = true;
      t.renamePending = false;
      putIndexedRecord(defs, RecordType::ThreadDef, i, &t.rec, sizeof(trace::ThreadRec));
    }
    dirtyThreads.clear();
    for (uint32_t i : renamedThreads) {
      ThreadEntry& t = threads[i];
      if (!t.renamePending) continue;
      t.renamePending = false;
      ThreadNamePayload p{i, t.rec.name.get()};
      putRecord(defs, RecordType::ThreadName, &p, sizeof(p));
    }
    renamedThreads.clear();
    for (uint32_t i : dirtyModules) {
      modules[i].dirty = false;
      putIndexedRecord(defs, RecordType::ModuleDef, i, &modules[i].rec, sizeof(trace::ModuleRec));
    }
    dirtyModules.clear();
    for (; framesEmitted < frames.size(); ++framesEmitted)
      putIndexedRecord(defs, RecordType::FrameDef, static_cast<uint32_t>(framesEmitted), &frames[framesEmitted],
                       sizeof(trace::FrameRec));
    for (; nodesEmitted < nodes.size(); ++nodesEmitted)
      putIndexedRecord(defs, RecordType::NodeDef, static_cast<uint32_t>(nodesEmitted), &nodes[nodesEmitted],
                       sizeof(trace::CallstackNodeRec));

    std::string events;
    events.reserve(pendingBytes + 64);
    if (!sTs.empty()) {
      const auto count = static_cast<uint32_t>(sTs.size());
      RecordHeader h{static_cast<uint32_t>(RecordType::Samples),
                     static_cast<uint32_t>(4 + static_cast<size_t>(count) * 20)};
      events.append(reinterpret_cast<const char*>(&h), sizeof(h));
      appendRaw(events, count);
      events.append(reinterpret_cast<const char*>(sTs.data()), count * 8ull);
      events.append(reinterpret_cast<const char*>(sThread.data()), count * 4ull);
      events.append(reinterpret_cast<const char*>(sNode.data()), count * 4ull);
      events.append(reinterpret_cast<const char*>(sCpu.data()), count * 2ull);
      events.append(reinterpret_cast<const char*>(sFlags.data()), count * 2ull);
      sTs.clear();
      sThread.clear();
      sNode.clear();
      sCpu.clear();
      sFlags.clear();
    }
    putArray(events, RecordType::ContextSwitches, switches);
    putArray(events, RecordType::Ready, readies);
    putArray(events, RecordType::Scopes, scopes);
    putArray(events, RecordType::FrameMarks, frameMarks);
    putArray(events, RecordType::Markers, markers);
    putArray(events, RecordType::Plots, plots);
    if (pendingDropped != 0) {
      putRecord(events, RecordType::Dropped, &pendingDropped, sizeof(pendingDropped));
      pendingDropped = 0;
    }
    putRecord(events, RecordType::TimeAdvance, &now, sizeof(now));
    lastBatchNow = now;
    timeDirty = false;
    pendingBytes = 0;

    LiveBatch b;
    b.seq = ++seq;
    b.defsBytes = defs.size();
    b.endTs = now;
    defs += events;
    b.data = std::make_shared<const std::string>(std::move(defs));
    retainedBytes += b.data->size();
    counters.batches = b.seq;
    batches.push_back(std::move(b));
    trimRetainedLocked();
    lastFlush = Clock::now();
    batchCv.notify_all();
    return seq;
  }

  void trimRetainedLocked() {
    while (batches.size() > 1 && (batches.front().endTs + windowTicks < now ||
                                  retainedBytes > config.maxRetainedBytes)) {
      retainedBytes -= batches.front().data->size();
      batches.pop_front();
    }
  }

  void maybeFlushLocked() {
    if (pendingBytes >= config.batchBytes) flushLocked();
  }

  void tickLoop() {
    std::unique_lock lock(mutex);
    while (!stopTicker) {
      tickCv.wait_for(lock, std::chrono::milliseconds(std::max(5, config.batchMs / 5)),
                      [this] { return stopTicker; });
      if (stopTicker) break;
      if (!ended && Clock::now() - lastFlush >= std::chrono::milliseconds(config.batchMs)) flushLocked();
    }
  }
};

// ---------------------------------------------------------------------------------------------------------

LiveEncoder::LiveEncoder(LiveEncoderConfig config) : m_impl(std::make_unique<Impl>(std::move(config))) {
  if (m_impl->config.autoTick) m_impl->ticker = std::thread([this] { m_impl->tickLoop(); });
}

LiveEncoder::~LiveEncoder() {
  finish();
  {
    std::lock_guard lock(m_impl->mutex);
    m_impl->stopTicker = true;
  }
  m_impl->tickCv.notify_all();
  if (m_impl->ticker.joinable()) m_impl->ticker.join();
}

void LiveEncoder::onSessionInfo(const capture::LiveSessionInfo& info) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  m.info = info;
  m.haveInfo = true;
  m.infoPending = true;
  if (info.clockFrequencyHz != 0)
    m.windowTicks = static_cast<uint64_t>(static_cast<double>(m.config.windowSeconds) *
                                          static_cast<double>(info.clockFrequencyHz));
  m.noteTsLocked(info.startTs);
}

void LiveEncoder::onProcess(const capture::LiveProcess& p) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended || p.pid == 0) return;
  auto it = m.procByPid.find(p.pid);
  uint32_t idx;
  if (it != m.procByPid.end() &&
      !(m.procs[it->second].rec.end_ts != 0 && p.startTs > m.procs[it->second].rec.end_ts)) {
    idx = it->second;
  } else {
    if (m.procs.size() >= m.config.maxProcesses) {
      ++m.counters.overflows;
      return;
    }
    m.procs.emplace_back();
    idx = static_cast<uint32_t>(m.procs.size() - 1);
    m.procByPid[p.pid] = idx;
  }
  Impl::ProcEntry& e = m.procs[idx];
  e.rec.pid = p.pid;
  e.rec.parent_pid = p.ppid;
  if (e.rec.start_ts == 0 || p.startTs != 0) e.rec.start_ts = p.startTs;
  e.rec.end_ts = p.endTs;
  e.rec.name = StringIdx(m.internStringLocked(p.name));
  e.rec.command_line = StringIdx(m.internStringLocked(p.commandLine));
  if (!e.dirty) {
    e.dirty = true;
    m.dirtyProcs.push_back(idx);
  }
  m.pendingBytes += 40;
  m.noteTsLocked(p.startTs);
  m.maybeFlushLocked();
}

void LiveEncoder::onThread(const capture::LiveThread& t) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended || t.tid == 0) return;
  const uint32_t idx = m.internThreadLocked(t.pid, t.tid);
  if (idx == m.overflowThread) return;
  Impl::ThreadEntry& e = m.threads[idx];
  const uint32_t targetFlag = t.isTarget ? trace::kThreadFlagTargetProcess : 0;
  const bool otherChange = (t.endTs != 0 && e.rec.end_ts != t.endTs) || (e.rec.flags | targetFlag) != e.rec.flags;
  if (e.rec.start_ts == 0 || (t.startTs != 0 && t.startTs < e.rec.start_ts)) e.rec.start_ts = t.startTs;
  if (t.endTs != 0) e.rec.end_ts = t.endTs;  // a thread does not come back to life
  e.rec.flags |= targetFlag;
  const StringIdx name(m.internStringLocked(t.name));
  const bool renamed = name != e.rec.name;
  e.rec.name = name;
  if (!e.emitted || e.dirty) {
    if (!e.dirty) {
      e.dirty = true;
      m.dirtyThreads.push_back(idx);
    }
  } else if (otherChange) {
    e.dirty = true;  // a full redefinition (carries the name too)
    m.dirtyThreads.push_back(idx);
  } else if (renamed && !e.renamePending) {
    e.renamePending = true;
    m.renamedThreads.push_back(idx);
  }
  m.pendingBytes += 40;
  m.noteTsLocked(t.startTs);
  m.maybeFlushLocked();
}

void LiveEncoder::onModule(const capture::LiveModule& mod) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  const Impl::ModKey key{mod.pid, mod.base, mod.size, m.internStringLocked(mod.path)};
  auto it = m.moduleByKey.find(key);
  uint32_t idx;
  if (it != m.moduleByKey.end()) {
    idx = it->second;
  } else {
    if (mod.unloadTs != 0) return;  // an unload of something never reported
    if (m.modules.size() >= m.config.maxModules) {
      ++m.counters.overflows;
      return;
    }
    m.modules.emplace_back();
    idx = static_cast<uint32_t>(m.modules.size() - 1);
    m.moduleByKey.emplace(key, idx);
  }
  Impl::ModEntry& e = m.modules[idx];
  trace::ModuleRec& r = e.rec;
  r.pid = mod.pid;
  r.base = mod.base;
  r.size = mod.size;
  r.path = StringIdx(key.path);
  r.id_kind = mod.idKind;
  r.id_len = std::min<uint8_t>(mod.idLen, 32);
  std::memcpy(r.id, mod.id.data(), 32);
  r.age = mod.age;
  r.load_ts = mod.loadTs;
  r.unload_ts = mod.unloadTs;
  auto& ranges = m.rangesByPid[mod.pid];
  ranges.erase(std::remove_if(ranges.begin(), ranges.end(), [&](const Impl::Range& x) { return x.idx == idx; }),
               ranges.end());
  e.loaded = mod.unloadTs == 0;
  if (e.loaded) {
    Impl::Range rg{mod.base, mod.base + std::max<uint64_t>(mod.size, 1), idx};
    ranges.insert(std::upper_bound(ranges.begin(), ranges.end(), rg,
                                   [](const Impl::Range& a, const Impl::Range& b) { return a.base < b.base; }),
                  rg);
  }
  ++m.moduleEpoch;
  if (!e.dirty) {
    e.dirty = true;
    m.dirtyModules.push_back(idx);
  }
  m.pendingBytes += 100;
  m.noteTsLocked(mod.loadTs);
  m.noteTsLocked(mod.unloadTs);
  m.maybeFlushLocked();
}

void LiveEncoder::onSample(uint64_t ts, uint32_t pid, uint32_t tid, uint32_t cpu, uint16_t flags,
                           std::span<const uint64_t> ips) {
  if (tid == 0) return;  // the idle task
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  constexpr size_t kMaxFrames = 512;
  if (ips.size() > kMaxFrames) ips = ips.first(kMaxFrames);
  const uint32_t thread = m.internThreadLocked(pid, tid);
  const uint32_t node = m.stackLocked(pid, ips);
  m.sTs.push_back(ts);
  m.sThread.push_back(thread);
  m.sNode.push_back(node);
  m.sCpu.push_back(static_cast<uint16_t>(cpu));
  m.sFlags.push_back(flags);
  ++m.counters.samples;
  m.pendingBytes += 20;
  m.noteTsLocked(ts);
  m.maybeFlushLocked();
}

void LiveEncoder::onContextSwitch(const capture::LiveContextSwitch& c) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  if (c.oldTid != 0 && c.oldPid != 0) (void)m.internThreadLocked(c.oldPid, c.oldTid);
  if (c.newTid != 0 && c.newPid != 0) (void)m.internThreadLocked(c.newPid, c.newTid);
  trace::ContextSwitchRec r{};
  r.ts = c.ts;
  r.cpu = c.cpu;
  r.old_tid = c.oldTid;
  r.new_tid = c.newTid;
  r.old_state = c.oldState;
  r.old_wait_reason = c.oldWaitReason;
  r.old_wait_mode = c.oldWaitMode;
  r.new_priority = c.newPriority;
  r.old_thread_stack = CallstackIdx(c.oldStack.empty() ? kInvalidIdx : m.stackLocked(c.oldPid, c.oldStack));
  r.reserved = c.flags;
  m.switches.push_back(r);
  m.pendingBytes += 32;
  m.noteTsLocked(c.ts);
  m.maybeFlushLocked();
}

void LiveEncoder::onReady(const capture::LiveReady& rd) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  if (rd.readiedTid != 0 && rd.readiedPid != 0) (void)m.internThreadLocked(rd.readiedPid, rd.readiedTid);
  if (rd.readyingTid != 0 && rd.readyingPid != 0) (void)m.internThreadLocked(rd.readyingPid, rd.readyingTid);
  trace::ReadyRec r{};
  r.ts = rd.ts;
  r.cpu = rd.cpu;
  r.readied_tid = rd.readiedTid;
  r.readying_tid = rd.readyingTid;
  r.readying_stack =
      CallstackIdx(rd.readyingStack.empty() ? kInvalidIdx : m.stackLocked(rd.readyingPid, rd.readyingStack));
  r.adjust_reason = rd.adjustReason;
  r.flags = rd.flags;
  m.readies.push_back(r);
  m.pendingBytes += 28;
  m.noteTsLocked(rd.ts);
  m.maybeFlushLocked();
}

void LiveEncoder::onScopeEvent(const capture::LiveScopeEvent& e) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  if (e.tid != 0 && e.pid != 0) (void)m.internThreadLocked(e.pid, e.tid);
  trace::ScopeEventRec r{};
  r.ts = e.ts;
  r.tid = e.tid;
  r.kind = e.kind;
  r.id = StringIdx(m.internStringLocked(e.id));
  r.data = StringIdx(m.internStringLocked(e.data));
  r.color_rgba = e.colorRgba;
  r.caller_addr = e.callerAddr;
  m.scopes.push_back(r);
  m.pendingBytes += 36;
  m.noteTsLocked(e.ts);
  m.maybeFlushLocked();
}

void LiveEncoder::onFrameMark(const capture::LiveFrameMark& f) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  if (f.tid != 0 && f.pid != 0) (void)m.internThreadLocked(f.pid, f.tid);
  trace::FrameMarkRec r{};
  r.ts = f.ts;
  r.tid = f.tid;
  r.name = StringIdx(m.internStringLocked(f.name));
  r.frame_number = f.frameNumber;
  m.frameMarks.push_back(r);
  m.pendingBytes += 24;
  m.noteTsLocked(f.ts);
  m.maybeFlushLocked();
}

void LiveEncoder::onMarker(const capture::LiveMarker& mk) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  if (mk.tid != 0 && mk.pid != 0) (void)m.internThreadLocked(mk.pid, mk.tid);
  trace::MarkerRec r{};
  r.ts = mk.ts;
  r.tid = mk.tid;
  r.name = StringIdx(m.internStringLocked(mk.name));
  r.data = StringIdx(m.internStringLocked(mk.data));
  r.color_rgba = mk.colorRgba;
  m.markers.push_back(r);
  m.pendingBytes += 24;
  m.noteTsLocked(mk.ts);
  m.maybeFlushLocked();
}

void LiveEncoder::onPlot(const capture::LivePlot& p) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  trace::PlotSampleRec r{};
  r.ts = p.ts;
  r.name = StringIdx(m.internStringLocked(p.name));
  r.value = p.value;
  m.plots.push_back(r);
  m.pendingBytes += 20;
  m.noteTsLocked(p.ts);
  m.maybeFlushLocked();
}

void LiveEncoder::onTimeAdvance(uint64_t ts) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended) return;
  m.noteTsLocked(ts);
  m.timeDirty = true;
}

void LiveEncoder::onDropped(uint64_t events) {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  if (m.ended || events == 0) return;
  m.pendingDropped += events;
  m.counters.droppedLive += events;
}

uint64_t LiveEncoder::flush() {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->flushLocked();
}

void LiveEncoder::finish() {
  {
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->ended) return;
    m_impl->flushLocked();
    m_impl->ended = true;
  }
  m_impl->batchCv.notify_all();
}

bool LiveEncoder::ended() const {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->ended;
}

uint64_t LiveEncoder::lastSeq() const {
  std::lock_guard lock(m_impl->mutex);
  return m_impl->seq;
}

bool LiveEncoder::retains(uint64_t seq) const {
  std::lock_guard lock(m_impl->mutex);
  const Impl& m = *m_impl;
  if (seq == 0 || seq > m.seq + 1) return false;
  if (seq == m.seq + 1) return true;
  return !m.batches.empty() && seq >= m.batches.front().seq;
}

LiveWait LiveEncoder::waitBatch(uint64_t seq, LiveBatch& out, std::chrono::milliseconds timeout) {
  std::unique_lock lock(m_impl->mutex);
  Impl& m = *m_impl;
  m.batchCv.wait_for(lock, timeout, [&] { return seq <= m.seq || m.ended; });
  if (seq <= m.seq) {
    if (m.batches.empty() || seq < m.batches.front().seq) return LiveWait::Gap;
    out = m.batches[static_cast<size_t>(seq - m.batches.front().seq)];
    return LiveWait::Ready;
  }
  return m.ended ? LiveWait::Ended : LiveWait::Timeout;
}

LiveKeyframe LiveEncoder::beginKeyframe() {
  std::lock_guard lock(m_impl->mutex);
  Impl& m = *m_impl;
  using namespace model::live_wire;
  LiveKeyframe kf;
  kf.m_seq = m.seq;
  std::string defs;
  auto cut = [&](bool force) {
    if (!defs.empty() && (force || defs.size() >= kMaxEventBytes - 4096)) {
      kf.m_defs.push_back(std::move(defs));
      defs = std::string();
    }
  };
  if (m.haveInfo) {
    InfoPayload p{m.info.clockFrequencyHz, m.info.platform, m.info.samplingHz, m.info.startTs};
    putRecord(defs, RecordType::Info, &p, sizeof(p));
  }
  for (size_t i = 0; i < m.strings.size(); ++i) {
    Impl::putStringDef(defs, static_cast<uint32_t>(i), m.strings[i]);
    cut(false);
  }
  for (size_t i = 0; i < m.procs.size(); ++i) {
    putIndexedRecord(defs, RecordType::ProcessDef, static_cast<uint32_t>(i), &m.procs[i].rec,
                     sizeof(trace::ProcessRec));
    cut(false);
  }
  for (size_t i = 0; i < m.threads.size(); ++i) {
    putIndexedRecord(defs, RecordType::ThreadDef, static_cast<uint32_t>(i), &m.threads[i].rec,
                     sizeof(trace::ThreadRec));
    cut(false);
  }
  for (size_t i = 0; i < m.modules.size(); ++i) {
    putIndexedRecord(defs, RecordType::ModuleDef, static_cast<uint32_t>(i), &m.modules[i].rec,
                     sizeof(trace::ModuleRec));
    cut(false);
  }
  for (size_t i = 0; i < m.frames.size(); ++i) {
    putIndexedRecord(defs, RecordType::FrameDef, static_cast<uint32_t>(i), &m.frames[i], sizeof(trace::FrameRec));
    cut(false);
  }
  for (size_t i = 1; i < m.nodes.size(); ++i) {
    putIndexedRecord(defs, RecordType::NodeDef, static_cast<uint32_t>(i), &m.nodes[i],
                     sizeof(trace::CallstackNodeRec));
    cut(false);
  }
  cut(true);
  kf.m_batches.assign(m.batches.begin(), m.batches.end());
  return kf;
}

LiveEncoderStats LiveEncoder::stats() const {
  std::lock_guard lock(m_impl->mutex);
  const Impl& m = *m_impl;
  LiveEncoderStats s = m.counters;
  s.lastSeq = m.seq;
  s.retainedBatches = m.batches.size();
  s.retainedBytes = m.retainedBytes;
  s.strings = m.strings.size();
  s.processes = m.procs.size();
  s.threads = m.threads.size();
  s.modules = m.modules.size();
  s.frames = m.frames.size();
  s.nodes = m.nodes.size();
  s.nowTs = m.now;
  return s;
}

}  // namespace tachyon::control
