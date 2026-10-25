// Sample weight attribution (3.8 rule 1) and per-node accumulation on hand-built models.
#include "analysis/attribution.h"

#include "aggregation_test_util.h"

#include <gtest/gtest.h>

#include <atomic>
#include <vector>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;

// Unknown [0,1000), Running [1000,1250), Waiting [1250,2000), Running [2000,3000), Waiting [3000,end).
HandModel makeWeighted() {
  HandModel h;
  h.state(0, 0, 1000, ThreadState::Unknown);
  h.state(0, 1000, 1250, ThreadState::Running);
  h.state(0, 1250, 2000, ThreadState::Waiting);
  h.state(0, 2000, 3000, ThreadState::Running);
  h.state(0, 3000, 100000, ThreadState::Waiting);
  const CallstackIdx s = h.stack({0x1000, 0x1100});
  for (uint64_t ts : {500, 1000, 1100, 1200, 1220, 1300, 2900, 2950, 3050}) h.sample(0, ts, s);
  h.finish();
  return h;
}

TEST(AttributionTest, WeightsAreClippedAndZeroOutsideRunning) {
  HandModel h = makeWeighted();
  std::vector<std::pair<uint64_t, uint64_t>> got;
  ASSERT_TRUE(forEachWeightedSample(h.m, ThreadIdx(0), 0, UINT64_MAX, {},
                                    [&](uint64_t ts, CallstackIdx, uint64_t w) { got.emplace_back(ts, w); }));
  const std::vector<std::pair<uint64_t, uint64_t>> want = {{500, 0},    {1000, 100}, {1100, 100},
                                                           {1200, 50},  {1220, 30},  {1300, 0},
                                                           {2900, 100}, {2950, 50},  {3050, 0}};
  // 1200 is 50 before the end of the Running interval [1000,1250): clipped to 50.
  EXPECT_EQ(got, want);
}

TEST(AttributionTest, TimeRangeSelectsHalfOpenSampleRange) {
  HandModel h = makeWeighted();
  std::vector<uint64_t> got;
  ASSERT_TRUE(forEachWeightedSample(h.m, ThreadIdx(0), 1100, 1300, {},
                                    [&](uint64_t ts, CallstackIdx, uint64_t) { got.push_back(ts); }));
  EXPECT_EQ(got, (std::vector<uint64_t>{1100, 1200, 1220}));
}

TEST(AttributionTest, NodeWeightsSumPerLeafAndKeepZeroWeightSamples) {
  HandModel h = makeWeighted();
  const std::vector<ThreadIdx> threads = {ThreadIdx(0)};
  auto r = accumulateNodeWeights(h.m, 0, UINT64_MAX, threads, 0, {});
  ASSERT_TRUE(r.ok()) << r.error().toString();
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0].samples, 9u);
  EXPECT_EQ((*r)[0].ticks, 100u + 100 + 50 + 30 + 100 + 50);
}

TEST(AttributionTest, WorkerCountDoesNotChangeResult) {
  HandModel h(5);
  const CallstackIdx a = h.stack({0x1000});
  const CallstackIdx b = h.stack({0x1000, 0x2000});
  for (uint32_t t = 0; t < 5; ++t) {
    h.state(t, 0, 100000, ThreadState::Running);
    for (uint64_t i = 0; i < 200; ++i) h.sample(t, i * 70 + t, (i + t) % 3 ? a : b);
  }
  h.finish();
  std::vector<ThreadIdx> threads;
  for (uint32_t t = 0; t < 5; ++t) threads.push_back(ThreadIdx(t));
  auto one = accumulateNodeWeights(h.m, 0, UINT64_MAX, threads, 1, {});
  auto many = accumulateNodeWeights(h.m, 0, UINT64_MAX, threads, 4, {});
  ASSERT_TRUE(one.ok() && many.ok());
  ASSERT_EQ(one->size(), many->size());
  for (size_t i = 0; i < one->size(); ++i) {
    EXPECT_EQ((*one)[i].node, (*many)[i].node);
    EXPECT_EQ((*one)[i].ticks, (*many)[i].ticks);
    EXPECT_EQ((*one)[i].samples, (*many)[i].samples);
  }
}

TEST(AttributionTest, CancelledAccumulationReportsCancelled) {
  HandModel h = makeWeighted();
  std::atomic<bool> flag{true};
  const std::vector<ThreadIdx> threads = {ThreadIdx(0)};
  auto r = accumulateNodeWeights(h.m, 0, UINT64_MAX, threads, 1, Cancel{&flag});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}

TEST(AttributionTest, RejectsModelWithoutStates) {
  HandModel h;
  h.sample(0, 10, h.stack({0x1000}));
  h.finish();
  const std::vector<ThreadIdx> threads = {ThreadIdx(0)};
  EXPECT_FALSE(accumulateNodeWeights(h.m, 0, UINT64_MAX, threads, 1, {}).ok());
}

}  // namespace
}  // namespace tachyon::analysis
