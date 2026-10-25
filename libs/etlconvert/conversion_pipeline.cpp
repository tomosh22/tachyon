#include "core/hash.h"
#include "core/time.h"
#include "etlconvert/etl_converter.h"
#include "etlconvert/stack_matcher.h"
#include "etlconvert/trace_metadata.h"
#include "trace/trace_writer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_set>

namespace tachyon::etlconvert {

namespace {

constexpr size_t kChunkRecords = 262'144;  // records buffered per type before a chunk is appended
constexpr uint32_t kInvalidPid = 0xFFFFFFFFu;
constexpr const char* kConverterVersion = "1";

struct StackRef {
  uint32_t begin = 0;
  uint32_t count = 0;
};

// A frame resolved to (module, RVA) or (invalid module, absolute address).
struct ResolvedFrame {
  uint64_t value = 0;
  uint32_t module = kInvalidIdx;
  uint32_t padding = 0;  // explicit so the struct can be hashed byte-wise
};

struct SampleItem {
  uint64_t ts = 0;
  uint32_t tid = 0;
  uint16_t cpu = 0;
  uint16_t flags = 0;
  StackRef stack;
};
struct SwitchItem {
  trace::ContextSwitchRec rec{};
  StackRef stack;
};
struct ReadyItem {
  trace::ReadyRec rec{};
  StackRef stack;
};

// One unit of work handed from the parse stage to the intern stage.
struct Batch {
  std::vector<SampleItem> samples;
  std::vector<SwitchItem> switches;
  std::vector<ReadyItem> readies;
  std::vector<ResolvedFrame> frames;

  size_t events() const { return samples.size() + switches.size() + readies.size(); }
  void clear() {
    samples.clear();
    switches.clear();
    readies.clear();
    frames.clear();
  }
};

// Bounded hand-off queue with a free list so batch storage is recycled.
class BatchQueue {
 public:
  explicit BatchQueue(size_t capacity) : m_capacity(std::max<size_t>(capacity, 1)) {}

  std::unique_ptr<Batch> acquire() {
    std::lock_guard lock(m_mutex);
    if (!m_free.empty()) {
      auto b = std::move(m_free.back());
      m_free.pop_back();
      return b;
    }
    return std::make_unique<Batch>();
  }
  void push(std::unique_ptr<Batch> b) {
    std::unique_lock lock(m_mutex);
    m_notFull.wait(lock, [&] { return m_queue.size() < m_capacity; });
    m_queue.push_back(std::move(b));
    m_notEmpty.notify_one();
  }
  // Returns null once closed and drained.
  std::unique_ptr<Batch> pop() {
    std::unique_lock lock(m_mutex);
    m_notEmpty.wait(lock, [&] { return !m_queue.empty() || m_closed; });
    if (m_queue.empty()) return nullptr;
    auto b = std::move(m_queue.front());
    m_queue.pop_front();
    m_notFull.notify_one();
    return b;
  }
  void recycle(std::unique_ptr<Batch> b) {
    b->clear();
    std::lock_guard lock(m_mutex);
    m_free.push_back(std::move(b));
  }
  void close() {
    std::lock_guard lock(m_mutex);
    m_closed = true;
    m_notEmpty.notify_all();
  }

 private:
  std::mutex m_mutex;
  std::condition_variable m_notFull;
  std::condition_variable m_notEmpty;
  std::deque<std::unique_ptr<Batch>> m_queue;
  std::vector<std::unique_ptr<Batch>> m_free;
  size_t m_capacity;
  bool m_closed = false;
};

// Owns the TraceWriter: interns frames and callstacks, buffers records per type and appends sorted chunks.
class InternStage {
 public:
  explicit InternStage(trace::TraceWriter writer) : m_writer(std::move(writer)) {}

  void process(const Batch& batch) {
    for (const SampleItem& s : batch.samples) {
      const CallstackIdx cs = internStack(batch, s.stack);
      m_samples.push(s.ts, s.tid, cs, s.cpu, s.flags);
      noteTs(s.ts);
    }
    for (const SwitchItem& s : batch.switches) {
      trace::ContextSwitchRec rec = s.rec;
      rec.old_thread_stack = internStack(batch, s.stack);
      m_switches.push_back(rec);
      noteTs(rec.ts);
    }
    for (const ReadyItem& r : batch.readies) {
      trace::ReadyRec rec = r.rec;
      rec.readying_stack = internStack(batch, r.stack);
      m_readies.push_back(rec);
      noteTs(rec.ts);
    }
    if (m_samples.size() >= kChunkRecords) flushSamples();
    if (m_switches.size() >= kChunkRecords) flushSwitches();
    if (m_readies.size() >= kChunkRecords) flushReadies();
  }

  void flush() {
    flushSamples();
    flushSwitches();
    flushReadies();
  }

  trace::TraceWriter& writer() { return m_writer; }
  bool hasTs() const { return m_hasTs; }
  uint64_t minTs() const { return m_minTs; }
  uint64_t maxTs() const { return m_maxTs; }

 private:
  // Real captures repeat a few thousand distinct stacks millions of times, so whole stacks are memoised in a
  // lossy direct-mapped cache keyed by two independent 64-bit hashes of the resolved frames (a false hit
  // needs a 128-bit collision). A hit skips the per-frame and per-node interning; the result is identical.
  CallstackIdx internStack(const Batch& batch, StackRef ref) {
    if (ref.count == 0) return CallstackIdx(0);
    const ResolvedFrame* f = batch.frames.data() + ref.begin;
    const size_t bytes = static_cast<size_t>(ref.count) * sizeof(ResolvedFrame);
    const uint64_t h1 = core::hashBytes(f, bytes, 0x51ED270B);
    const uint64_t h2 = core::hashBytes(f, bytes, 0x9E3779B97F4A7C15ull);
    CacheEntry& slot = m_cache[h1 & (kCacheSlots - 1)];
    if (slot.h1 == h1 && slot.h2 == h2 && slot.count == ref.count) return CallstackIdx(slot.callstack);
    m_scratch.clear();
    for (uint32_t i = 0; i < ref.count; ++i)
      m_scratch.push_back(m_writer.internFrame(ModuleIdx(f[i].module), f[i].value));
    const CallstackIdx cs = m_writer.internCallstack(m_scratch);
    slot = CacheEntry{h1, h2, cs.get(), ref.count};
    return cs;
  }

  void noteTs(uint64_t ts) {
    if (!m_hasTs) {
      m_minTs = m_maxTs = ts;
      m_hasTs = true;
    } else {
      m_minTs = std::min(m_minTs, ts);
      m_maxTs = std::max(m_maxTs, ts);
    }
  }

  void flushSamples() {
    if (m_samples.size() == 0) return;
    if (!std::is_sorted(m_samples.ts.begin(), m_samples.ts.end())) {
      std::vector<uint32_t> order(m_samples.size());
      std::iota(order.begin(), order.end(), 0u);
      std::stable_sort(order.begin(), order.end(),
                       [&](uint32_t a, uint32_t b) { return m_samples.ts[a] < m_samples.ts[b]; });
      trace::SampleBatch sorted;
      for (uint32_t i : order)
        sorted.push(m_samples.ts[i], m_samples.tid[i], m_samples.callstack[i], m_samples.cpu[i],
                    m_samples.flags[i]);
      m_samples = std::move(sorted);
    }
    m_writer.append(m_samples);
    m_samples = trace::SampleBatch{};
  }
  void flushSwitches() {
    if (m_switches.empty()) return;
    std::stable_sort(
        m_switches.begin(), m_switches.end(),
        [](const trace::ContextSwitchRec& a, const trace::ContextSwitchRec& b) { return a.ts < b.ts; });
    m_writer.append(std::span<const trace::ContextSwitchRec>(m_switches));
    m_switches.clear();
  }
  void flushReadies() {
    if (m_readies.empty()) return;
    std::stable_sort(m_readies.begin(), m_readies.end(),
                     [](const trace::ReadyRec& a, const trace::ReadyRec& b) { return a.ts < b.ts; });
    m_writer.append(std::span<const trace::ReadyRec>(m_readies));
    m_readies.clear();
  }

  struct CacheEntry {
    uint64_t h1 = 0;
    uint64_t h2 = 0;
    uint32_t callstack = 0;
    uint32_t count = 0;  // 0 = empty
  };
  static constexpr size_t kCacheSlots = size_t{1} << 19;

  trace::TraceWriter m_writer;
  std::vector<CacheEntry> m_cache = std::vector<CacheEntry>(kCacheSlots);
  trace::SampleBatch m_samples;
  std::vector<trace::ContextSwitchRec> m_switches;
  std::vector<trace::ReadyRec> m_readies;
  std::vector<FrameIdx> m_scratch;
  bool m_hasTs = false;
  uint64_t m_minTs = 0;
  uint64_t m_maxTs = 0;
};

// Set of thread ids that were written; a bitmap for the usual small ids, a hash set beyond.
class TidSet {
 public:
  void insert(uint32_t tid) {
    if (tid < kBitmapBits) {
      uint64_t& word = m_bits[tid >> 6];
      const uint64_t bit = uint64_t{1} << (tid & 63);
      if ((word & bit) == 0) {
        word |= bit;
        m_list.push_back(tid);
      }
    } else if (m_large.insert(tid).second) {
      m_list.push_back(tid);
    }
  }
  // Every id inserted, in first-insertion order.
  const std::vector<uint32_t>& list() const { return m_list; }
  bool contains(uint32_t tid) const {
    if (tid < kBitmapBits) return (m_bits[tid >> 6] >> (tid & 63)) & 1;
    return m_large.count(tid) != 0;
  }

 private:
  static constexpr uint32_t kBitmapBits = 1u << 22;
  std::vector<uint64_t> m_bits = std::vector<uint64_t>(kBitmapBits / 64, 0);
  std::unordered_set<uint32_t> m_large;
  std::vector<uint32_t> m_list;
};

bool isUserFrame(uint64_t addr) {
  return addr < kKernelAddressBase;
}

}  // namespace

struct ConversionPipeline::Impl final : ParentListener {
  ConvertOptions opt;
  EtlFileInfo info;
  ConvertStats stats;
  PipelinePass pass = PipelinePass::Main;
  bool prescanned = false;
  bool finished = false;
  bool keepAll = false;
  std::vector<uint32_t> targets;  // sorted

  TraceMetadata meta;
  std::unique_ptr<ModuleTable> modules;
  std::unique_ptr<StackMatcher> matcher;
  TidSet usedTids;
  uint64_t seenMinTs = ~uint64_t{0};
  uint64_t seenMaxTs = 0;
  uint64_t newestTs = 0;
  uint64_t eventsInPass = 0;
  std::chrono::steady_clock::time_point startedAt;

  // Output stage.
  std::unique_ptr<InternStage> stage;
  std::unique_ptr<BatchQueue> queue;
  std::thread worker;
  std::unique_ptr<Batch> current;
  size_t hintUser = ~size_t{0};
  size_t hintKernel = ~size_t{0};

  // ---- construction ------------------------------------------------------------------------------------
  void startWorker() {
    queue = std::make_unique<BatchQueue>(opt.queueDepth);
    worker = std::thread([this] {
      while (auto b = queue->pop()) {
        stage->process(*b);
        queue->recycle(std::move(b));
      }
    });
  }

  void newBatch() {
    if (queue) {
      current = queue->acquire();
    } else if (!current) {
      current = std::make_unique<Batch>();
    }
  }

  void submit() {
    if (!current || current->events() == 0) return;
    if (opt.threaded) {
      queue->push(std::move(current));
      newBatch();
    } else {
      stage->process(*current);
      current->clear();
    }
  }

  // ---- classification ----------------------------------------------------------------------------------
  bool isTargetPid(uint32_t pid) const { return std::binary_search(targets.begin(), targets.end(), pid); }

  // Thread tid belongs to a target process at time ts. `headerPid` is a fallback for threads whose lifetime
  // was never seen.
  bool isTargetThread(uint32_t tid, uint64_t ts, uint32_t headerPid) const {
    if (const ThreadLife* life = meta.lifeOf(tid, ts)) return isTargetPid(life->pid);
    if (tid != 0 && headerPid != 0 && headerPid != kInvalidPid) return isTargetPid(headerPid);
    return false;
  }

  uint32_t pidOfStack(const PendingParent& p) const {
    if (p.stackProcess != 0 && p.stackProcess != kInvalidPid) return p.stackProcess;
    if (const ThreadLife* life = meta.lifeOf(p.stackThread, p.ts)) return life->pid;
    return p.headerPid;
  }

  // ---- parse stage: event dispatch -----------------------------------------------------------------------
  void handleMetadata(const EtwEvent& e) {
    if (e.provider == guids::kProcess) {
      if (e.opcode >= opcodes::kProcessStart && e.opcode <= opcodes::kProcessDcEnd && !meta.onProcess(e))
        ++stats.malformedEvents;
    } else if (e.provider == guids::kThread) {
      if (e.opcode == opcodes::kThreadSetName) {
        if (!meta.onSetName(e)) ++stats.malformedEvents;
      } else if (e.opcode >= opcodes::kThreadStart && e.opcode <= opcodes::kThreadDcEnd) {
        if (!meta.onThread(e)) ++stats.malformedEvents;
      }
    } else if (e.provider == guids::kImage) {
      handleImage(e);
    }
  }

  void handleImage(const EtwEvent& e) {
    if (e.opcode != opcodes::kImageLoad && e.opcode != opcodes::kImageDcStart &&
        e.opcode != opcodes::kImageDcEnd && e.opcode != opcodes::kImageUnload)
      return;
    auto img = decodeImage(e.payload);
    if (!img) {
      ++stats.malformedEvents;
      return;
    }
    if (e.opcode == opcodes::kImageUnload) {
      modules->unload(img->processId, img->imageBase, e.timestamp);
    } else {
      modules->load(img->processId, img->imageBase, img->imageSize, img->fileName, img->imageChecksum,
                    img->timeDateStamp, e.opcode == opcodes::kImageLoad ? e.timestamp : 0);
    }
  }

  void onEvent(const EtwEvent& e) {
    seenMinTs = std::min(seenMinTs, e.timestamp);
    seenMaxTs = std::max(seenMaxTs, e.timestamp);
    if (pass == PipelinePass::Metadata) {
      handleMetadata(e);
      return;
    }
    ++stats.eventsTotal;
    if (!prescanned) handleMetadata(e);
    if (e.provider == guids::kPerfInfo) {
      if (e.opcode == opcodes::kPerfInfoSampledProfile)
        onSample(e);
      else
        ++stats.otherEvents;
    } else if (e.provider == guids::kStackWalk) {
      if (e.opcode == opcodes::kStackWalkStack)
        onStack(e);
      else if (e.opcode >= opcodes::kStackWalkKeyKernel && e.opcode <= opcodes::kStackWalkKeyDelete)
        ++stats.stackKeyEvents;
      else
        ++stats.otherEvents;
    } else if (e.provider == guids::kThread) {
      if (e.opcode == opcodes::kThreadCSwitch) {
        onCSwitch(e);
      } else if (e.opcode == opcodes::kThreadReadyThread) {
        onReady(e);
      } else if (e.opcode == opcodes::kThreadSetName) {
        ++stats.setNameEvents;
      } else if (e.opcode >= opcodes::kThreadStart && e.opcode <= opcodes::kThreadDcEnd) {
        ++stats.threadEvents;
      } else {
        ++stats.otherEvents;
      }
    } else if (e.provider == guids::kProcess) {
      ++stats.processEvents;
    } else if (e.provider == guids::kImage) {
      ++stats.imageEvents;
    } else {
      ++stats.otherEvents;
    }
    newestTs = std::max(newestTs, e.timestamp);
    matcher->advance(newestTs);
    if ((++eventsInPass & 0xFFFF) == 0 && opt.progress && prescanned) {
      const double frac =
          static_cast<double>(newestTs - pass1Min) / static_cast<double>(pass1Max - pass1Min + 1);
      opt.progress(static_cast<float>(0.1 + 0.85 * std::min(1.0, std::max(0.0, frac))));
    }
  }
  uint64_t pass1Min = 0;
  uint64_t pass1Max = 0;

  void onSample(const EtwEvent& e) {
    ++stats.sampledProfileEvents;
    auto s = decodeSampledProfile(e.payload);
    if (!s) {
      ++stats.malformedEvents;
      return;
    }
    if (!keepAll && (s->threadId == 0 || !isTargetThread(s->threadId, e.timestamp, e.processId))) {
      ++stats.samplesDropped;
      return;
    }
    const PendingParent::Candidate cands[2] = {{s->threadId, MatchRole::PayloadThread},
                                               {e.threadId, MatchRole::HeaderThread}};
    PendingParent& p = matcher->add(ParentKind::Sample, e.timestamp, cands, 2);
    p.cpu = e.cpu;
    p.headerTid = e.threadId;
    p.headerPid = e.processId;
    p.tidA = s->threadId;
    p.ip = s->instructionPointer;
    usedTids.insert(s->threadId);
  }

  void onCSwitch(const EtwEvent& e) {
    ++stats.cswitchEvents;
    auto c = decodeCSwitch(e.payload);
    if (!c) {
      ++stats.malformedEvents;
      return;
    }
    if (!keepAll && !isTargetThread(c->newThreadId, e.timestamp, 0) &&
        !isTargetThread(c->oldThreadId, e.timestamp, 0)) {
      ++stats.cswitchesDropped;
      return;
    }
    const PendingParent::Candidate cands[3] = {{e.threadId, MatchRole::HeaderThread},
                                               {c->newThreadId, MatchRole::PayloadThread},
                                               {c->oldThreadId, MatchRole::PayloadOldThread}};
    PendingParent& p = matcher->add(ParentKind::CSwitch, e.timestamp, cands, 3);
    p.cpu = e.cpu;
    p.headerTid = e.threadId;
    p.headerPid = e.processId;
    p.tidA = c->newThreadId;
    p.tidB = c->oldThreadId;
    p.bytes[0] = static_cast<uint8_t>(c->oldThreadState);
    p.bytes[1] = static_cast<uint8_t>(c->oldThreadWaitReason);
    p.bytes[2] = static_cast<uint8_t>(c->oldThreadWaitMode);
    p.bytes[3] = static_cast<uint8_t>(c->newThreadPriority);
    usedTids.insert(c->newThreadId);
    usedTids.insert(c->oldThreadId);
  }

  void onReady(const EtwEvent& e) {
    ++stats.readyEvents;
    auto r = decodeReadyThread(e.payload);
    if (!r) {
      ++stats.malformedEvents;
      return;
    }
    // The readying thread is the header thread ([VERIFY]); its stack is kept even when it belongs to another
    // process, as long as the readied thread is a target.
    if (!keepAll && !isTargetThread(r->threadId, e.timestamp, 0) &&
        !isTargetThread(e.threadId, e.timestamp, e.processId)) {
      ++stats.readiesDropped;
      return;
    }
    const PendingParent::Candidate cands[2] = {{e.threadId, MatchRole::HeaderThread},
                                               {r->threadId, MatchRole::PayloadThread}};
    PendingParent& p = matcher->add(ParentKind::Ready, e.timestamp, cands, 2);
    p.cpu = e.cpu;
    p.headerTid = e.threadId;
    p.headerPid = e.processId;
    p.tidA = r->threadId;
    p.tidB = e.threadId;
    p.bytes[0] = static_cast<uint8_t>(r->adjustReason);
    p.bytes[1] = static_cast<uint8_t>(r->flag);
    usedTids.insert(r->threadId);
    usedTids.insert(e.threadId);
  }

  void onStack(const EtwEvent& e) {
    ++stats.stackWalkEvents;
    auto s = decodeStackWalk(e.payload);
    if (!s) {
      ++stats.malformedEvents;
      return;
    }
    const StackMatchResult r = matcher->onStack(*s);
    if (r.kind == StackMatchResult::Kind::Unmatched) {
      ++stats.stacksUnmatched;
      if (!keepAll && !isTargetThread(s->stackThread, e.timestamp, s->stackProcess))
        ++stats.stacksUnmatchedNonTarget;
      return;
    }
    ++stats.stacksMatched;
    if (r.kind == StackMatchResult::Kind::Matched)
      ++stats.matchRole[static_cast<size_t>(r.parent)][static_cast<size_t>(r.role)];
  }

  // ---- parse stage: finished parents -> batch
  // --------------------------------------------------------------
  void onParentDone(PendingParent& p) override {
    if (!current) newBatch();
    Batch& b = *current;
    const size_t kind = static_cast<size_t>(p.kind);
    StackRef ref;
    if (p.hasStack()) {
      ++stats.parentsWithStack[kind];
      if (p.twoPart) ++stats.stacksTwoPart;
      ref = resolve(p, b);
    } else {
      ++stats.parentsWithoutStack[kind];
    }
    switch (p.kind) {
      case ParentKind::Sample: {
        SampleItem s;
        s.ts = p.ts;
        s.tid = p.tidA;
        s.cpu = p.cpu;
        const uint64_t leaf = p.hasStack() && !p.frames.empty() ? p.frames[0] : p.ip;
        s.flags = leaf >= kKernelAddressBase ? trace::kSampleFlagKernelLeaf : 0;
        s.stack = ref;
        b.samples.push_back(s);
        ++stats.samplesWritten;
        break;
      }
      case ParentKind::CSwitch: {
        SwitchItem s;
        s.rec.ts = p.ts;
        s.rec.cpu = p.cpu;
        s.rec.new_tid = p.tidA;
        s.rec.old_tid = p.tidB;
        s.rec.old_state = p.bytes[0];
        s.rec.old_wait_reason = p.bytes[1];
        s.rec.old_wait_mode = p.bytes[2];
        s.rec.new_priority = p.bytes[3];
        s.rec.reserved =
            p.hasStack() && p.stackThread != p.tidB ? trace::kSwitchFlagStackFromOtherThread : 0u;
        s.stack = ref;
        b.switches.push_back(s);
        ++stats.cswitchesWritten;
        break;
      }
      case ParentKind::Ready: {
        ReadyItem r;
        r.rec.ts = p.ts;
        r.rec.cpu = p.cpu;
        r.rec.readied_tid = p.tidA;
        r.rec.readying_tid = p.tidB;
        r.rec.adjust_reason = p.bytes[0];
        r.rec.flags = p.bytes[1];
        r.rec.reserved =
            p.hasStack() && p.stackThread != p.tidB ? trace::kSwitchFlagStackFromOtherThread : 0u;
        r.stack = ref;
        b.readies.push_back(r);
        ++stats.readiesWritten;
        break;
      }
      case ParentKind::kCount: break;
    }
    if (b.events() >= opt.batchEvents) submit();
  }

  StackRef resolve(const PendingParent& p, Batch& b) {
    StackRef ref;
    ref.begin = static_cast<uint32_t>(b.frames.size());
    const ModuleBucket* user = modules->bucketFor(pidOfStack(p));
    const ModuleBucket& kernel = modules->kernelBucket();
    hintUser = hintKernel = ~size_t{0};
    for (uint64_t addr : p.frames) {
      ++stats.framesTotal;
      const bool isKernel = !isUserFrame(addr);
      const ModuleBucket* bucket = isKernel ? &kernel : user;
      const ModuleInterval* hit =
          bucket != nullptr ? bucket->find(addr, p.ts, isKernel ? hintKernel : hintUser) : nullptr;
      if (hit != nullptr) {
        b.frames.push_back(ResolvedFrame{addr - hit->base, hit->moduleIdx});
      } else {
        ++stats.framesUnmapped;
        if (opt.keepUnmappedFrames) b.frames.push_back(ResolvedFrame{addr, kInvalidIdx});
      }
    }
    ref.count = static_cast<uint32_t>(b.frames.size()) - ref.begin;
    return ref;
  }
};

// ---- ConversionPipeline
// ------------------------------------------------------------------------------------

ConversionPipeline::ConversionPipeline(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

ConversionPipeline::~ConversionPipeline() {
  if (m_impl && m_impl->worker.joinable()) {
    m_impl->queue->close();
    m_impl->worker.join();
  }
}

Result<std::unique_ptr<ConversionPipeline>> ConversionPipeline::create(const std::filesystem::path& out,
                                                                       ConvertOptions options,
                                                                       const EtlFileInfo& info) {
  if (info.clockFrequencyHz == 0)
    return makeError(ErrorCode::InvalidArgument, "clock frequency must be known");
  trace::FileHeader header;
  header.clock_frequency_hz = info.clockFrequencyHz;
  header.platform = trace::kPlatformWindows;
  TACHYON_TRY_ASSIGN(trace::TraceWriter writer, trace::TraceWriter::open(out, header));

  auto impl = std::make_unique<Impl>();
  impl->opt = std::move(options);
  impl->info = info;
  impl->targets = impl->opt.targetPids;
  std::sort(impl->targets.begin(), impl->targets.end());
  impl->keepAll = impl->targets.empty() || impl->opt.mode == CaptureMode::System;
  const DevicePathMapper mapper =
      impl->opt.deviceMapper != nullptr ? *impl->opt.deviceMapper : DevicePathMapper::fromSystem();
  impl->modules = std::make_unique<ModuleTable>(impl->opt.peLookup, mapper);
  impl->stage = std::make_unique<InternStage>(std::move(writer));
  impl->startedAt = std::chrono::steady_clock::now();
  impl->stats.clockFrequencyHz = info.clockFrequencyHz;
  auto pipeline = std::unique_ptr<ConversionPipeline>(new ConversionPipeline(std::move(impl)));
  pipeline->beginPass(PipelinePass::Main);
  return pipeline;
}

void ConversionPipeline::setFileInfo(const EtlFileInfo& info) {
  const uint64_t freq = m_impl->info.clockFrequencyHz;
  m_impl->info = info;
  m_impl->info.clockFrequencyHz = freq;
}

void ConversionPipeline::beginPass(PipelinePass pass) {
  Impl& s = *m_impl;
  s.pass = pass;
  s.eventsInPass = 0;
  if (pass == PipelinePass::Metadata) {
    s.prescanned = true;
    return;
  }
  if (s.prescanned) {
    s.pass1Min = s.seenMinTs;
    s.pass1Max = s.seenMaxTs;
  } else {
    s.pass1Min = 0;
    s.pass1Max = ~uint64_t{0} >> 1;
  }
  if (s.opt.progress) s.opt.progress(s.prescanned ? 0.1f : 0.0f);
  if (!s.matcher) {
    const uint64_t window = s.info.clockFrequencyHz * s.opt.stackWindowMs / 1000;
    s.matcher = std::make_unique<StackMatcher>(window, s);
    s.newBatch();
    if (s.opt.threaded) s.startWorker();
  }
}

void ConversionPipeline::onEvent(const EtwEvent& event) {
  m_impl->onEvent(event);
}

const ConvertStats& ConversionPipeline::stats() const {
  return m_impl->stats;
}

Result<ConvertStats> ConversionPipeline::finish() {
  Impl& s = *m_impl;
  if (s.finished) return makeError(ErrorCode::InvalidArgument, "finish() called twice");
  s.finished = true;
  if (!s.matcher) beginPass(PipelinePass::Main);

  // Release everything still waiting for a stack, then drain the intern stage.
  s.matcher->flush();
  s.submit();
  if (s.worker.joinable()) {
    s.queue->close();
    s.worker.join();
  }
  s.stage->flush();
  trace::TraceWriter& w = s.stage->writer();

  // Time range of the file: the kept records, else everything that was seen.
  uint64_t startTs = 0;
  uint64_t endTs = 0;
  if (s.stage->hasTs()) {
    startTs = s.stage->minTs();
    endTs = s.stage->maxTs();
  } else if (s.seenMaxTs >= s.seenMinTs) {
    startTs = s.seenMinTs;
    endTs = s.seenMaxTs;
  }
  w.setTimeRange(startTs, endTs);
  s.stats.startTs = startTs;
  s.stats.endTs = endTs;

  // Threads: every thread referenced by a kept record, plus all threads of the target processes.
  const std::vector<ThreadLife>& lives = s.meta.threads();
  std::vector<trace::ThreadRec> threadRecs;
  std::unordered_set<uint32_t> seenTid;
  std::unordered_set<uint32_t> pids;
  for (const ThreadLife& life : lives) {
    const bool target = s.isTargetPid(life.pid);
    if (!target && !s.usedTids.contains(life.tid)) continue;
    trace::ThreadRec rec{};
    rec.tid = life.tid;
    rec.pid = life.pid;
    rec.start_ts = life.start == 0 ? startTs : life.start;
    rec.end_ts = life.end == kOpenEnded ? endTs : life.end;
    rec.name = w.internString(life.name);
    rec.flags = target ? trace::kThreadFlagTargetProcess : 0;
    threadRecs.push_back(rec);
    seenTid.insert(life.tid);
    pids.insert(life.pid);
  }
  // Referenced threads without any lifetime event (started before the trace, no rundown): pid unknown (0).
  for (uint32_t tid : s.usedTids.list()) {
    if (seenTid.count(tid) != 0) continue;
    trace::ThreadRec rec{};
    rec.tid = tid;
    rec.pid = 0;
    rec.start_ts = startTs;
    rec.end_ts = endTs;
    rec.name = w.internString(tid == 0 ? "Idle" : "");
    threadRecs.push_back(rec);
    seenTid.insert(tid);
    pids.insert(0);
  }
  std::sort(threadRecs.begin(), threadRecs.end(), [](const trace::ThreadRec& a, const trace::ThreadRec& b) {
    return a.tid != b.tid ? a.tid < b.tid : a.start_ts < b.start_ts;
  });

  std::vector<trace::ProcessRec> processRecs;
  for (uint32_t pid : s.targets) pids.insert(pid);
  std::vector<uint32_t> pidList(pids.begin(), pids.end());
  std::sort(pidList.begin(), pidList.end());
  for (uint32_t pid : pidList) {
    trace::ProcessRec rec{};
    rec.pid = pid;
    auto it = s.meta.processes().find(pid);
    if (it != s.meta.processes().end()) {
      const ProcessInfo& p = it->second;
      rec.parent_pid = p.parentPid;
      rec.start_ts = p.start == 0 ? startTs : p.start;
      rec.end_ts = p.end == kOpenEnded ? endTs : p.end;
      rec.name = w.internString(p.name);
      rec.command_line = w.internString(p.commandLine);
    } else {
      rec.start_ts = startTs;
      rec.end_ts = endTs;
    }
    processRecs.push_back(rec);
  }

  std::vector<trace::ThreadNameRec> nameRecs;
  for (const ThreadNameEvent& n : s.meta.threadNames()) {
    if (seenTid.count(n.tid) == 0) continue;
    nameRecs.push_back(trace::ThreadNameRec{n.ts, n.tid, w.internString(n.name)});
  }

  std::vector<trace::ModuleRec> moduleRecs;
  moduleRecs.reserve(s.modules->modules().size());
  for (const LoadedModule& m : s.modules->modules()) {
    // The on-disk record is packed; build it in an aligned local and copy it into the array as bytes.
    alignas(8) trace::ModuleRec rec{};
    rec.pid = m.rec.pid;
    rec.base = m.rec.base;
    rec.size = m.rec.size;
    rec.path = w.internString(m.path);
    rec.id_kind = m.rec.id_kind;
    rec.id_len = m.rec.id_len;
    std::memcpy(rec.id, m.rec.id.data(), sizeof(rec.id));
    rec.age = m.rec.age;
    rec.load_ts = m.rec.load_ts == 0 ? startTs : m.rec.load_ts;
    rec.unload_ts = m.rec.unload_ts == kOpenEnded ? endTs : m.rec.unload_ts;
    moduleRecs.push_back(rec);
  }

  w.append(std::span<const trace::ProcessRec>(processRecs));
  w.append(std::span<const trace::ThreadRec>(threadRecs));
  w.append(std::span<const trace::ModuleRec>(moduleRecs));
  w.append(std::span<const trace::ThreadNameRec>(nameRecs));

  ConvertStats& st = s.stats;
  st.processesWritten = processRecs.size();
  st.threadsWritten = threadRecs.size();
  st.modulesWritten = moduleRecs.size();
  const ModuleIdCounters& mc = s.modules->counters();
  st.modulesRsds = mc.rsds;
  st.modulesFallback = mc.fallback;
  st.modulesFileMissing = mc.fileMissing;
  st.modulesNoCodeView = mc.noCodeView;
  st.modulesMismatch = mc.mismatch;
  st.eventsLost = std::max<uint64_t>(s.opt.eventsLost, s.info.eventsLost);
  st.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.startedAt).count();

  if (st.stackKeyEvents > 0) {
    st.warnings.push_back(std::to_string(st.stackKeyEvents) +
                          " stack-key events (StackWalk opcodes 34-37, stack caching) were skipped: "
                          "compressed stacks are not decoded; re-record without stack caching");
  }
  if (st.eventsLost > 0)
    st.warnings.push_back(std::to_string(st.eventsLost) + " events were lost during recording");
  if (st.modulesFallback > 0) {
    st.warnings.push_back(std::to_string(st.modulesFallback) + " modules use the TimeDateStamp+SizeOfImage " +
                          "fallback id (" + std::to_string(st.modulesFileMissing) + " file missing, " +
                          std::to_string(st.modulesMismatch) + " differ from disk, " +
                          std::to_string(st.modulesNoCodeView) + " without CodeView)");
  }

  // CaptureInfo: capture options, host info, events_lost, sampling_hz, agent version (Part 3.1) plus the
  // conversion statistics.
  using nlohmann::json;
  json doc;
  doc["source"] = "etl";
  doc["converter"] = {{"name", "tachyon-etlconvert"}, {"version", kConverterVersion}};
  doc["agent_version"] = s.opt.agentVersion;
  doc["sampling_hz"] = s.opt.samplingHz;
  doc["events_lost"] = st.eventsLost;
  doc["clock_frequency_hz"] = s.info.clockFrequencyHz;
  doc["mode"] = s.keepAll ? "system" : "process";
  doc["target_pids"] = s.targets;
  if (!s.opt.captureOptionsJson.empty()) {
    json parsed = json::parse(s.opt.captureOptionsJson, nullptr, false);
    doc["capture_options"] = parsed.is_discarded() ? json(s.opt.captureOptionsJson) : parsed;
  } else {
    doc["capture_options"] = json::object();
  }
  doc["host"] = {{"platform", "windows"},
                 {"logical_cores", s.info.numberOfProcessors},
                 {"pointer_size", s.info.pointerSize},
                 {"etl_buffers_lost", s.info.buffersLost},
                 {"etl_buffers_written", s.info.buffersWritten}};
  const char* roleNames[3] = {"header", "payload", "payload_old"};
  const char* kindNames[3] = {"sample", "cswitch", "ready"};
  json roles = json::object();
  json withStack = json::object();
  json withoutStack = json::object();
  for (size_t k = 0; k < 3; ++k) {
    json byRole = json::object();
    for (size_t r = 0; r < 3; ++r) byRole[roleNames[r]] = st.matchRole[k][r];
    roles[kindNames[k]] = byRole;
    withStack[kindNames[k]] = st.parentsWithStack[k];
    withoutStack[kindNames[k]] = st.parentsWithoutStack[k];
  }
  doc["convert_stats"] = {{"events_total", st.eventsTotal},
                          {"samples_written", st.samplesWritten},
                          {"samples_dropped", st.samplesDropped},
                          {"context_switches_written", st.cswitchesWritten},
                          {"ready_events_written", st.readiesWritten},
                          {"stacks_matched", st.stacksMatched},
                          {"stacks_unmatched", st.stacksUnmatched},
                          {"stack_match_roles", roles},
                          {"parents_with_stack", withStack},
                          {"parents_without_stack", withoutStack},
                          {"frames_total", st.framesTotal},
                          {"frames_unmapped", st.framesUnmapped},
                          {"modules_rsds", st.modulesRsds},
                          {"modules_fallback", st.modulesFallback},
                          {"warnings", st.warnings}};
  w.appendCaptureInfo(doc.dump());

  TACHYON_TRY(w.finish());
  if (s.opt.progress) s.opt.progress(1.0f);
  return s.stats;
}

}  // namespace tachyon::etlconvert
