// WP-46: the longest-waits job with its generation guard (stale results after a filter change).
#include "analysis/thread_states.h"
#include "viewer/interaction/longest_waits_runner.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::interaction;

namespace {

std::shared_ptr<const model::TraceModel> loadShared(const char* name) {
  auto loaded = model::TraceModel::load(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / name);
  EXPECT_TRUE(loaded.ok());
  auto m = std::make_shared<model::TraceModel>(std::move(loaded).value());
  EXPECT_TRUE(analysis::buildThreadStates(*m).ok());
  return m;
}

void drain(JobQueue& jobs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (jobs.activeCount() > 0 && std::chrono::steady_clock::now() < deadline) {
    jobs.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  jobs.pump();
  ASSERT_EQ(jobs.activeCount(), 0u);
}

}  // namespace

TEST(LongestWaitsRunner, SynchronousWithoutAJobQueue) {
  const auto m = loadShared("tiny.tachyon");
  LongestWaitsRunner runner;
  WaitQuery q;
  q.t0 = m->header.start_ts;
  q.maxResults = 7;
  const uint64_t gen = runner.request(m, q, nullptr);
  EXPECT_EQ(gen, 1u);
  EXPECT_FALSE(runner.pending());
  EXPECT_EQ(runner.resultGeneration(), gen);
  EXPECT_EQ(runner.rows().size(), 7u);
  EXPECT_EQ(runner.rows(), longestWaits(*m, q, nullptr).rows);
  EXPECT_EQ(runner.query(), q);
}

TEST(LongestWaitsRunner, DeliversOnTheMainThreadThroughPump) {
  const auto m = loadShared("medium.tachyon");
  JobQueue jobs;
  LongestWaitsRunner runner;
  WaitQuery q;
  q.t0 = m->header.start_ts;
  int done = 0;
  runner.request(m, q, &jobs, [&] { ++done; });
  EXPECT_TRUE(runner.pending());
  EXPECT_TRUE(runner.rows().empty());  // nothing is published before the completion runs on this thread
  drain(jobs);
  EXPECT_FALSE(runner.pending());
  EXPECT_EQ(done, 1);
  EXPECT_EQ(runner.rows(), longestWaits(*m, q, nullptr).rows);
  EXPECT_EQ(runner.resultGeneration(), runner.generation());
  EXPECT_EQ(runner.staleDiscarded(), 0u);
}

TEST(LongestWaitsRunner, ANewerRequestMakesTheOlderResultStale) {
  const auto m = loadShared("medium.tachyon");
  JobQueue jobs;
  LongestWaitsRunner runner;
  WaitQuery first;
  first.t0 = m->header.start_ts;
  WaitQuery second = first;
  second.t0 = m->header.start_ts + (m->header.end_ts - m->header.start_ts) / 2;
  second.maxResults = 9;
  int done = 0;
  const uint64_t g1 = runner.request(m, first, &jobs, [&] { ++done; });
  const uint64_t g2 = runner.request(m, second, &jobs, [&] { ++done; });  // before anything was pumped
  EXPECT_GT(g2, g1);
  drain(jobs);
  EXPECT_EQ(runner.resultGeneration(), g2);
  EXPECT_EQ(runner.rows(), longestWaits(*m, second, nullptr).rows);
  EXPECT_NE(runner.rows(), longestWaits(*m, first, nullptr).rows);
  EXPECT_EQ(runner.staleDiscarded(), 1u);
  EXPECT_EQ(done, 1);  // only the current result notifies
}

TEST(LongestWaitsRunner, CancelDropsTheResultInFlightAndKeepsTheOldRows) {
  const auto m = loadShared("tiny.tachyon");
  JobQueue jobs;
  LongestWaitsRunner runner;
  WaitQuery q;
  q.t0 = m->header.start_ts;
  runner.request(m, q, nullptr);
  const std::vector<WaitEpisode> before = runner.rows();
  ASSERT_FALSE(before.empty());
  WaitQuery other = q;
  other.maxResults = 1;
  runner.request(m, other, &jobs);
  runner.cancel(&jobs);
  drain(jobs);
  EXPECT_FALSE(runner.pending());
  EXPECT_EQ(runner.rows(), before);
  EXPECT_EQ(runner.staleDiscarded(), 1u);
  runner.reset(&jobs);
  EXPECT_TRUE(runner.rows().empty());
  EXPECT_EQ(runner.resultGeneration(), 0u);
}

TEST(LongestWaitsRunner, NullModelClearsTheRows) {
  const auto m = loadShared("tiny.tachyon");
  LongestWaitsRunner runner;
  WaitQuery q;
  q.t0 = m->header.start_ts;
  runner.request(m, q, nullptr);
  ASSERT_FALSE(runner.rows().empty());
  runner.request(nullptr, q, nullptr);
  EXPECT_TRUE(runner.rows().empty());
  EXPECT_FALSE(runner.pending());
}
