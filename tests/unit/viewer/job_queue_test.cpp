#include "viewer/job_queue.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace tachyon::viewer;
using namespace std::chrono_literals;

namespace {

// Pumps until `pred` holds or a generous timeout passes.
template <typename Pred>
bool pumpUntil(JobQueue& q, Pred pred) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    q.pump();
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

}  // namespace

TEST(JobQueue, CompletionRunsOnTheCallingThreadFromPump) {
  JobQueue q;
  std::thread::id workThread, doneThread;
  bool done = false;
  JobOutcome outcome = JobOutcome::Cancelled;
  q.submit(
      "job", [&](JobContext&) { workThread = std::this_thread::get_id(); },
      [&](JobQueue::JobId, JobOutcome o) {
        doneThread = std::this_thread::get_id();
        outcome = o;
        done = true;
      });
  ASSERT_TRUE(pumpUntil(q, [&] { return done; }));
  EXPECT_EQ(outcome, JobOutcome::Completed);
  EXPECT_NE(workThread, std::this_thread::get_id());
  EXPECT_EQ(doneThread, std::this_thread::get_id());
  EXPECT_EQ(q.activeCount(), 0u);
}

TEST(JobQueue, ProgressIsVisibleWhileRunning) {
  JobQueue q;
  std::atomic<bool> release{false}, reported{false};
  q.submit("p", [&](JobContext& ctx) {
    ctx.setProgress(0.5f);
    reported = true;
    while (!release) std::this_thread::sleep_for(1ms);
  });
  while (!reported) std::this_thread::sleep_for(1ms);
  const auto info = q.active();
  ASSERT_EQ(info.size(), 1u);
  EXPECT_EQ(info[0].label, "p");
  EXPECT_FLOAT_EQ(info[0].progress, 0.5f);
  EXPECT_FALSE(info[0].cancelRequested);
  release = true;
  ASSERT_TRUE(pumpUntil(q, [&] { return q.activeCount() == 0; }));
}

TEST(JobQueue, ProgressIsClamped) {
  JobQueue q;
  std::atomic<bool> release{false}, reported{false};
  q.submit("p", [&](JobContext& ctx) {
    ctx.setProgress(7.0f);
    reported = true;
    while (!release) std::this_thread::sleep_for(1ms);
  });
  while (!reported) std::this_thread::sleep_for(1ms);
  EXPECT_FLOAT_EQ(q.active()[0].progress, 1.0f);
  release = true;
  ASSERT_TRUE(pumpUntil(q, [&] { return q.activeCount() == 0; }));
}

TEST(JobQueue, CancelIsObservedByTheJobAndReported) {
  JobQueue q;
  std::atomic<bool> started{false};
  bool done = false;
  JobOutcome outcome = JobOutcome::Completed;
  const auto id = q.submit(
      "c",
      [&](JobContext& ctx) {
        started = true;
        while (!ctx.cancelled()) std::this_thread::sleep_for(1ms);
      },
      [&](JobQueue::JobId, JobOutcome o) {
        outcome = o;
        done = true;
      });
  while (!started) std::this_thread::sleep_for(1ms);
  q.cancel(id);
  EXPECT_TRUE(q.active()[0].cancelRequested);
  ASSERT_TRUE(pumpUntil(q, [&] { return done; }));
  EXPECT_EQ(outcome, JobOutcome::Cancelled);
}

TEST(JobQueue, CompletionsRunInSubmissionOrder) {
  JobQueue q;
  std::vector<int> order;
  for (int i = 0; i < 5; ++i) {
    q.submit("o", [](JobContext&) {}, [&order, i](JobQueue::JobId, JobOutcome) { order.push_back(i); });
  }
  std::this_thread::sleep_for(100ms);  // let every worker finish so one pump sees them all
  ASSERT_TRUE(pumpUntil(q, [&] { return order.size() == 5; }));
  EXPECT_EQ(order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST(JobQueue, CompletionMayCancelAndSubmit) {
  JobQueue q;
  bool second = false;
  q.submit(
      "a", [](JobContext&) {},
      [&](JobQueue::JobId, JobOutcome) {
        q.submit("b", [](JobContext&) {}, [&](JobQueue::JobId, JobOutcome) { second = true; });
      });
  ASSERT_TRUE(pumpUntil(q, [&] { return second; }));
}

TEST(JobQueue, DestructorCancelsAndJoinsRunningJobs) {
  std::atomic<bool> sawCancel{false};
  std::atomic<bool> started{false};
  {
    JobQueue q;
    q.submit("long", [&](JobContext& ctx) {
      started = true;
      while (!ctx.cancelled()) std::this_thread::sleep_for(1ms);
      sawCancel = true;
    });
    while (!started) std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(sawCancel);
}
