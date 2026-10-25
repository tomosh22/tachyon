// TraceModel::load: parallel chunk decoding and a two-pass counting-sort split of samples per thread.
//
// Phases (each a Runner batch over independent work items):
//   A  decode every non-sample chunk (decompress + typed records), merged afterwards in chunk order;
//   B  decompress every Samples chunk and count samples per tid (payload is discarded);
//   -  sequential: build threads / tidToThread, prefix-sum per (chunk, thread) destination offsets;
//   C0 size the per-thread sample columns;
//   C  decompress every Samples chunk again and scatter its samples straight to their final slots;
//   D  verify per-thread ts order (stable sort only if needed).
// Every sample has exactly one destination slot determined by the file order, so the result does not depend
// on the worker count or scheduling.
#include "core/log.h"
#include "model/model.h"
#include "trace/trace_reader.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <thread>
#include <unordered_set>

namespace tachyon::model {

namespace {

using trace::ChunkType;
using trace::FooterRec;
using trace::TraceReader;

constexpr size_t kMedianDeltasPerThread = 2048;

// Aligned local copy of a packed on-disk record (see the ALIGNMENT RULE in docs/specs/trace-format.md): lets
// StrongId members be used without touching misaligned addresses.
template <typename T>
struct Local {
  alignas(8) T v;
  explicit Local(const T& src) {
    std::memcpy(static_cast<void*>(&v), static_cast<const void*>(&src), sizeof(T));
  }
};

// Runs batches of independent work items on a fixed number of workers. The calling thread is worker 0 and the
// only one that reports progress.
class Runner {
 public:
  using Fn = std::function<Result<void>(size_t item, unsigned worker)>;

  Runner(unsigned workers, const std::atomic<bool>* cancel, std::function<void(float)> progress)
      : m_workers(std::max(1u, workers)), m_cancel(cancel), m_progress(std::move(progress)) {}

  unsigned workers() const { return m_workers; }

  // Runs fn(0..count-1); `share` is the fraction of the overall progress this batch accounts for.
  Result<void> run(size_t count, float share, const Fn& fn) {
    if (count > 0) {
      std::atomic<size_t> next{0};
      std::atomic<size_t> done{0};
      std::atomic<bool> failed{false};
      std::mutex errMutex;
      Result<void> firstError;

      auto fail = [&](Error e) {
        std::lock_guard<std::mutex> lock(errMutex);
        if (!failed.exchange(true)) firstError = std::move(e);
      };
      auto work = [&](unsigned worker) {
        for (;;) {
          if (failed.load(std::memory_order_relaxed)) return;
          if (m_cancel && m_cancel->load(std::memory_order_relaxed)) {
            fail(makeError(ErrorCode::Cancelled, "load cancelled"));
            return;
          }
          const size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= count) return;
          Result<void> r = ok();
          try {
            r = fn(i, worker);
          } catch (const std::bad_alloc&) {
            r = makeError(ErrorCode::OutOfMemory, "out of memory while loading trace");
          } catch (const std::exception& e) {
            r = makeError(ErrorCode::Internal, e.what());
          }
          if (!r.ok()) {
            fail(std::move(r).error());
            return;
          }
          const size_t d = done.fetch_add(1, std::memory_order_relaxed) + 1;
          if (worker == 0) report(m_base + share * static_cast<float>(d) / static_cast<float>(count));
        }
      };

      const unsigned n = static_cast<unsigned>(std::min<size_t>(m_workers, count));
      std::vector<std::thread> threads;
      threads.reserve(n);
      for (unsigned w = 1; w < n; ++w) threads.emplace_back(work, w);
      work(0);
      for (std::thread& t : threads) t.join();
      if (failed.load()) return firstError;
    }
    m_base += share;
    report(m_base);
    return ok();
  }

  void finish() { report(1.0f); }

 private:
  void report(float p) {
    p = std::min(1.0f, std::max(p, m_last));
    m_last = p;
    if (m_progress) m_progress(p);
  }

  unsigned m_workers;
  const std::atomic<bool>* m_cancel;
  std::function<void(float)> m_progress;
  float m_base = 0.0f;
  float m_last = 0.0f;
};

// Open-addressing map uint32 -> uint32 (power-of-two capacity, load <= 0.5).
class U32Map {
 public:
  explicit U32Map(size_t expected = 32) { reset(expected); }

  // Returns the stored value or kInvalidIdx.
  uint32_t get(uint32_t key) const {
    uint32_t h = hash(key);
    for (;; h = (h + 1) & m_mask) {
      const Slot& s = m_slots[h];
      if (s.val == kInvalidIdx) return kInvalidIdx;
      if (s.key == key) return s.val;
    }
  }
  // Inserts key -> val if absent; returns the stored value (existing or new). val must not be kInvalidIdx.
  uint32_t insert(uint32_t key, uint32_t val) {
    if ((m_size + 1) * 2 > m_slots.size()) grow();
    uint32_t h = hash(key);
    for (;; h = (h + 1) & m_mask) {
      Slot& s = m_slots[h];
      if (s.val == kInvalidIdx) {
        s = Slot{key, val};
        ++m_size;
        return val;
      }
      if (s.key == key) return s.val;
    }
  }

 private:
  struct Slot {
    uint32_t key = 0;
    uint32_t val = kInvalidIdx;
  };
  uint32_t hash(uint32_t key) const { return (key * 0x9E3779B1u) >> m_shift; }
  void reset(size_t expected) {
    size_t cap = 16;
    uint32_t shift = 28;
    while (cap < expected * 2) {
      cap <<= 1;
      --shift;
    }
    m_slots.assign(cap, Slot{});
    m_mask = static_cast<uint32_t>(cap - 1);
    m_shift = shift;
    m_size = 0;
  }
  void grow() {
    std::vector<Slot> old = std::move(m_slots);
    reset(old.size());
    for (const Slot& s : old)
      if (s.val != kInvalidIdx) insert(s.key, s.val);
  }

  std::vector<Slot> m_slots;
  uint32_t m_mask = 0, m_shift = 0;
  size_t m_size = 0;
};

// Decoded content of one non-sample chunk.
struct Decoded {
  std::vector<std::byte> bytes;  // Strings and CaptureInfo payloads
  std::vector<trace::ProcessRec> processes;
  std::vector<trace::ThreadRec> threads;
  std::vector<trace::ModuleInfo> modules;
  std::vector<trace::FrameRec> frames;
  std::vector<trace::CallstackNodeRec> nodes;
  std::vector<trace::ContextSwitchRec> contextSwitches;
  std::vector<trace::ReadyRec> readyEvents;
  std::vector<trace::ScopeEventRec> scopeEvents;
  std::vector<trace::FiberEventRec> fiberEvents;
  std::vector<trace::FrameMarkRec> frameMarks;
  std::vector<trace::MarkerRec> markers;
  std::vector<trace::PlotSampleRec> plotSamples;
  std::vector<trace::ThreadNameRec> threadNames;
  std::vector<trace::ClockCalibrationRec> clockCalibrations;
};

struct TidCount {
  uint32_t tid;
  uint32_t count;
};

// Where the samples of one thread in one Samples chunk go.
struct Placement {
  uint32_t thread;
  uint32_t count;
  uint64_t offset;
};

template <typename T>
void appendAll(std::vector<T>& dst, std::vector<T>& src) {
  dst.insert(dst.end(), std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()));
}

template <typename T>
bool sortedByTs(const std::vector<T>& v) {
  return std::is_sorted(v.begin(), v.end(), [](const T& a, const T& b) { return a.ts < b.ts; });
}
template <typename T>
void ensureSortedByTs(std::vector<T>& v) {
  if (!sortedByTs(v))
    std::stable_sort(v.begin(), v.end(), [](const T& a, const T& b) { return a.ts < b.ts; });
}

Result<void> checkSamplePayload(const FooterRec& c, const std::vector<std::byte>& payload) {
  if (c.record_count > trace::kMaxRecordsPerChunk || payload.size() != c.record_count * trace::kSampleBytes)
    return makeError(ErrorCode::CorruptData, "Samples payload size does not match record count");
  return ok();
}

template <typename T>
T loadAt(const std::byte* base, size_t index) {
  T v;
  std::memcpy(&v, base + index * sizeof(T), sizeof(T));
  return v;
}

CaptureInfo parseCaptureInfo(std::string json) {
  CaptureInfo info;
  info.raw_json = std::move(json);
  nlohmann::json doc = nlohmann::json::parse(info.raw_json, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    TACHYON_LOG_WARN("model: CaptureInfo is not a valid JSON object");
    return info;
  }
  auto num = [&](const char* key) -> uint64_t {
    auto it = doc.find(key);
    if (it == doc.end() || !it->is_number() || it->get<double>() < 0) return 0;
    return static_cast<uint64_t>(std::llround(it->get<double>()));
  };
  info.sampling_hz = num("sampling_hz");
  info.events_lost = num("events_lost");
  return info;
}

// Median of consecutive sample deltas over a bounded prefix of every thread.
uint64_t medianSampleDelta(const std::vector<SampleColumns>& samples) {
  std::vector<uint64_t> deltas;
  for (const SampleColumns& s : samples) {
    const size_t n = std::min(s.ts.size(), kMedianDeltasPerThread + 1);
    for (size_t i = 1; i < n; ++i)
      if (s.ts[i] > s.ts[i - 1]) deltas.push_back(s.ts[i] - s.ts[i - 1]);
  }
  if (deltas.empty()) return 0;
  auto mid = deltas.begin() + static_cast<std::ptrdiff_t>(deltas.size() / 2);
  std::nth_element(deltas.begin(), mid, deltas.end());
  return *mid;
}

}  // namespace

Result<TraceModel> TraceModel::load(const std::filesystem::path& path, const LoadOptions& options,
                                    std::function<void(float)> progress) {
  TACHYON_TRY_ASSIGN(TraceReader opened, TraceReader::open(path));

  TraceModel m;
  m.header = opened.header();
  m.isPartial = opened.isPartial();
  const std::vector<FooterRec> chunks(opened.chunks().begin(), opened.chunks().end());

  unsigned workers = options.threads ? options.threads : std::thread::hardware_concurrency();
  if (workers == 0) workers = 1;
  Runner runner(workers, options.cancel, std::move(progress));

  // One reader (own file handle) per worker; worker 0 reuses the already opened one.
  std::vector<std::unique_ptr<TraceReader>> readers(runner.workers());
  readers[0] = std::make_unique<TraceReader>(std::move(opened));
  auto readerFor = [&](unsigned worker) -> Result<TraceReader*> {
    if (!readers[worker]) {
      TACHYON_TRY_ASSIGN(TraceReader r, TraceReader::open(path));
      readers[worker] = std::make_unique<TraceReader>(std::move(r));
    }
    return readers[worker].get();
  };

  std::vector<size_t> otherIdx, sampleIdx;
  for (size_t i = 0; i < chunks.size(); ++i) {
    if (chunks[i].type == static_cast<uint32_t>(ChunkType::Samples))
      sampleIdx.push_back(i);
    else if (chunks[i].type != static_cast<uint32_t>(ChunkType::Footer))
      otherIdx.push_back(i);
  }

  // ---- Phase A: non-sample chunks ----
  std::vector<Decoded> decoded(otherIdx.size());
  TACHYON_TRY(runner.run(otherIdx.size(), 0.15f, [&](size_t item, unsigned worker) -> Result<void> {
    const FooterRec& c = chunks[otherIdx[item]];
    Decoded& d = decoded[item];
    TACHYON_TRY_ASSIGN(TraceReader * reader, readerFor(worker));
    std::vector<std::byte> payload;
    TACHYON_TRY(reader->readChunk(c, payload));
    switch (static_cast<ChunkType>(c.type)) {
      case ChunkType::Strings:
      case ChunkType::CaptureInfo: d.bytes = std::move(payload); return ok();
      case ChunkType::Processes: return TraceReader::decodeRecords(c, payload, d.processes);
      case ChunkType::Threads: return TraceReader::decodeRecords(c, payload, d.threads);
      case ChunkType::Modules: {
        std::vector<trace::ModuleRec> recs;
        TACHYON_TRY(TraceReader::decodeRecords(c, payload, recs));
        d.modules.reserve(recs.size());
        for (const trace::ModuleRec& r : recs) d.modules.push_back(trace::toModuleInfo(r));
        return ok();
      }
      case ChunkType::Frames: return TraceReader::decodeRecords(c, payload, d.frames);
      case ChunkType::CallstackNodes: return TraceReader::decodeRecords(c, payload, d.nodes);
      case ChunkType::ContextSwitches: return TraceReader::decodeRecords(c, payload, d.contextSwitches);
      case ChunkType::ReadyEvents: return TraceReader::decodeRecords(c, payload, d.readyEvents);
      case ChunkType::ScopeEvents: return TraceReader::decodeRecords(c, payload, d.scopeEvents);
      case ChunkType::FiberEvents: return TraceReader::decodeRecords(c, payload, d.fiberEvents);
      case ChunkType::FrameMarks: return TraceReader::decodeRecords(c, payload, d.frameMarks);
      case ChunkType::Markers: return TraceReader::decodeRecords(c, payload, d.markers);
      case ChunkType::PlotSamples: return TraceReader::decodeRecords(c, payload, d.plotSamples);
      case ChunkType::ThreadNames: return TraceReader::decodeRecords(c, payload, d.threadNames);
      case ChunkType::ClockCalibration: return TraceReader::decodeRecords(c, payload, d.clockCalibrations);
      default: return ok();
    }
  }));

  // ---- Merge phase A results in chunk order ----
  bool firstStrings = true;
  bool haveCaptureInfo = false;
  for (size_t k = 0; k < otherIdx.size(); ++k) {
    const FooterRec& c = chunks[otherIdx[k]];
    Decoded& d = decoded[k];
    if (c.type == static_cast<uint32_t>(ChunkType::Strings)) {
      if (c.record_count == 0) return makeError(ErrorCode::CorruptData, "empty Strings chunk");
      std::span<const std::byte> bytes(d.bytes);
      uint64_t count = c.record_count;
      if (firstStrings) {
        // String 0 ("") is pre-interned by StringTable; the first chunk's record 0 must be that empty string.
        uint32_t len = 0;
        if (bytes.size() < 4) return makeError(ErrorCode::CorruptData, "string 0 is missing");
        std::memcpy(&len, bytes.data(), 4);
        if (len != 0) return makeError(ErrorCode::CorruptData, "string 0 is not empty");
        bytes = bytes.subspan(4);
        --count;
        firstStrings = false;
      }
      TACHYON_TRY(m.strings.deserializeAppend(bytes, count));
      std::vector<std::byte>().swap(d.bytes);
    } else if (c.type == static_cast<uint32_t>(ChunkType::CaptureInfo) && !haveCaptureInfo) {
      m.captureInfo =
          parseCaptureInfo(std::string(reinterpret_cast<const char*>(d.bytes.data()), d.bytes.size()));
      haveCaptureInfo = true;
    }
  }

  auto totalOf = [&](auto member) {
    size_t n = 0;
    for (const Decoded& d : decoded) n += (d.*member).size();
    return n;
  };
  const size_t nodeTotal = totalOf(&Decoded::nodes);
  const size_t frameTotal = totalOf(&Decoded::frames);
  m.nodes.reserve(std::max<size_t>(nodeTotal, 1));
  m.frames.reserve(frameTotal);
  m.contextSwitches.reserve(totalOf(&Decoded::contextSwitches));
  m.readyEvents.reserve(totalOf(&Decoded::readyEvents));
  m.scopeEvents.reserve(totalOf(&Decoded::scopeEvents));
  m.clockCalibrations.reserve(totalOf(&Decoded::clockCalibrations));

  std::vector<trace::ThreadRec> threadRecs;
  std::vector<trace::FiberEventRec> fiberRecs;
  std::vector<trace::FrameMarkRec> frameMarkRecs;
  std::vector<trace::MarkerRec> markerRecs;
  std::vector<trace::PlotSampleRec> plotRecs;
  std::vector<trace::ThreadNameRec> nameRecs;
  std::vector<trace::ProcessRec> processRecs;
  std::vector<trace::ModuleInfo> moduleInfos;
  for (Decoded& d : decoded) {
    appendAll(processRecs, d.processes);
    appendAll(threadRecs, d.threads);
    appendAll(moduleInfos, d.modules);
    for (const trace::FrameRec& src : d.frames) {
      Local<trace::FrameRec> r(src);
      m.frames.push_back(Frame{r.v.module, r.v.addr_or_rva});
    }
    for (const trace::CallstackNodeRec& src : d.nodes) {
      Local<trace::CallstackNodeRec> r(src);
      m.nodes.push_back(CallstackNode{r.v.parent, r.v.frame});
    }
    appendAll(m.contextSwitches, d.contextSwitches);
    appendAll(m.readyEvents, d.readyEvents);
    appendAll(m.scopeEvents, d.scopeEvents);
    appendAll(fiberRecs, d.fiberEvents);
    appendAll(frameMarkRecs, d.frameMarks);
    appendAll(markerRecs, d.markers);
    appendAll(plotRecs, d.plotSamples);
    appendAll(nameRecs, d.threadNames);
    appendAll(m.clockCalibrations, d.clockCalibrations);
    d = Decoded{};
  }
  decoded.clear();
  decoded.shrink_to_fit();

  if (m.nodes.empty()) m.nodes.push_back(CallstackNode{});  // root: parent and frame invalid

  // ---- Validate the interned tables so later phases can index them blindly ----
  for (const Frame& f : m.frames)
    if (f.module.valid() && f.module.get() >= moduleInfos.size())
      return makeError(ErrorCode::CorruptData, "frame refers to a module that does not exist");
  for (size_t i = 0; i < m.nodes.size(); ++i) {
    const CallstackNode& n = m.nodes[i];
    if (i == 0) continue;
    if (!n.parent.valid() || n.parent.get() >= i)
      return makeError(ErrorCode::CorruptData, "callstack node parent is not an earlier node");
    if (!n.frame.valid() || n.frame.get() >= m.frames.size())
      return makeError(ErrorCode::CorruptData, "callstack node refers to a frame that does not exist");
  }

  // ---- Processes, modules ----
  m.processes.reserve(processRecs.size());
  for (const trace::ProcessRec& src : processRecs) {
    Local<trace::ProcessRec> r(src);
    Process p;
    p.pid = r.v.pid;
    p.parent_pid = r.v.parent_pid;
    p.start_ts = r.v.start_ts;
    p.end_ts = r.v.end_ts;
    p.name = std::string(m.strings.get(r.v.name));
    p.command_line = std::string(m.strings.get(r.v.command_line));
    m.processes.push_back(std::move(p));
  }
  m.modules.reserve(moduleInfos.size());
  for (const trace::ModuleInfo& mi : moduleInfos) {
    Module mod;
    mod.pid = mi.pid;
    mod.base = mi.base;
    mod.size = mi.size;
    mod.path = std::string(m.strings.get(mi.path));
    mod.id.kind = mi.id_kind;
    mod.id.bytes = mi.id;
    mod.id.len = std::min<uint8_t>(mi.id_len, 32);
    mod.id.age = mi.age;
    mod.load_ts = mi.load_ts;
    mod.unload_ts = mi.unload_ts;
    m.modules.push_back(std::move(mod));
  }
  std::vector<trace::ModuleInfo>().swap(moduleInfos);

  // ---- Phase B: count samples per tid in every Samples chunk ----
  std::vector<std::vector<TidCount>> chunkCounts(sampleIdx.size());
  std::vector<std::vector<std::byte>> buffers(runner.workers());
  TACHYON_TRY(runner.run(sampleIdx.size(), 0.25f, [&](size_t item, unsigned worker) -> Result<void> {
    const FooterRec& c = chunks[sampleIdx[item]];
    TACHYON_TRY_ASSIGN(TraceReader * reader, readerFor(worker));
    std::vector<std::byte>& payload = buffers[worker];
    TACHYON_TRY(reader->readChunk(c, payload));
    TACHYON_TRY(checkSamplePayload(c, payload));
    const size_t n = static_cast<size_t>(c.record_count);
    const std::byte* tids = payload.data() + n * 8;
    U32Map index;
    std::vector<TidCount>& counts = chunkCounts[item];
    uint32_t lastTid = 0, lastIdx = kInvalidIdx;
    for (size_t i = 0; i < n; ++i) {
      const uint32_t tid = loadAt<uint32_t>(tids, i);
      if (lastIdx == kInvalidIdx || tid != lastTid) {
        uint32_t idx = index.get(tid);
        if (idx == kInvalidIdx) {
          idx = index.insert(tid, static_cast<uint32_t>(counts.size()));
          counts.push_back(TidCount{tid, 0});
        }
        lastTid = tid;
        lastIdx = idx;
      }
      ++counts[lastIdx].count;
    }
    std::sort(counts.begin(), counts.end(),
              [](const TidCount& a, const TidCount& b) { return a.tid < b.tid; });
    return ok();
  }));

  // ---- Threads ----
  // ThreadNames: the latest rename (by ts, ties in file order) wins.
  ensureSortedByTs(nameRecs);
  std::unordered_map<uint32_t, StringIdx> renamed;
  for (const trace::ThreadNameRec& src : nameRecs) {
    Local<trace::ThreadNameRec> r(src);
    renamed[r.v.tid] = r.v.name;
  }
  m.threads.reserve(threadRecs.size());
  for (const trace::ThreadRec& src : threadRecs) {
    Local<trace::ThreadRec> r(src);
    Thread t;
    t.tid = r.v.tid;
    t.pid = r.v.pid;
    t.first_ts = r.v.start_ts;
    t.last_ts = r.v.end_ts;
    t.is_target = (r.v.flags & trace::kThreadFlagTargetProcess) != 0;
    t.name = std::string(m.strings.get(r.v.name));
    m.tidToThread.emplace(t.tid, ThreadIdx(static_cast<uint32_t>(m.threads.size())));
    m.threads.push_back(std::move(t));
  }
  for (const auto& [tid, name] : renamed) {
    auto it = m.tidToThread.find(tid);
    if (it != m.tidToThread.end()) m.threads[it->second.get()].name = std::string(m.strings.get(name));
  }
  // Threads seen in events but missing from ThreadRecs get a synthesised Thread (ascending tid order).
  {
    std::vector<uint32_t> seen;
    for (const auto& counts : chunkCounts)
      for (const TidCount& tc : counts) seen.push_back(tc.tid);
    // tid 0 on an event means "no thread" (e.g. a global marker) and does not create a Thread.
    auto addTid = [&](uint32_t tid) {
      if (tid != 0) seen.push_back(tid);
    };
    for (const trace::ScopeEventRec& e : m.scopeEvents) addTid(Local<trace::ScopeEventRec>(e).v.tid);
    for (const trace::FiberEventRec& e : fiberRecs) addTid(Local<trace::FiberEventRec>(e).v.tid);
    for (const trace::FrameMarkRec& e : frameMarkRecs) addTid(Local<trace::FrameMarkRec>(e).v.tid);
    for (const trace::MarkerRec& e : markerRecs) addTid(Local<trace::MarkerRec>(e).v.tid);
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    const uint32_t defaultPid = m.processes.empty() ? 0 : m.processes.front().pid;
    size_t synthesised = 0;
    for (uint32_t tid : seen) {
      if (m.tidToThread.count(tid)) continue;
      Thread t;
      t.tid = tid;
      t.pid = defaultPid;
      auto rn = renamed.find(tid);
      if (rn != renamed.end()) t.name = std::string(m.strings.get(rn->second));
      m.tidToThread.emplace(tid, ThreadIdx(static_cast<uint32_t>(m.threads.size())));
      m.threads.push_back(std::move(t));
      ++synthesised;
    }
    if (synthesised) TACHYON_LOG_DEBUG("model: synthesised {} threads missing from ThreadRecs", synthesised);
  }
  for (Thread& t : m.threads) t.role = classifyThreadRole(t.name);
  const size_t threadCount = m.threads.size();
  m.samplesByThread.resize(threadCount);
  m.statesByThread.resize(threadCount);
  m.scopesByThread.resize(threadCount);

  // ---- Per (chunk, thread) destination offsets: prefix sums in chunk order ----
  std::vector<std::vector<Placement>> placements(sampleIdx.size());
  std::vector<uint64_t> totals(threadCount, 0);
  for (size_t k = 0; k < sampleIdx.size(); ++k) {
    placements[k].reserve(chunkCounts[k].size());
    for (const TidCount& tc : chunkCounts[k]) {
      const uint32_t t = m.tidToThread.at(tc.tid).get();
      placements[k].push_back(Placement{t, tc.count, totals[t]});
      totals[t] += tc.count;
    }
  }
  std::vector<std::vector<TidCount>>().swap(chunkCounts);

  // ---- Phase C0: size the columns ----
  TACHYON_TRY(runner.run(threadCount, 0.03f, [&](size_t t, unsigned) -> Result<void> {
    SampleColumns& s = m.samplesByThread[t];
    const size_t n = static_cast<size_t>(totals[t]);
    s.ts.resize(n);
    s.callstack.resize(n);
    s.cpu.resize(n);
    s.flags.resize(n);
    return ok();
  }));

  // ---- Phase C: scatter ----
  std::vector<uint32_t> chunkMaxStack(sampleIdx.size(), 0);
  TACHYON_TRY(runner.run(sampleIdx.size(), 0.40f, [&](size_t item, unsigned worker) -> Result<void> {
    const FooterRec& c = chunks[sampleIdx[item]];
    TACHYON_TRY_ASSIGN(TraceReader * reader, readerFor(worker));
    std::vector<std::byte>& payload = buffers[worker];
    TACHYON_TRY(reader->readChunk(c, payload));
    TACHYON_TRY(checkSamplePayload(c, payload));
    const size_t n = static_cast<size_t>(c.record_count);
    const std::byte* tsCol = payload.data();
    const std::byte* tidCol = tsCol + n * 8;
    const std::byte* stackCol = tidCol + n * 4;
    const std::byte* cpuCol = stackCol + n * 4;
    const std::byte* flagCol = cpuCol + n * 2;

    struct Cursor {
      uint64_t* ts;
      CallstackIdx* stack;
      uint16_t* cpu;
      uint16_t* flags;
      uint32_t pos, end;
    };
    const std::vector<Placement>& place = placements[item];
    std::vector<Cursor> cursors(place.size());
    U32Map index(place.size());
    for (size_t j = 0; j < place.size(); ++j) {
      SampleColumns& s = m.samplesByThread[place[j].thread];
      const size_t off = static_cast<size_t>(place[j].offset);
      cursors[j] =
          Cursor{s.ts.data() + off, s.callstack.data() + off, s.cpu.data() + off, s.flags.data() + off, 0,
                 place[j].count};
      index.insert(m.threads[place[j].thread].tid, static_cast<uint32_t>(j));
    }
    uint32_t maxStack = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint32_t j = index.get(loadAt<uint32_t>(tidCol, i));
      if (j == kInvalidIdx) return makeError(ErrorCode::Internal, "sample thread changed between passes");
      Cursor& cur = cursors[j];
      if (cur.pos >= cur.end) return makeError(ErrorCode::Internal, "sample count changed between passes");
      const uint32_t stack = loadAt<uint32_t>(stackCol, i);
      if (stack != kInvalidIdx && stack > maxStack) maxStack = stack;
      cur.ts[cur.pos] = loadAt<uint64_t>(tsCol, i);
      cur.stack[cur.pos] = CallstackIdx(stack);
      cur.cpu[cur.pos] = loadAt<uint16_t>(cpuCol, i);
      cur.flags[cur.pos] = loadAt<uint16_t>(flagCol, i);
      ++cur.pos;
    }
    chunkMaxStack[item] = maxStack;
    return ok();
  }));
  std::vector<std::vector<std::byte>>().swap(buffers);
  readers.clear();

  // Samples whose callstack lies beyond the recovered node table: error, or (partial file) drop the stack.
  const uint32_t maxStack =
      chunkMaxStack.empty() ? 0 : *std::max_element(chunkMaxStack.begin(), chunkMaxStack.end());
  const bool stacksOutOfRange = maxStack >= m.nodes.size();
  if (stacksOutOfRange && !m.isPartial)
    return makeError(ErrorCode::CorruptData, "sample refers to a callstack node that does not exist");

  // ---- Phase D: per-thread order check / stable sort, stack sanitising ----
  std::atomic<size_t> resorted{0};
  TACHYON_TRY(runner.run(threadCount, 0.15f, [&](size_t t, unsigned) -> Result<void> {
    SampleColumns& s = m.samplesByThread[t];
    if (stacksOutOfRange) {
      TACHYON_LOG_WARN("model: partial trace has samples with unrecovered callstacks; stacks dropped");
      for (CallstackIdx& cs : s.callstack)
        if (cs.valid() && cs.get() >= m.nodes.size()) cs = CallstackIdx::invalid();
    }
    if (std::is_sorted(s.ts.begin(), s.ts.end())) return ok();
    resorted.fetch_add(1, std::memory_order_relaxed);
    std::vector<uint32_t> perm(s.ts.size());
    std::iota(perm.begin(), perm.end(), 0u);
    std::stable_sort(perm.begin(), perm.end(), [&](uint32_t a, uint32_t b) { return s.ts[a] < s.ts[b]; });
    auto apply = [&](auto& col) {
      auto copy = col;
      for (size_t i = 0; i < perm.size(); ++i) col[i] = copy[perm[i]];
    };
    apply(s.ts);
    apply(s.callstack);
    apply(s.cpu);
    apply(s.flags);
    return ok();
  }));
  if (resorted.load())
    TACHYON_LOG_WARN("model: {} threads had unsorted samples and were re-sorted", resorted.load());

  // ---- Event streams ----
  ensureSortedByTs(m.contextSwitches);
  ensureSortedByTs(m.readyEvents);
  ensureSortedByTs(m.scopeEvents);
  ensureSortedByTs(fiberRecs);
  ensureSortedByTs(frameMarkRecs);
  ensureSortedByTs(markerRecs);
  ensureSortedByTs(plotRecs);

  auto threadOf = [&](uint32_t tid) {
    auto it = m.tidToThread.find(tid);
    return it == m.tidToThread.end() ? ThreadIdx::invalid() : it->second;
  };
  m.fiberSwitches.reserve(fiberRecs.size());
  for (const trace::FiberEventRec& src : fiberRecs) {
    Local<trace::FiberEventRec> r(src);
    m.fiberSwitches.push_back(
        FiberSwitch{r.v.ts, threadOf(r.v.tid), r.v.kind, r.v.fiber_id, r.v.other_fiber_id});
  }
  m.frameMarks.reserve(frameMarkRecs.size());
  for (const trace::FrameMarkRec& src : frameMarkRecs) {
    Local<trace::FrameMarkRec> r(src);
    m.frameMarks.push_back(FrameMark{r.v.ts, threadOf(r.v.tid), r.v.name, r.v.frame_number});
  }
  m.markers.reserve(markerRecs.size());
  for (const trace::MarkerRec& src : markerRecs) {
    Local<trace::MarkerRec> r(src);
    m.markers.push_back(Marker{r.v.ts, threadOf(r.v.tid), r.v.name, r.v.data, r.v.color_rgba});
  }
  {
    std::unordered_map<uint32_t, size_t> seriesOf;
    for (const trace::PlotSampleRec& src : plotRecs) {
      Local<trace::PlotSampleRec> r(src);
      auto [it, inserted] = seriesOf.emplace(r.v.name.get(), m.plots.size());
      if (inserted) {
        m.plots.emplace_back();
        m.plots.back().name = r.v.name;
      }
      PlotSeries& p = m.plots[it->second];
      const uint64_t ts = r.v.ts;
      const double value = r.v.value;
      p.ts.push_back(ts);
      p.value.push_back(value);
    }
  }

  // ---- Thread first/last timestamps: ThreadRec values, else first/last event ----
  {
    std::vector<uint64_t> evFirst(threadCount, UINT64_MAX), evLast(threadCount, 0);
    auto touch = [&](uint32_t tid, uint64_t ts) {
      auto it = m.tidToThread.find(tid);
      if (it == m.tidToThread.end()) return;
      const uint32_t t = it->second.get();
      evFirst[t] = std::min(evFirst[t], ts);
      evLast[t] = std::max(evLast[t], ts);
    };
    for (size_t t = 0; t < threadCount; ++t) {
      const SampleColumns& s = m.samplesByThread[t];
      if (!s.ts.empty()) {
        evFirst[t] = s.ts.front();
        evLast[t] = s.ts.back();
      }
    }
    for (const trace::ContextSwitchRec& e : m.contextSwitches) {
      Local<trace::ContextSwitchRec> r(e);
      touch(r.v.old_tid, r.v.ts);
      touch(r.v.new_tid, r.v.ts);
    }
    for (const trace::ReadyRec& e : m.readyEvents) {
      Local<trace::ReadyRec> r(e);
      touch(r.v.readied_tid, r.v.ts);
    }
    for (const trace::ScopeEventRec& e : m.scopeEvents) {
      Local<trace::ScopeEventRec> r(e);
      touch(r.v.tid, r.v.ts);
    }
    auto touchIdx = [&](ThreadIdx t, uint64_t ts) {
      if (t.valid()) touch(m.threads[t.get()].tid, ts);
    };
    for (const FiberSwitch& e : m.fiberSwitches) touchIdx(e.thread, e.ts);
    for (const FrameMark& e : m.frameMarks) touchIdx(e.thread, e.ts);
    for (const Marker& e : m.markers) touchIdx(e.thread, e.ts);
    for (size_t t = 0; t < threadCount; ++t) {
      if (m.threads[t].first_ts == 0 && evFirst[t] != UINT64_MAX) m.threads[t].first_ts = evFirst[t];
      if (m.threads[t].last_ts == 0) m.threads[t].last_ts = evLast[t];
    }
  }

  // ---- Sample interval ----
  if (m.header.clock_frequency_hz != 0 && m.captureInfo.sampling_hz != 0) {
    const double ticks =
        static_cast<double>(m.header.clock_frequency_hz) / static_cast<double>(m.captureInfo.sampling_hz);
    m.sampleIntervalTicks = static_cast<uint64_t>(std::llround(ticks));
  } else {
    m.sampleIntervalTicks = medianSampleDelta(m.samplesByThread);
    TACHYON_LOG_WARN("model: no CaptureInfo sampling_hz; sample interval estimated from deltas: {} ticks",
                     m.sampleIntervalTicks);
  }

  // ---- Shrink to fit ----
  m.processes.shrink_to_fit();
  m.threads.shrink_to_fit();
  m.modules.shrink_to_fit();
  m.frames.shrink_to_fit();
  m.nodes.shrink_to_fit();
  m.contextSwitches.shrink_to_fit();
  m.readyEvents.shrink_to_fit();
  m.scopeEvents.shrink_to_fit();
  m.clockCalibrations.shrink_to_fit();
  m.fiberSwitches.shrink_to_fit();
  m.frameMarks.shrink_to_fit();
  m.markers.shrink_to_fit();
  for (PlotSeries& p : m.plots) {
    p.ts.shrink_to_fit();
    p.value.shrink_to_fit();
  }
  for (SampleColumns& s : m.samplesByThread) {
    s.ts.shrink_to_fit();
    s.callstack.shrink_to_fit();
    s.cpu.shrink_to_fit();
    s.flags.shrink_to_fit();
  }

  runner.finish();
  return m;
}

}  // namespace tachyon::model
