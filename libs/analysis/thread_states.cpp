#include "analysis/thread_states.h"

#include "analysis/wait_reasons.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <span>
#include <thread>
#include <vector>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using model::ThreadStateInterval;
using model::TraceModel;

// Aligned local copy of a packed on-disk record (docs/specs/trace-format.md alignment rule): StrongId members
// can then be read without touching misaligned addresses.
template <typename T>
struct Local {
  alignas(8) T v;
  explicit Local(const T& src) {
    std::memcpy(static_cast<void*>(&v), static_cast<const void*>(&src), sizeof(T));
  }
};

// Per-thread lists of record indices (CSR layout): items of thread t are list[offset[t] .. offset[t + 1]).
struct Csr {
  std::vector<uint64_t> offset;
  std::vector<uint32_t> list;
  std::span<const uint32_t> of(size_t t) const {
    return {list.data() + offset[t], static_cast<size_t>(offset[t + 1] - offset[t])};
  }
};

struct Context {
  const TraceModel& model;
  uint32_t platform;
  uint64_t start;
  uint64_t end;
};

class ThreadBuilder {
 public:
  ThreadBuilder(const Context& ctx, uint32_t tid, uint64_t limit, std::vector<ThreadStateInterval>& out)
      : m_ctx(ctx), m_tid(tid), m_limit(limit), m_out(out), m_curStart(ctx.start) {}

  void run(std::span<const uint32_t> switches, std::span<const uint32_t> readies) {
    size_t si = 0, ri = 0;
    Local<trace::ContextSwitchRec> sw{trace::ContextSwitchRec{}};
    Local<trace::ReadyRec> rd{trace::ReadyRec{}};
    while (!m_finished) {
      // Next switch event of this thread (records with old_tid == new_tid are not switches).
      bool haveS = false;
      while (si < switches.size()) {
        sw = Local<trace::ContextSwitchRec>(m_ctx.model.contextSwitches[switches[si]]);
        if (sw.v.old_tid != sw.v.new_tid) {
          haveS = true;
          break;
        }
        ++si;
      }
      const bool haveR = ri < readies.size();
      if (haveR) rd = Local<trace::ReadyRec>(m_ctx.model.readyEvents[readies[ri]]);
      if (!haveS && !haveR) break;
      // Order at equal ts: switch-out, then ReadyRec, then switch-in.
      const bool sIsOut = haveS && sw.v.old_tid == m_tid;
      bool takeS = haveS;
      if (haveS && haveR) takeS = sw.v.ts < rd.v.ts || (sw.v.ts == rd.v.ts && sIsOut);
      const uint64_t ts = takeS ? sw.v.ts : rd.v.ts;
      if (ts > m_limit) break;
      const uint64_t t = std::max(ts, m_ctx.start);
      if (takeS) {
        ++si;
        if (sIsOut) {
          switchOut(t, sw.v);
        } else {
          switchIn(t, sw.v.cpu);
        }
      } else {
        ++ri;
        ready(t, rd.v);
      }
    }
    if (!m_finished) closeAt(m_limit);
  }

 private:
  enum class Cur : uint8_t { Unknown, Running, Off };

  struct Off {
    uint8_t reason = 0;
    bool preempted = false;
    bool gotReady = false;
    uint32_t cpu = 0;
    CallstackIdx blocking;
    CallstackIdx unblocking;
    uint32_t waker = 0;
    uint64_t readyTs = 0;
  };

  void push(const ThreadStateInterval& iv) {
    if (iv.end <= iv.start) return;
    if (iv.state == ThreadState::Unknown && !m_out.empty() && m_out.back().state == ThreadState::Unknown &&
        m_out.back().end == iv.start) {
      m_out.back().end = iv.end;
      return;
    }
    m_out.push_back(iv);
  }

  void emitUnknown(uint64_t a, uint64_t b) {
    ThreadStateInterval iv;
    iv.start = a;
    iv.end = b;
    iv.state = ThreadState::Unknown;
    push(iv);
  }

  void emitRunning(uint64_t a, uint64_t b) {
    ThreadStateInterval iv;
    iv.start = a;
    iv.end = b;
    iv.state = ThreadState::Running;
    iv.cpu = m_runCpu;
    push(iv);
  }

  void emitOff(ThreadState state, uint64_t a, uint64_t b, uint64_t readyTs) {
    ThreadStateInterval iv;
    iv.start = a;
    iv.end = b;
    iv.state = state;
    iv.wait_reason = m_off.reason;
    iv.cpu = m_off.cpu;
    iv.blocking_stack = m_off.blocking;
    iv.unblocking_stack = m_off.unblocking;
    iv.woke_by_tid = m_off.waker;
    iv.ready_ts = readyTs;
    push(iv);
  }

  void closeOff(uint64_t t) {
    if (m_off.preempted) {
      emitOff(ThreadState::Ready, m_curStart, t, m_off.gotReady ? m_off.readyTs : m_curStart);
    } else if (m_off.gotReady && m_off.readyTs < t) {
      if (m_off.readyTs > m_curStart) emitOff(ThreadState::Waiting, m_curStart, m_off.readyTs, m_off.readyTs);
      emitOff(ThreadState::Ready, std::max(m_curStart, m_off.readyTs), t, m_off.readyTs);
    } else {
      emitOff(ThreadState::Waiting, m_curStart, t, m_off.gotReady ? m_off.readyTs : 0);
    }
  }

  void closeAt(uint64_t t) {
    switch (m_cur) {
      case Cur::Unknown: emitUnknown(m_curStart, t); break;
      case Cur::Running: emitRunning(m_curStart, t); break;
      case Cur::Off: closeOff(t); break;
    }
  }

  void switchOut(uint64_t t, const trace::ContextSwitchRec& r) {
    if (m_cur == Cur::Off) {
      emitUnknown(m_curStart, t);  // two consecutive switch-outs: records were lost in between
    } else {
      closeAt(t);
    }
    if (oldStateIsExit(r.old_state)) {
      m_finished = true;
      return;
    }
    m_cur = Cur::Off;
    m_curStart = t;
    m_off = Off{};
    m_off.preempted = oldStateIsReady(r.old_state);
    m_off.reason = r.old_wait_reason;
    m_off.cpu = r.cpu;
    m_off.blocking = r.old_thread_stack;
  }

  void switchIn(uint64_t t, uint32_t cpu) {
    if (m_cur == Cur::Running) {
      emitUnknown(m_curStart, t);  // two consecutive switch-ins: records were lost in between
    } else {
      closeAt(t);
    }
    m_cur = Cur::Running;
    m_curStart = t;
    m_runCpu = cpu;
  }

  void ready(uint64_t t, const trace::ReadyRec& r) {
    if (m_cur != Cur::Off || m_off.gotReady) return;
    m_off.gotReady = true;
    m_off.readyTs = std::max(t, m_curStart);
    m_off.unblocking = r.readying_stack;
    m_off.waker = r.readying_tid;
  }

  const Context& m_ctx;
  uint32_t m_tid;
  uint64_t m_limit;
  std::vector<ThreadStateInterval>& m_out;
  Cur m_cur = Cur::Unknown;
  uint64_t m_curStart;
  uint32_t m_runCpu = 0;
  Off m_off;
  bool m_finished = false;
};

}  // namespace

Result<void> buildThreadStates(model::TraceModel& model, const BuildOptions& options) {
  const size_t threadCount = model.threads.size();
  if (model.contextSwitches.size() >= std::numeric_limits<uint32_t>::max() ||
      model.readyEvents.size() >= std::numeric_limits<uint32_t>::max())
    return makeError(ErrorCode::Unsupported, "too many scheduler records for thread state building");
  model.statesByThread.assign(threadCount, {});

  Context ctx{model, model.header.platform, model.header.start_ts, model.header.end_ts};
  if (ctx.end <= ctx.start || threadCount == 0) return ok();

  // ---- Partition the scheduler records per thread (sequential, O(records)) ----
  const size_t nCs = model.contextSwitches.size();
  std::vector<uint32_t> oldT(nCs, kInvalidIdx), newT(nCs, kInvalidIdx);
  Csr cs;
  cs.offset.assign(threadCount + 1, 0);
  std::vector<uint64_t> lastEvent(threadCount, 0);  // latest event ts the model knows for each thread
  auto lookup = [&](uint32_t tid) -> uint32_t {
    if (tid == 0) return kInvalidIdx;
    const auto it = model.tidToThread.find(tid);
    return it == model.tidToThread.end() ? kInvalidIdx : it->second.get();
  };
  for (size_t i = 0; i < nCs; ++i) {
    const Local<trace::ContextSwitchRec> r(model.contextSwitches[i]);
    if (r.v.old_tid == r.v.new_tid) continue;
    oldT[i] = lookup(r.v.old_tid);
    newT[i] = lookup(r.v.new_tid);
    if (oldT[i] != kInvalidIdx) {
      ++cs.offset[oldT[i] + 1];
      lastEvent[oldT[i]] = std::max(lastEvent[oldT[i]], r.v.ts);
    }
    if (newT[i] != kInvalidIdx) {
      ++cs.offset[newT[i] + 1];
      lastEvent[newT[i]] = std::max(lastEvent[newT[i]], r.v.ts);
    }
  }
  for (size_t t = 0; t < threadCount; ++t) cs.offset[t + 1] += cs.offset[t];
  cs.list.resize(cs.offset[threadCount]);
  {
    std::vector<uint64_t> fill(cs.offset.begin(), cs.offset.end() - 1);
    for (size_t i = 0; i < nCs; ++i) {
      if (oldT[i] != kInvalidIdx) cs.list[fill[oldT[i]]++] = static_cast<uint32_t>(i);
      if (newT[i] != kInvalidIdx) cs.list[fill[newT[i]]++] = static_cast<uint32_t>(i);
    }
  }
  std::vector<uint32_t>().swap(oldT);
  std::vector<uint32_t>().swap(newT);

  const size_t nRd = model.readyEvents.size();
  std::vector<uint32_t> readied(nRd, kInvalidIdx);
  Csr rd;
  rd.offset.assign(threadCount + 1, 0);
  for (size_t i = 0; i < nRd; ++i) {
    const Local<trace::ReadyRec> r(model.readyEvents[i]);
    const uint32_t waker = lookup(r.v.readying_tid);
    if (waker != kInvalidIdx) lastEvent[waker] = std::max(lastEvent[waker], r.v.ts);
    readied[i] = lookup(r.v.readied_tid);
    if (readied[i] != kInvalidIdx) {
      ++rd.offset[readied[i] + 1];
      lastEvent[readied[i]] = std::max(lastEvent[readied[i]], r.v.ts);
    }
  }
  for (size_t t = 0; t < threadCount; ++t) rd.offset[t + 1] += rd.offset[t];
  rd.list.resize(rd.offset[threadCount]);
  {
    std::vector<uint64_t> fill(rd.offset.begin(), rd.offset.end() - 1);
    for (size_t i = 0; i < nRd; ++i)
      if (readied[i] != kInvalidIdx) rd.list[fill[readied[i]]++] = static_cast<uint32_t>(i);
  }
  std::vector<uint32_t>().swap(readied);

  // Other event kinds only matter for telling a ThreadRec exit time from an event-derived Thread::last_ts.
  for (size_t t = 0; t < threadCount && t < model.samplesByThread.size(); ++t)
    if (!model.samplesByThread[t].ts.empty())
      lastEvent[t] = std::max(lastEvent[t], model.samplesByThread[t].ts.back());
  for (const trace::ScopeEventRec& e : model.scopeEvents) {
    const Local<trace::ScopeEventRec> r(e);
    const uint32_t t = lookup(r.v.tid);
    if (t != kInvalidIdx) lastEvent[t] = std::max(lastEvent[t], r.v.ts);
  }
  auto touch = [&](ThreadIdx t, uint64_t ts) {
    if (t.valid() && t.get() < threadCount) lastEvent[t.get()] = std::max(lastEvent[t.get()], ts);
  };
  for (const model::FiberSwitch& e : model.fiberSwitches) touch(e.thread, e.ts);
  for (const model::FrameMark& e : model.frameMarks) touch(e.thread, e.ts);
  for (const model::Marker& e : model.markers) touch(e.thread, e.ts);

  // ---- Build every thread in parallel ----
  unsigned workers = options.threads != 0 ? options.threads : std::thread::hardware_concurrency();
  workers = std::max(1u, std::min<unsigned>(workers, static_cast<unsigned>(threadCount)));
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  auto work = [&]() {
    try {
      for (;;) {
        const size_t t = next.fetch_add(1, std::memory_order_relaxed);
        if (t >= threadCount) return;
        const model::Thread& th = model.threads[t];
        uint64_t limit = ctx.end;
        // Thread::last_ts later than every known event can only come from ThreadRec::end_ts: the thread
        // exited.
        if (th.last_ts > lastEvent[t] && th.last_ts > ctx.start && th.last_ts < ctx.end) limit = th.last_ts;
        std::vector<ThreadStateInterval>& out = model.statesByThread[t];
        out.reserve(cs.of(t).size() + rd.of(t).size() + 2);
        ThreadBuilder b(ctx, th.tid, limit, out);
        b.run(cs.of(t), rd.of(t));
        out.shrink_to_fit();
      }
    } catch (...) {
      failed.store(true);
    }
  };
  std::vector<std::thread> pool;
  for (unsigned i = 1; i < workers; ++i) pool.emplace_back(work);
  work();
  for (std::thread& th : pool) th.join();
  if (failed.load()) return makeError(ErrorCode::OutOfMemory, "thread state building failed");
  return ok();
}

const model::ThreadStateInterval* stateAt(const model::TraceModel& model, ThreadIdx thread, uint64_t ts) {
  if (!thread.valid() || thread.get() >= model.statesByThread.size()) return nullptr;
  const auto& v = model.statesByThread[thread.get()];
  auto it = std::upper_bound(v.begin(), v.end(), ts,
                             [](uint64_t x, const model::ThreadStateInterval& iv) { return x < iv.start; });
  if (it == v.begin()) return nullptr;
  --it;
  return ts < it->end ? &*it : nullptr;
}

}  // namespace tachyon::analysis
