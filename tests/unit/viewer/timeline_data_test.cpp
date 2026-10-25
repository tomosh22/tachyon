// WP-42: data side of the timeline (thread summaries, frame-mark index, callstack descriptions, the LOD build
// job).
#include "viewer/timeline/timeline_data.h"
#include "analysis/thread_states.h"

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
  EXPECT_EQ(describeCallstackLeaf(m, CallstackIdx(1)), "engine.exe+0x1a2b");
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
