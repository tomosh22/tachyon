// WP-42: data side of the timeline (thread summaries, frame-mark index, callstack descriptions, the LOD build
// job).
#include "viewer/timeline/timeline_data.h"
#include "analysis/thread_states.h"
#include "analysis/wait_reasons.h"
#include "symbols/function_key.h"
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::timeline;

namespace {

std::shared_ptr<const model::TraceModel> loadFixture(const char* name) {
  auto loaded = model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / name);
  EXPECT_TRUE(loaded.ok()) << (loaded.ok() ? "" : loaded.error().toString());
  model::TraceModel m = std::move(*loaded);
  EXPECT_TRUE(analysis::buildThreadStates(m).ok());
  return std::make_shared<const model::TraceModel>(std::move(m));
}

}  // namespace

TEST(TimelineData, ThreadSummariesMatchTheStateIntervals) {
  const auto m = loadFixture("tiny.tachyon");
  const auto sums = computeThreadSummaries(*m);
  ASSERT_EQ(sums.size(), m->threads.size());
  uint64_t anyRunning = 0;
  for (size_t t = 0; t < sums.size(); ++t) {
    uint64_t running = 0;
    for (const auto& s : m->statesByThread[t]) {
      if (s.state == model::ThreadState::Running) running += s.end - s.start;
    }
    EXPECT_EQ(sums[t].runningTicks, running);
    EXPECT_EQ(sums[t].sampleCount, m->samplesByThread[t].ts.size());
    anyRunning += running;
  }
  EXPECT_GT(anyRunning, 0u);
}

TEST(TimelineData, FrameTicksAreSortedAndDeduplicatedByNumber) {
  model::TraceModel m;
  m.frameMarks = {{300, ThreadIdx(0), StringIdx(), 3},
                  {100, ThreadIdx(0), StringIdx(), 1},
                  {200, ThreadIdx(1), StringIdx(), 2},
                  {105, ThreadIdx(1), StringIdx(), 1},  // same frame number from another thread
                  {200, ThreadIdx(0), StringIdx(), 2}};
  const auto ticks = buildFrameTicks(m);
  ASSERT_EQ(ticks.size(), 3u);
  EXPECT_EQ(ticks[0].ts, 100u);
  EXPECT_EQ(ticks[0].number, 1u);
  EXPECT_EQ(ticks[1].ts, 200u);
  EXPECT_EQ(ticks[2].number, 3u);
  EXPECT_EQ(firstFrameTickAtOrAfter(ticks, 0), 0u);
  EXPECT_EQ(firstFrameTickAtOrAfter(ticks, 100), 0u);
  EXPECT_EQ(firstFrameTickAtOrAfter(ticks, 101), 1u);
  EXPECT_EQ(firstFrameTickAtOrAfter(ticks, 300), 2u);
  EXPECT_EQ(firstFrameTickAtOrAfter(ticks, 301), 3u);
  EXPECT_TRUE(buildFrameTicks(model::TraceModel{}).empty());
}

TEST(TimelineData, FrameTicksOfTheFramesFixture) {
  const auto m = loadFixture("frames.tachyon");
  const auto ticks = buildFrameTicks(*m);
  ASSERT_FALSE(ticks.empty());
  for (size_t i = 1; i < ticks.size(); ++i) {
    EXPECT_LE(ticks[i - 1].ts, ticks[i].ts);
    EXPECT_NE(ticks[i - 1].number, ticks[i].number);
  }
}

TEST(TimelineData, CallstackDescriptionAndDepth) {
  model::TraceModel m;
  m.modules.resize(1);
  m.modules[0].path = "C:\\bin\\engine.exe";
  m.frames = {{ModuleIdx(0), 0x1a2b}, {ModuleIdx(), 0x7ff61234}};
  m.nodes = {{CallstackIdx(), FrameIdx()}, {CallstackIdx(0), FrameIdx(0)}, {CallstackIdx(1), FrameIdx(1)}};
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx(1)), "engine.exe!0x1a2b");
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx(2)), "0x7ff61234");
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx(0)), "(root)");
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx()), "?");
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx(77)), "?");
  EXPECT_EQ(callstackDepth(m, CallstackIdx(2)), 2u);
  EXPECT_EQ(callstackDepth(m, CallstackIdx(1)), 1u);
  EXPECT_EQ(callstackDepth(m, CallstackIdx(0)), 0u);
  EXPECT_EQ(callstackDepth(m, CallstackIdx()), 0u);
}

TEST(TimelineData, LodBuildRunsOnTheJobQueueAndPublishesIncrementally) {
  auto shared = std::make_shared<LodShared>(loadFixture("medium.tachyon"));
  EXPECT_EQ(shared->lodIfPrepared(), nullptr);
  EXPECT_EQ(shared->summariesIfReady(), nullptr);

  JobQueue jobs;
  bool done = false;
  startLodBuild(jobs, shared, {}, [&] { done = true; });
  EXPECT_EQ(jobs.activeCount(), 1u);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    jobs.pump();
    // Whatever has been published is consistent: ready threads never exceed the thread count.
    if (const auto* lod = shared->lodIfPrepared()) EXPECT_LE(lod->readyThreadCount(), lod->threadCount());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(done);
  EXPECT_TRUE(shared->finished.load());
  EXPECT_FALSE(shared->failed.load());
  ASSERT_NE(shared->summariesIfReady(), nullptr);
  const auto* lod = shared->lodIfPrepared();
  ASSERT_NE(lod, nullptr);
  EXPECT_EQ(lod->readyThreadCount(), shared->model->threads.size());
  EXPECT_EQ(jobs.activeCount(), 0u);
}

TEST(TimelineData, CancelledBuildKeepsTheModelAliveAndCallsBack) {
  auto shared = std::make_shared<LodShared>(loadFixture("medium.tachyon"));
  std::weak_ptr<const model::TraceModel> weak = shared->model;
  JobQueue jobs;
  bool done = false;
  const auto id = startLodBuild(jobs, shared, {}, [&] { done = true; });
  jobs.cancel(id);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    jobs.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(done);
  EXPECT_FALSE(shared->failed.load());
  shared.reset();
  EXPECT_TRUE(weak.expired());  // nothing leaks the model once the shared state is released
}

TEST(TimelineData, InlineBuildFinishesEverything) {
  LodShared shared(loadFixture("tiny.tachyon"));
  buildLodShared(shared, {}, nullptr, {});
  ASSERT_NE(shared.lodIfPrepared(), nullptr);
  EXPECT_EQ(shared.lodIfPrepared()->readyThreadCount(), shared.model->threads.size());
  EXPECT_FALSE(shared.failed.load());

  // A pre-set cancel flag stops after the summaries.
  LodShared cancelled(loadFixture("tiny.tachyon"));
  const std::atomic<bool> cancel{true};
  buildLodShared(cancelled, {}, &cancel, {});
  EXPECT_NE(cancelled.summariesIfReady(), nullptr);
  EXPECT_EQ(cancelled.lodIfPrepared(), nullptr);
}

// ---- WP-42b ----

namespace {

struct ResolvedFixture {
  std::shared_ptr<const model::TraceModel> model;
  std::unique_ptr<symbols::SymbolService> service;
};

ResolvedFixture resolveFixture(const char* trace, const char* symbolsJson) {
  ResolvedFixture r;
  r.model = loadFixture(trace);
  r.service = std::make_unique<symbols::SymbolService>(
      std::vector<std::string>{}, std::filesystem::path(TACHYON_BINARY_DIR) / "tl_symcache");
  EXPECT_TRUE(
      r.service->addSymbolFileOverride(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / symbolsJson)
          .ok());
  r.service->resolveAllAsync(const_cast<model::TraceModel&>(*r.model));
  r.service->waitAll();
  return r;
}

}  // namespace

TEST(TimelineSymbols, DescriptionsAndKeysUseTheSymbolService) {
  const ResolvedFixture f = resolveFixture("tiny.tachyon", "tiny.symbols.json");
  const model::TraceModel& m = *f.model;
  size_t resolved = 0, differentFromRva = 0;
  for (uint32_t n = 1; n < m.nodes.size(); ++n) {
    const CallstackIdx node(n);
    const FrameIdx fi = m.nodes[n].frame;
    const symbols::ResolvedAddress& r = f.service->lookup(fi);
    const auto key = functionKeyForNode(m, f.service.get(), node);
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(*key, symbols::functionKeyFor(*f.service, fi));
    const std::string withSymbols = describeCallstackLeaf(m, node, f.service.get());
    const std::string without = describeCallstackLeaf(m, node, nullptr);
    // "module!0x<rva>" without symbols (an unmapped frame is just the address).
    if (m.frames[fi.get()].module.valid()) {
      EXPECT_NE(without.find("!0x"), std::string::npos) << without;
    } else {
      EXPECT_EQ(without.rfind("0x", 0), 0u) << without;
    }
    if (r.resolved) {
      ++resolved;
      const std::string name(f.service->symbolStrings().get(r.function));
      ASSERT_FALSE(name.empty());
      EXPECT_EQ(withSymbols.substr(withSymbols.find('!') + 1), name);
      EXPECT_EQ(key->value, r.functionStartRva);
      if (r.functionStartRva != m.frames[fi.get()].addr_or_rva) ++differentFromRva;
      // Without symbols the same frame is keyed by its own rva.
      EXPECT_EQ(functionKeyForNode(m, nullptr, node)->value, m.frames[fi.get()].addr_or_rva);
    } else {
      EXPECT_EQ(withSymbols, without);
      EXPECT_EQ(key->value, m.frames[fi.get()].addr_or_rva);
    }
  }
  EXPECT_GT(resolved, 0u);
  EXPECT_GT(differentFromRva, 0u) << "keys group frames of one function (frames sit mid-function)";
}

TEST(TimelineSymbols, UnresolvedAndUnmappedFramesKeepTheirAddress) {
  model::TraceModel m;
  m.modules.resize(1);
  m.modules[0].path = "/opt/game/libengine.so";
  m.frames = {{ModuleIdx(0), 0x40}, {ModuleIdx(), 0xdead}};
  m.nodes = {{CallstackIdx(), FrameIdx()}, {CallstackIdx(0), FrameIdx(0)}, {CallstackIdx(1), FrameIdx(1)}};
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx(1)), "libengine.so!0x40");
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx(2)), "0xdead");
  const auto k = functionKeyForNode(m, nullptr, CallstackIdx(2));
  ASSERT_TRUE(k.has_value());
  EXPECT_TRUE(k->isUnmapped());
  EXPECT_EQ(k->value, 0xdeadu);
  EXPECT_FALSE(functionKeyForNode(m, nullptr, CallstackIdx(0)).has_value());
  EXPECT_FALSE(functionKeyForNode(m, nullptr, CallstackIdx()).has_value());
}

TEST(TimelineData, FrameMarkLayoutThinsTicksAndPlacesLabelsThatFit) {
  TimeLimits l;
  l.traceStart = 1000;
  l.traceEnd = 1000 + 100'000;
  std::vector<FrameTick> ticks;
  for (uint64_t i = 0; i < 1000; ++i) ticks.push_back({1000 + i * 100, i + 1});

  // 1000 px for 100 ticks of view: ticks are 10 px apart... narrow view: every mark and every label drawn.
  const TimeView zoomed{5000, 1000};  // 10 ticks of 100
  const auto sparse = layoutFrameMarks(ticks, zoomed, l, 100.0f, 1000.0f, 7.0f);
  ASSERT_GE(sparse.size(), 9u);
  for (const FrameMarkItem& m : sparse) EXPECT_TRUE(m.hasLabel || m.x > 1070.0f || m.x < 100.0f) << m.x;
  EXPECT_EQ(sparse[1].number, sparse[0].number + 1);

  // Whole trace: 1000 marks over 1000 px (1 px apart): thinned to >= 3 px, labels never overlap.
  const auto dense = layoutFrameMarks(ticks, fitView(l), l, 100.0f, 1000.0f, 7.0f);
  ASSERT_FALSE(dense.empty());
  EXPECT_LE(dense.size(), 1000u / 3 + 1);
  float lastLabelEnd = -1e9f;
  for (size_t i = 0; i < dense.size(); ++i) {
    if (i) EXPECT_GE(dense[i].x - dense[i - 1].x, 3.0f - 1e-3f);
    if (!dense[i].hasLabel) continue;
    EXPECT_GT(dense[i].x, lastLabelEnd);
    size_t digits = 1;
    for (uint64_t n = dense[i].number; n >= 10; n /= 10) ++digits;
    lastLabelEnd = dense[i].x + 2.0f + 7.0f * static_cast<float>(digits);
    EXPECT_LE(lastLabelEnd, 1100.0f);
  }
  EXPECT_TRUE(layoutFrameMarks({}, zoomed, l, 100.0f, 1000.0f, 7.0f).empty());
  EXPECT_TRUE(layoutFrameMarks(ticks, zoomed, l, 100.0f, 0.0f, 7.0f).empty());
}

TEST(TimelineData, TooltipTextNamesTimeStateWaitReasonAndTopFunction) {
  const ResolvedFixture f = resolveFixture("tiny.tachyon", "tiny.symbols.json");
  const model::TraceModel& m = *f.model;
  // Find a Waiting and a Running interval and a sampled node.
  const model::ThreadStateInterval* waiting = nullptr;
  const model::ThreadStateInterval* running = nullptr;
  ThreadIdx waitThread, runThread;
  for (size_t t = 0; t < m.statesByThread.size(); ++t) {
    for (const auto& s : m.statesByThread[t]) {
      if (!waiting && s.state == model::ThreadState::Waiting) {
        waiting = &s;
        waitThread = ThreadIdx(static_cast<uint32_t>(t));
      }
      if (!running && s.state == model::ThreadState::Running) {
        running = &s;
        runThread = ThreadIdx(static_cast<uint32_t>(t));
      }
    }
  }
  ASSERT_NE(waiting, nullptr);
  ASSERT_NE(running, nullptr);

  TooltipInput in;
  in.model = &m;
  in.symbols = f.service.get();
  in.clockHz = static_cast<double>(m.header.clock_frequency_hz);
  in.nsPerPixel = 1000.0;
  in.thread = waitThread;
  in.ts = waiting->start + (waiting->end - waiting->start) / 2;
  in.relTimeNs = static_cast<double>(in.ts - m.header.start_ts) * 1e9 / in.clockHz;
  CallstackIdx node;
  for (uint32_t n = 1; n < m.nodes.size() && !node.valid(); ++n) {
    if (f.service->lookup(m.nodes[n].frame).resolved) node = CallstackIdx(n);
  }
  ASSERT_TRUE(node.valid());
  in.stack = node;
  in.share = 0.75f;

  auto lines = tooltipLines(in);
  const std::string reason(analysis::waitReasonName(m.header.platform, waiting->wait_reason));
  const std::string cat(
      analysis::waitCategoryName(analysis::waitReasonCategory(m.header.platform, waiting->wait_reason)));
  const std::string thread = m.threads[waitThread.get()].name;
  EXPECT_EQ(lines[0].rfind(thread, 0), 0u);
  EXPECT_EQ(lines[1].rfind("Time: ", 0), 0u);
  EXPECT_EQ(lines[2], "State: Waiting - " + reason + " (" + cat + ")");
  EXPECT_EQ(lines[3].rfind("Interval: ", 0), 0u);
  const std::string fn = describeCallstackLeaf(m, node, f.service.get());
  EXPECT_EQ(lines[4], "Top function: " + fn + " (75% of samples here)");
  EXPECT_EQ(fn.find("!0x"), std::string::npos);

  in.thread = runThread;
  in.ts = running->start + (running->end - running->start) / 2;
  lines = tooltipLines(in);
  EXPECT_EQ(lines[2].rfind("State: Running (cpu ", 0), 0u);

  // Flame span, merged span, pending and empty variants.
  in.hasSpan = true;
  in.spanDepth = 3;
  in.spanNode = node;
  in.spanTicks = 10'000;
  in.spanMerged = 1;
  lines = tooltipLines(in);
  EXPECT_EQ(lines[4], "Depth 3: " + fn);
  EXPECT_EQ(lines[5], "Span: 1.00 ms");
  in.spanNode = CallstackIdx();
  in.spanMerged = 12;
  lines = tooltipLines(in);
  EXPECT_EQ(lines[4], "Depth 3: (merged)");
  EXPECT_EQ(lines.back(), "12 samples merged");
  in.pending = true;
  EXPECT_EQ(tooltipLines(in).back(), "Timeline index still building...");
  in.pending = false;
  in.hasSpan = false;
  in.stack = CallstackIdx();
  EXPECT_EQ(tooltipLines(in).back(), "No samples");
}
