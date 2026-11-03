// WP-64: streaming load. A prefix preview is a consistent, self-contained model of the capture so far; the
// progressive loader hands out previews and then the complete model, and is cancellable.
#include "model/model.h"
#include "model_test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

namespace tachyon::model {
namespace {

using testing_util::fixturePath;

size_t sampleChunks(const TraceModel& m) {
  return m.loadStatus.sampleChunksTotal;
}

TEST(StreamingLoad, TheCompleteLoadReportsItselfComplete) {
  auto full = TraceModel::load(fixturePath("medium"));
  ASSERT_TRUE(full.ok());
  EXPECT_TRUE(full->loadStatus.complete);
  EXPECT_EQ(full->loadStatus.sampleChunksLoaded, full->loadStatus.sampleChunksTotal);
  EXPECT_GE(sampleChunks(*full), 4u);
  EXPECT_EQ(full->loadStatus.fullEndTs, full->header.end_ts);
  EXPECT_DOUBLE_EQ(full->loadStatus.fraction(), 1.0);
  uint64_t samples = 0;
  for (const auto& s : full->samplesByThread) samples += s.ts.size();
  EXPECT_EQ(full->loadStatus.samplesLoaded, samples);
}

TEST(StreamingLoad, APrefixPreviewIsAConsistentPrefixOfTheCompleteModel) {
  auto full = TraceModel::load(fixturePath("medium"));
  ASSERT_TRUE(full.ok());
  for (double fraction : {0.1, 0.25, 0.5, 0.9}) {
    SCOPED_TRACE(fraction);
    LoadOptions opt;
    opt.prefixFraction = fraction;
    opt.generation = 7;
    auto p = TraceModel::load(fixturePath("medium"), opt);
    ASSERT_TRUE(p.ok()) << p.error().toString();
    EXPECT_FALSE(p->loadStatus.complete);
    EXPECT_EQ(p->loadStatus.generation, 7u);
    EXPECT_EQ(p->loadStatus.sampleChunksTotal, full->loadStatus.sampleChunksTotal);
    EXPECT_LT(p->loadStatus.sampleChunksLoaded, p->loadStatus.sampleChunksTotal);
    EXPECT_GE(p->loadStatus.sampleChunksLoaded, 1u);
    EXPECT_EQ(p->loadStatus.fullEndTs, full->header.end_ts);
    // The capture so far ends where its samples end.
    EXPECT_LT(p->header.end_ts, full->header.end_ts);
    EXPECT_EQ(p->header.start_ts, full->header.start_ts);
    // Tables are complete: the same ids mean the same things.
    EXPECT_EQ(p->nodes.size(), full->nodes.size());
    EXPECT_EQ(p->frames.size(), full->frames.size());
    EXPECT_EQ(p->strings.size(), full->strings.size());
    EXPECT_EQ(p->modules.size(), full->modules.size());
    ASSERT_EQ(p->threads.size(), full->threads.size());
    for (size_t t = 0; t < p->threads.size(); ++t) {
      EXPECT_EQ(p->threads[t].tid, full->threads[t].tid);
      EXPECT_EQ(p->threads[t].name, full->threads[t].name);
      EXPECT_LE(p->threads[t].last_ts, p->header.end_ts);
    }
    EXPECT_EQ(p->sampleIntervalTicks, full->sampleIntervalTicks);
    // Samples: per thread the preview is a prefix of the complete model, all inside the clamped range.
    uint64_t samples = 0;
    for (size_t t = 0; t < p->threads.size(); ++t) {
      const SampleColumns& a = p->samplesByThread[t];
      const SampleColumns& b = full->samplesByThread[t];
      ASSERT_LE(a.ts.size(), b.ts.size());
      for (size_t i = 0; i < a.ts.size(); ++i) {
        ASSERT_EQ(a.ts[i], b.ts[i]);
        ASSERT_EQ(a.callstack[i].get(), b.callstack[i].get());
        ASSERT_EQ(a.cpu[i], b.cpu[i]);
        ASSERT_EQ(a.flags[i], b.flags[i]);
        ASSERT_LT(a.ts[i], p->header.end_ts);
      }
      samples += a.ts.size();
    }
    EXPECT_EQ(p->loadStatus.samplesLoaded, samples);
    EXPECT_GT(samples, 0u);
    EXPECT_LT(samples, full->loadStatus.samplesLoaded);
    // Events beyond the horizon are gone; the ones before it equal the complete model's.
    auto before = [&](const auto& all, size_t keptCount) {
      size_t n = 0;
      for (const auto& e : all) n += e.ts < p->header.end_ts ? 1 : 0;
      EXPECT_EQ(n, keptCount);
    };
    before(full->contextSwitches, p->contextSwitches.size());
    before(full->readyEvents, p->readyEvents.size());
    for (const auto& e : p->contextSwitches) EXPECT_LT(e.ts, p->header.end_ts);
    for (const auto& e : p->scopeEvents) EXPECT_LT(e.ts, p->header.end_ts);
    for (const FrameMark& e : p->frameMarks) EXPECT_LT(e.ts, p->header.end_ts);
    for (const Marker& e : p->markers) EXPECT_LT(e.ts, p->header.end_ts);
    // The model is as large as the part of the file it holds.
    EXPECT_LT(p->residentBytes(), full->residentBytes());
  }
}

TEST(StreamingLoad, PreviewIsIndependentOfTheWorkerCount) {
  LoadOptions one;
  one.threads = 1;
  one.prefixFraction = 0.3;
  auto a = TraceModel::load(fixturePath("medium"), one);
  ASSERT_TRUE(a.ok());
  const uint64_t fa = testing_util::fingerprint(*a);
  for (unsigned n : {2u, 5u}) {
    LoadOptions opt = one;
    opt.threads = n;
    auto b = TraceModel::load(fixturePath("medium"), opt);
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(testing_util::fingerprint(*b), fa) << n;
  }
}

TEST(StreamingLoad, FilesWithASingleSamplesChunkLoadCompletely) {
  LoadOptions opt;
  opt.prefixFraction = 0.5;
  auto p = TraceModel::load(fixturePath("tiny"), opt);
  auto full = TraceModel::load(fixturePath("tiny"));
  ASSERT_TRUE(p.ok() && full.ok());
  EXPECT_LE(sampleChunks(*full), 1u);
  EXPECT_TRUE(p->loadStatus.complete);
  EXPECT_EQ(testing_util::fingerprint(*p), testing_util::fingerprint(*full));
}

TEST(StreamingLoad, TornAndPartialFilesPreviewToo) {
  for (const char* name : {"partial", "partial_torn"}) {
    SCOPED_TRACE(name);
    LoadOptions opt;
    opt.prefixFraction = 0.5;
    auto p = TraceModel::load(fixturePath(name), opt);
    ASSERT_TRUE(p.ok()) << p.error().toString();
    auto full = TraceModel::load(fixturePath(name));
    ASSERT_TRUE(full.ok());
    EXPECT_TRUE(p->isPartial);
    EXPECT_LE(p->loadStatus.samplesLoaded, full->loadStatus.samplesLoaded);
  }
}

TEST(StreamingLoad, ProgressiveDeliversPreviewsInOrderThenTheCompleteModel) {
  auto full = TraceModel::load(fixturePath("medium"));
  ASSERT_TRUE(full.ok());
  std::vector<uint32_t> generations;
  std::vector<double> fractions;
  std::vector<float> progress;
  auto result = TraceModel::loadProgressive(
      fixturePath("medium"), {}, {0.1, 0.4},
      [&](TraceModel&& m) {
        EXPECT_FALSE(m.loadStatus.complete);
        generations.push_back(m.loadStatus.generation);
        fractions.push_back(m.loadStatus.fraction());
      },
      [&](float p) { progress.push_back(p); });
  ASSERT_TRUE(result.ok()) << result.error().toString();
  EXPECT_TRUE(result->loadStatus.complete);
  EXPECT_EQ(result->loadStatus.generation, 2u);
  ASSERT_EQ(generations.size(), 2u);
  EXPECT_EQ(generations[0], 0u);
  EXPECT_EQ(generations[1], 1u);
  EXPECT_LT(fractions[0], fractions[1]);
  EXPECT_LT(fractions[1], 1.0);
  ASSERT_FALSE(progress.empty());
  EXPECT_FLOAT_EQ(progress.back(), 1.0f);
  for (size_t i = 1; i < progress.size(); ++i) EXPECT_LE(progress[i - 1], progress[i]) << i;
  // The final model is the plain complete load.
  EXPECT_EQ(testing_util::fingerprint(*result), testing_util::fingerprint(*full));
}

TEST(StreamingLoad, ProgressiveIgnoresUnusableStagesAndSmallFiles) {
  int snapshots = 0;
  auto onSnapshot = [&](TraceModel&&) { ++snapshots; };
  auto r = TraceModel::loadProgressive(fixturePath("medium"), {}, {}, onSnapshot);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(snapshots, 0);
  EXPECT_TRUE(r->loadStatus.complete);
  r = TraceModel::loadProgressive(fixturePath("medium"), {}, {0.0, 1.0, 1.5, -1.0, 0.5, 0.3}, onSnapshot);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(snapshots, 1) << "only 0.5 is usable (0.3 is not ascending)";
  snapshots = 0;
  r = TraceModel::loadProgressive(fixturePath("tiny"), {}, {0.5}, onSnapshot);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(snapshots, 0) << "a single Samples chunk cannot be split";
  EXPECT_TRUE(r->loadStatus.complete);
}

TEST(StreamingLoad, CancellingAfterAPreviewStopsTheLoad) {
  std::atomic<bool> cancel{false};
  LoadOptions opt;
  opt.cancel = &cancel;
  int snapshots = 0;
  auto r = TraceModel::loadProgressive(fixturePath("medium"), opt, {0.25}, [&](TraceModel&&) {
    ++snapshots;
    cancel.store(true);  // the consumer closed the document while the next stage was about to start
  });
  EXPECT_EQ(snapshots, 1);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}

TEST(StreamingLoad, CancellingBeforeTheStartReturnsCancelled) {
  std::atomic<bool> cancel{true};
  LoadOptions opt;
  opt.cancel = &cancel;
  int snapshots = 0;
  auto r = TraceModel::loadProgressive(fixturePath("medium"), opt, {0.25}, [&](TraceModel&&) { ++snapshots; });
  EXPECT_EQ(snapshots, 0);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}

TEST(StreamingLoad, ErrorsPropagate) {
  auto r = TraceModel::loadProgressive("/nonexistent/file.tachyon", {}, {0.5}, {});
  EXPECT_FALSE(r.ok());
}

}  // namespace
}  // namespace tachyon::model
