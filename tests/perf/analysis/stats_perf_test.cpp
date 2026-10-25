// Perf budgets (WP-35), each asserted at 2x:
//   functionStats of the top function on `medium`            < 200 ms
//   frames() on the `frames` fixture                          < 100 ms
//   frames() on a synthetic 100k-frame model                  < 100 ms
//   scopeStats on a synthetic 1 M-scope model (cold: pairing + selection) < 200 ms
// Measured numbers are printed; a memoized repeat must be near-instant.
#include "analysis/query.h"

#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace tachyon::analysis {
namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

struct Prepared {
  model::TraceModel m;
  symbols::SymbolService svc{std::vector<std::string>{}, std::filesystem::path{}};
};

bool prepare(Prepared& p, const std::string& name) {
  const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
  if (!std::filesystem::exists(dir / (name + ".tachyon"))) return false;
  auto r = model::TraceModel::load(dir / (name + ".tachyon"));
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  if (!r.ok()) return false;
  p.m = std::move(*r);
  EXPECT_TRUE(buildThreadStates(p.m).ok());
  EXPECT_TRUE(p.svc.addSymbolFileOverride(dir / (name + ".symbols.json")).ok());
  p.svc.resolveAllAsync(p.m);
  p.svc.waitAll();
  return true;
}

TEST(StatsPerf, FunctionStatsOfTheTopFunctionOnMedium) {
  Prepared p;
  if (!prepare(p, "medium")) GTEST_SKIP() << "medium fixture not generated";
  FunctionKey top;
  {
    QueryContext ctx(p.m, p.svc);
    auto rows = bottomUpFunctions(ctx, {});
    ASSERT_TRUE(rows.ok());
    ASSERT_FALSE(rows->empty());
    // The function with the most inclusive time that has a name (skip synthetic keys).
    top = (*std::max_element(rows->begin(), rows->end(), [](const FunctionRow& a, const FunctionRow& b) {
            return a.inclusiveTicks < b.inclusiveTicks;
          })).key;
  }
  size_t samples = 0;
  for (const auto& s : p.m.samplesByThread) samples += s.ts.size();
  double best = 1e30;
  uint32_t count = 0;
  for (int i = 0; i < 5; ++i) {
    QueryContext ctx(p.m, p.svc);  // empty cache each time
    const auto t0 = Clock::now();
    auto r = functionStats(ctx, {}, top);
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    count = r->count;
    best = std::min(best, s);
    std::printf("[stats perf] medium functionStats #%d: %.4f s\n", i, s);
    const auto t1 = Clock::now();
    auto again = functionStats(ctx, {}, top);
    ASSERT_TRUE(again.ok());
    EXPECT_LT(seconds(t1), 0.01) << "memoized repeat should be near-instant";
  }
  std::printf(
      "[stats perf] medium functionStats best: %.4f s (target 0.20 s, assert 0.40 s, %u hw threads); %zu "
      "samples, %u instances\n",
      best, std::thread::hardware_concurrency(), samples, count);
  EXPECT_GT(count, 0u);
  EXPECT_LT(best, 0.2 * 2);
}

TEST(StatsPerf, FramesOnTheFramesFixture) {
  Prepared p;
  if (!prepare(p, "frames")) GTEST_SKIP() << "frames fixture not generated";
  double best = 1e30;
  size_t n = 0;
  for (int i = 0; i < 5; ++i) {
    QueryContext ctx(p.m, p.svc);
    const auto t0 = Clock::now();
    auto r = frames(ctx, {});
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok());
    n = r->size();
    best = std::min(best, s);
    const auto t1 = Clock::now();
    auto slow = slowestFrames(ctx, {}, 10);
    ASSERT_TRUE(slow.ok());
    if (i == 0) std::printf("[stats perf] frames fixture slowestFrames(10): %.4f s\n", seconds(t1));
  }
  std::printf("[stats perf] frames fixture frames(): %.4f s for %zu frames (target 0.10 s, assert 0.20 s)\n",
              best, n);
  EXPECT_LT(best, 0.1 * 2);
}

// 100k frames of 1000 ticks, 4 threads each Running 875 of every 1000 ticks with 7 samples per frame.
TEST(StatsPerf, FramesOnASyntheticHundredThousandFrameModel) {
  constexpr uint64_t kFrames = 100000, kFrameTicks = 1000, kInterval = 125;
  constexpr uint32_t kThreads = 4, kStacks = 64, kFunctions = 16;
  model::TraceModel m;
  m.header.start_ts = 0;
  m.header.end_ts = kFrames * kFrameTicks + kFrameTicks;
  m.sampleIntervalTicks = kInterval;
  m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
  model::Module mod;
  mod.path = "game.exe";
  m.modules.push_back(mod);
  for (uint32_t f = 0; f < kFunctions; ++f)
    m.frames.push_back(model::Frame{ModuleIdx(0), 0x1000 + 0x100 * f + 0x10});
  std::vector<CallstackIdx> stacks;
  for (uint32_t s = 0; s < kStacks; ++s) {
    CallstackIdx cur(0);
    for (uint32_t d = 0; d < 6; ++d) {
      m.nodes.push_back(model::CallstackNode{cur, FrameIdx((s * 7 + d * 3) % kFunctions)});
      cur = CallstackIdx(static_cast<uint32_t>(m.nodes.size() - 1));
    }
    stacks.push_back(cur);
  }
  m.threads.resize(kThreads);
  m.samplesByThread.resize(kThreads);
  m.statesByThread.resize(kThreads);
  m.scopesByThread.resize(kThreads);
  for (uint32_t t = 0; t < kThreads; ++t) {
    m.threads[t].tid = 100 + t;
    m.tidToThread[100 + t] = ThreadIdx(t);
    auto& cols = m.samplesByThread[t];
    auto& ivs = m.statesByThread[t];
    for (uint64_t f = 0; f < kFrames + 1; ++f) {
      const uint64_t base = f * kFrameTicks;
      model::ThreadStateInterval run;
      run.start = base;
      run.end = base + 875;
      run.state = model::ThreadState::Running;
      ivs.push_back(run);
      model::ThreadStateInterval wait;
      wait.start = base + 875;
      wait.end = base + kFrameTicks;
      wait.state = model::ThreadState::Waiting;
      ivs.push_back(wait);
      for (uint64_t k = 0; k < 7; ++k) {
        cols.ts.push_back(base + k * kInterval);
        cols.callstack.push_back(stacks[(f * 7 + k + t) % kStacks]);
        cols.cpu.push_back(0);
        cols.flags.push_back(0);
      }
    }
  }
  const StringIdx name = m.strings.intern("Frame");
  for (uint64_t f = 0; f < kFrames + 1; ++f) {
    model::FrameMark fm;
    fm.ts = f * kFrameTicks;
    fm.thread = ThreadIdx(0);
    fm.name = name;
    fm.frame_number = f;
    m.frameMarks.push_back(fm);
  }
  symbols::SymbolService svc{std::vector<std::string>{}, std::filesystem::path{}};
  svc.resolveAllAsync(m);
  svc.waitAll();

  double best = 1e30;
  size_t n = 0;
  for (int i = 0; i < 5; ++i) {
    QueryContext ctx(m, svc);
    const auto t0 = Clock::now();
    auto r = frames(ctx, {});
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    n = r->size();
    best = std::min(best, s);
    std::printf("[stats perf] synthetic frames() #%d: %.4f s\n", i, s);
    if (i == 0) {
      const auto t1 = Clock::now();
      auto again = framesShared(ctx, {});  // a hit costs a lookup and a reference count
      ASSERT_TRUE(again.ok());
      EXPECT_LT(seconds(t1), 0.01) << "memoized repeat should be near-instant";
      const auto tc = Clock::now();
      auto copy = frames(ctx, {});  // the by-value form copies the 100k summaries
      ASSERT_TRUE(copy.ok());
      std::printf("[stats perf] synthetic frames() by-value cache hit (copy): %.4f s\n", seconds(tc));
      const auto t2 = Clock::now();
      auto slow = slowestFrames(ctx, {}, 20);
      ASSERT_TRUE(slow.ok());
      std::printf("[stats perf] synthetic slowestFrames(20): %.4f s\n", seconds(t2));
    }
  }
  std::printf(
      "[stats perf] synthetic frames(): %.4f s for %zu frames, %u threads, 2.8 M samples (target 0.10 s, "
      "assert 0.20 s, %u hw "
      "threads)\n",
      best, n, kThreads, std::thread::hardware_concurrency());
  EXPECT_EQ(n, kFrames);
  EXPECT_LT(best, 0.1 * 2);
}

// 4 threads x 125k iterations of (outer { inner }) = 1 M scopes, 2 M events.
TEST(StatsPerf, ScopeStatsOnASyntheticMillionScopeModel) {
  constexpr uint32_t kThreads = 4;
  constexpr uint64_t kIterations = 125000;
  model::TraceModel m;
  m.header.start_ts = 0;
  m.header.end_ts = kIterations * 1000 + 1000;
  m.sampleIntervalTicks = 100;
  m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
  m.threads.resize(kThreads);
  m.samplesByThread.resize(kThreads);
  m.statesByThread.resize(kThreads);
  m.scopesByThread.resize(kThreads);
  for (uint32_t t = 0; t < kThreads; ++t) {
    m.threads[t].tid = 100 + t;
    m.tidToThread[100 + t] = ThreadIdx(t);
  }
  const StringIdx outer = m.strings.intern("Outer"), inner = m.strings.intern("Inner");
  m.scopeEvents.reserve(kIterations * kThreads * 4);
  auto push = [&](uint64_t ts, uint32_t tid, uint8_t kind, StringIdx id) {
    trace::ScopeEventRec e{};
    e.ts = ts;
    e.tid = tid;
    e.kind = kind;
    e.id = id;
    e.caller_addr = 0x1000;
    m.scopeEvents.push_back(e);
  };
  for (uint64_t i = 0; i < kIterations; ++i) {
    for (uint32_t t = 0; t < kThreads; ++t) {
      const uint64_t base = i * 1000 + t;
      const uint32_t tid = 100 + t;
      push(base, tid, trace::kScopeBegin, outer);
      push(base + 100, tid, trace::kScopeBegin, inner);
      push(base + 100 + 50 + (i * 31 + t) % 300, tid, trace::kScopeEnd, {});
      push(base + 600 + (i * 17) % 300, tid, trace::kScopeEnd, {});
    }
  }
  // Interleaving threads at +t offsets keeps ts non-decreasing within a thread; make the stream globally
  // sorted.
  std::stable_sort(m.scopeEvents.begin(), m.scopeEvents.end(),
                   [](const trace::ScopeEventRec& a, const trace::ScopeEventRec& b) {
                     const uint64_t ta = a.ts, tb = b.ts;
                     return ta < tb;
                   });
  symbols::SymbolService svc{std::vector<std::string>{}, std::filesystem::path{}};
  double best = 1e30;
  uint32_t count = 0;
  for (int i = 0; i < 5; ++i) {
    QueryContext ctx(m, svc);
    const auto t0 = Clock::now();
    auto r = scopeStats(ctx, {}, inner);
    const double s = seconds(t0);
    ASSERT_TRUE(r.ok()) << r.error().toString();
    count = r->count;
    best = std::min(best, s);
    std::printf("[stats perf] synthetic scopeStats #%d (cold, incl. pairing): %.4f s\n", i, s);
    const auto t1 = Clock::now();
    auto other = scopeStats(ctx, {}, outer);  // pairing is cached
    ASSERT_TRUE(other.ok());
    if (i == 0)
      std::printf("[stats perf] synthetic scopeStats second id (pairing cached): %.4f s\n", seconds(t1));
  }
  std::printf(
      "[stats perf] synthetic scopeStats best: %.4f s for 1 M scopes (target 0.20 s, assert 0.40 s)\n", best);
  EXPECT_EQ(count, kIterations * kThreads);
  EXPECT_LT(best, 0.2 * 2);
}

}  // namespace
}  // namespace tachyon::analysis
