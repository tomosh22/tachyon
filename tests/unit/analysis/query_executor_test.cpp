// QueryExecutor: priorities, anti-starvation, cancellation, the stale-result guard and nested fan-out.
#include "analysis/query_executor.h"

#include "aggregation_test_util.h"
#include "analysis/query.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

namespace tachyon::analysis {
namespace {

using namespace std::chrono_literals;

// A task that blocks a single-thread executor until released.
struct Gate {
  std::mutex m;
  std::condition_variable cv;
  bool open = false;
  bool entered = false;

  void hold() {
    std::unique_lock lock(m);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return open; });
  }
  void waitEntered() {
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return entered; });
  }
  void release() {
    {
      std::lock_guard lock(m);
      open = true;
    }
    cv.notify_all();
  }
};

struct Order {
  std::mutex m;
  std::vector<int> ids;
  void add(int id) {
    std::lock_guard lock(m);
    ids.push_back(id);
  }
};

QueryExecutor::Options oneThread(unsigned backgroundEvery = 16) {
  QueryExecutor::Options o;
  o.threads = 1;
  o.backgroundEvery = backgroundEvery;
  return o;
}

TEST(QueryExecutorTest, RunsTasksAndReportsState) {
  QueryExecutor ex(oneThread());
  std::atomic<int> ran{0};
  TaskHandle h = ex.submit(Priority::Foreground, [&](Cancel c) {
    EXPECT_FALSE(c.requested());
    ++ran;
  });
  ASSERT_TRUE(h.valid());
  h.wait();
  EXPECT_EQ(ran.load(), 1);
  EXPECT_EQ(h.state(), TaskState::Done);
  EXPECT_TRUE(h.finished());
  EXPECT_TRUE(h.isCurrent());
  EXPECT_EQ(ex.threadCount(), 1u);
}

TEST(QueryExecutorTest, ForegroundIsDequeuedBeforeBackground) {
  Gate gate;  // declared before the executor: tasks use it until the executor is destroyed
  QueryExecutor ex(oneThread(1000));
  Order order;
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  // Queue background first, then foreground: foreground still runs first.
  for (int i = 0; i < 3; ++i) ex.submit(Priority::Background, [&, i](Cancel) { order.add(100 + i); });
  for (int i = 0; i < 3; ++i) ex.submit(Priority::Foreground, [&, i](Cancel) { order.add(i); });
  EXPECT_EQ(ex.queued(Priority::Background), 3u);
  EXPECT_EQ(ex.queued(Priority::Foreground), 3u);
  gate.release();
  ex.waitIdle();
  EXPECT_EQ(order.ids, (std::vector<int>{0, 1, 2, 100, 101, 102}));
}

TEST(QueryExecutorTest, BackgroundNeverStarvesUnderForegroundLoad) {
  Gate gate;  // declared before the executor: tasks use it until the executor is destroyed
  QueryExecutor ex(oneThread(4));
  Order order;
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  ex.submit(Priority::Background, [&](Cancel) { order.add(-1); });
  ex.submit(Priority::Background, [&](Cancel) { order.add(-2); });
  for (int i = 0; i < 12; ++i) ex.submit(Priority::Foreground, [&, i](Cancel) { order.add(i); });
  gate.release();
  ex.waitIdle();
  // The gate task was the first foreground dequeue; after every 4 consecutive foreground dequeues one
  // background task is taken.
  EXPECT_EQ(order.ids, (std::vector<int>{0, 1, 2, -1, 3, 4, 5, 6, -2, 7, 8, 9, 10, 11}));
}

TEST(QueryExecutorTest, CancellingAQueuedTaskNeverRunsIt) {
  Gate gate;  // declared before the executor: tasks use it until the executor is destroyed
  QueryExecutor ex(oneThread());
  std::atomic<int> ran{0};
  std::atomic<int> finishedState{-1};
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  SubmitOptions o;
  o.onFinished = [&](TaskState s) { finishedState = static_cast<int>(s); };
  TaskHandle h = ex.submit(o, [&](Cancel) { ++ran; });
  TaskHandle keep = ex.submit(Priority::Foreground, [&](Cancel) { ++ran; });
  ex.cancel(h);
  EXPECT_EQ(h.state(), TaskState::Cancelled);
  EXPECT_TRUE(h.cancelRequested());
  EXPECT_EQ(finishedState.load(), static_cast<int>(TaskState::Cancelled));
  EXPECT_EQ(ex.queued(Priority::Foreground), 1u);
  gate.release();
  keep.wait();
  ex.waitIdle();
  EXPECT_EQ(ran.load(), 1);
  h.wait();  // already final: returns at once
}

TEST(QueryExecutorTest, CancellingARunningTaskRaisesItsFlag) {
  QueryExecutor ex(oneThread());
  std::atomic<bool> started{false};
  TaskHandle h = ex.submit(Priority::Foreground, [&](Cancel c) {
    started = true;
    while (!c.requested()) std::this_thread::sleep_for(1ms);
  });
  while (!started) std::this_thread::sleep_for(1ms);
  EXPECT_EQ(h.state(), TaskState::Running);
  ex.cancel(h);
  ASSERT_TRUE(h.waitFor(2000ms));
  EXPECT_EQ(h.state(), TaskState::Done);
  EXPECT_TRUE(h.cancelRequested());
}

TEST(QueryExecutorTest, StaleGenerationsAreDroppedBeforeTheyRun) {
  Gate gate;  // declared before the executor: tasks use it until the executor is destroyed
  QueryExecutor ex(oneThread());
  std::atomic<int> ran{0};
  GenerationToken token;
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  SubmitOptions o;
  o.guard = &token;
  o.generation = token.advance();
  TaskHandle old = ex.submit(o, [&](Cancel) { ++ran; });
  EXPECT_TRUE(old.isCurrent());
  o.generation = token.advance();  // the user scrubbed again
  TaskHandle fresh = ex.submit(o, [&](Cancel) { ++ran; });
  EXPECT_FALSE(old.isCurrent());
  EXPECT_TRUE(fresh.isCurrent());
  gate.release();
  fresh.wait();
  old.wait();
  EXPECT_EQ(old.state(), TaskState::Superseded);
  EXPECT_EQ(fresh.state(), TaskState::Done);
  EXPECT_EQ(ran.load(), 1);
  // A running task that is superseded finishes normally, and the viewer sees it is stale.
  std::atomic<bool> inside{false};
  std::atomic<bool> go{false};
  o.generation = token.advance();
  TaskHandle running = ex.submit(o, [&](Cancel) {
    inside = true;
    while (!go) std::this_thread::sleep_for(1ms);
  });
  while (!inside) std::this_thread::sleep_for(1ms);
  token.advance();
  go = true;
  running.wait();
  EXPECT_EQ(running.state(), TaskState::Done);
  EXPECT_FALSE(running.isCurrent());
}

TEST(QueryExecutorTest, GuardWithoutExplicitGenerationUsesTheCurrentOne) {
  QueryExecutor ex(oneThread());
  GenerationToken token;
  token.advance();
  SubmitOptions o;
  o.guard = &token;
  TaskHandle h = ex.submit(o, [](Cancel) {});
  EXPECT_EQ(h.generation(), 1u);
  h.wait();
  EXPECT_EQ(h.state(), TaskState::Done);
}

TEST(QueryExecutorTest, SubmitQueryReturnsTheResult) {
  Gate gate;  // declared before the executor: tasks use it until the executor is destroyed
  QueryExecutor ex(oneThread());
  SubmitOptions o;
  auto f = ex.submitQuery<int>(o, [](Cancel) -> Result<int> { return 42; });
  ASSERT_TRUE(f.get().ok());
  EXPECT_EQ(*f.get(), 42);
  // A task that never ran reports Cancelled.
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  auto g = ex.submitQuery<int>(o, [](Cancel) -> Result<int> { return 1; });
  ex.cancel(g.handle());
  gate.release();
  EXPECT_EQ(g.get().error().code, ErrorCode::Cancelled);
}

TEST(QueryExecutorTest, DestructorCancelsQueuedTasksAndStopsRunningOnes) {
  std::atomic<int> ran{0};
  std::atomic<bool> started{false};
  TaskHandle queued, running;
  {
    QueryExecutor ex(oneThread());
    running = ex.submit(Priority::Foreground, [&](Cancel c) {
      started = true;
      while (!c.requested()) std::this_thread::sleep_for(1ms);
    });
    while (!started) std::this_thread::sleep_for(1ms);
    queued = ex.submit(Priority::Background, [&](Cancel) { ++ran; });
  }
  EXPECT_EQ(queued.state(), TaskState::Cancelled);
  EXPECT_EQ(running.state(), TaskState::Done);
  EXPECT_EQ(ran.load(), 0);
}

TEST(QueryExecutorTest, ManyTasksFromManyThreads) {
  QueryExecutor::Options o;
  o.threads = 4;
  QueryExecutor ex(o);
  std::atomic<int> sum{0};
  std::vector<std::thread> producers;
  for (int p = 0; p < 4; ++p)
    producers.emplace_back([&] {
      for (int i = 0; i < 500; ++i)
        ex.submit(i % 2 ? Priority::Foreground : Priority::Background, [&](Cancel) { ++sum; });
    });
  for (auto& t : producers) t.join();
  ex.waitIdle();
  EXPECT_EQ(sum.load(), 2000);
}

TEST(QueryExecutorTest, WaitIdleIsWokenWhenCancelEmptiesTheQueue) {
  // Regression: cancel() removed a queued task without waking waitIdle(); a hang (caught by the ctest
  // timeout) showed up after a few hundred iterations.
  QueryExecutor ex(oneThread());
  for (int i = 0; i < 5000; ++i) {
    TaskHandle h = ex.submit(Priority::Foreground, [](Cancel) {});
    std::thread canceller([&] { ex.cancel(h); });
    ex.waitIdle();
    canceller.join();
    h.wait();
  }
}

TEST(QueryExecutorTest, WaitIdleCompletesWhenASupersededTaskIsDropped) {
  Gate gate;
  GenerationToken token;
  QueryExecutor ex(oneThread());
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  SubmitOptions o;
  o.guard = &token;
  o.generation = token.advance();
  ex.submit(o, [](Cancel) {});
  token.advance();  // the queued task is now stale and will be dropped by the worker
  std::thread waiter([&] { ex.waitIdle(); });
  gate.release();
  waiter.join();
  EXPECT_EQ(ex.queued(Priority::Foreground), 0u);
}

TEST(QueryExecutorTest, ExceptionsAreReportedNotMistakenForCancellation) {
  QueryExecutor ex(oneThread());
  auto bad = ex.submitQuery<int>(SubmitOptions{}, [](Cancel) -> Result<int> { throw std::bad_alloc(); });
  EXPECT_EQ(bad.get().error().code, ErrorCode::OutOfMemory);
  EXPECT_EQ(bad.handle().failure(), ErrorCode::OutOfMemory);
  auto other =
      ex.submitQuery<int>(SubmitOptions{}, [](Cancel) -> Result<int> { throw std::runtime_error("boom"); });
  EXPECT_EQ(other.get().error().code, ErrorCode::Internal);
  EXPECT_EQ(other.handle().failure(), ErrorCode::Internal);
  TaskHandle raw = ex.submit(Priority::Foreground, [](Cancel) { throw 42; });
  raw.wait();
  EXPECT_EQ(raw.state(), TaskState::Done);
  EXPECT_EQ(raw.failure(), ErrorCode::Internal);
  auto fine = ex.submitQuery<int>(SubmitOptions{}, [](Cancel) -> Result<int> { return 1; });
  EXPECT_TRUE(fine.get().ok());
  EXPECT_EQ(fine.handle().failure(), ErrorCode::Ok);
}

TEST(QueryExecutorTest, ThrowingCompletionCallbackDoesNotHurtThePool) {
  Gate gate;
  QueryExecutor ex(oneThread());
  SubmitOptions o;
  o.onFinished = [](TaskState) { throw std::runtime_error("callback"); };
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  TaskHandle queued = ex.submit(o, [](Cancel) {});
  ex.cancel(queued);  // callback throws on this thread
  EXPECT_EQ(queued.state(), TaskState::Cancelled);
  gate.release();
  TaskHandle ran = ex.submit(o, [](Cancel) {});  // callback throws on the worker
  ran.wait();
  auto after = ex.submitQuery<int>(SubmitOptions{}, [](Cancel) -> Result<int> { return 5; });
  EXPECT_EQ(*after.get(), 5);
}

// ---- queries on an executor ----

using model::ThreadState;
using testutil::HandModel;

struct Hand {
  HandModel h{8};
  Hand() {
    const CallstackIdx main = h.stack({0x1000});
    const CallstackIdx a = h.push(main, 0x2000);
    const CallstackIdx b = h.push(a, 0x3000);
    for (uint32_t t = 0; t < 8; ++t) {
      h.state(t, 0, 1000000, ThreadState::Running);
      for (uint64_t i = 0; i < 2000; ++i) h.sample(t, i * 100, i % 3 == 0 ? b : (i % 3 == 1 ? a : main));
    }
    h.finish();
  }
};

TEST(QueryExecutorTest, QueriesFanOutThroughTheExecutorWithoutDeadlock) {
  Hand hand;
  // Reference without an executor.
  QueryContext refCtx(hand.h.m, *hand.h.svc);
  auto want = bottomUpFunctions(refCtx, {});
  ASSERT_TRUE(want.ok());

  // One executor thread, and the query runs ON that thread: every helper task stays queued behind it.
  for (unsigned threads : {1u, 2u}) {
    QueryExecutor::Options o;
    o.threads = threads;
    QueryExecutor ex(o);
    QueryContext ctx(hand.h.m, *hand.h.svc);
    ctx.setExecutor(&ex);
    auto f = ex.submitQuery<std::vector<FunctionRow>>(
        SubmitOptions{}, [&](Cancel c) { return bottomUpFunctions(ctx, {}, c); });
    ASSERT_TRUE(f.handle().waitFor(20000ms)) << "deadlock with " << threads << " executor threads";
    ASSERT_TRUE(f.get().ok());
    ASSERT_EQ(f.get()->size(), want->size());
    for (size_t i = 0; i < want->size(); ++i) {
      EXPECT_EQ((*f.get())[i].key, (*want)[i].key);
      EXPECT_EQ((*f.get())[i].inclusiveTicks, (*want)[i].inclusiveTicks);
      EXPECT_EQ((*f.get())[i].exclusiveTicks, (*want)[i].exclusiveTicks);
    }
    ex.waitIdle();  // leftover helper tasks drain
  }
}

TEST(QueryExecutorTest, ManyConcurrentQueriesSaturatingTheExecutorComplete) {
  Hand hand;
  QueryExecutor::Options o;
  o.threads = 3;
  QueryExecutor ex(o);
  QueryContext ctx(hand.h.m, *hand.h.svc);
  ctx.setExecutor(&ex);
  std::vector<QueryFuture<NodeAgg>> futures;
  for (uint64_t i = 0; i < 24; ++i) {
    Filter f;
    f.t1 = 1000 + i * 7919;  // distinct filters: every one scans
    SubmitOptions so;
    so.priority = i % 2 ? Priority::Background : Priority::Foreground;
    futures.push_back(ex.submitQuery<NodeAgg>(so, [&ctx, f](Cancel c) { return topDownTree(ctx, f, c); }));
  }
  for (auto& f : futures) {
    ASSERT_TRUE(f.handle().waitFor(30000ms)) << "deadlock";
    EXPECT_TRUE(f.get().ok());
  }
}

TEST(QueryExecutorTest, QueryCancelledThroughTheTaskFlag) {
  Hand hand;
  Gate gate;  // declared before the executor: tasks use it until the executor is destroyed
  QueryExecutor ex(oneThread());
  QueryContext ctx(hand.h.m, *hand.h.svc);
  ctx.setExecutor(&ex);
  ex.submit(Priority::Foreground, [&](Cancel) { gate.hold(); });
  gate.waitEntered();
  auto f = ex.submitQuery<std::vector<FunctionRow>>(SubmitOptions{},
                                                    [&](Cancel c) { return bottomUpFunctions(ctx, {}, c); });
  ex.cancel(f.handle());  // still queued behind the gate task: deterministic
  gate.release();
  EXPECT_EQ(f.get().error().code, ErrorCode::Cancelled);
  ex.waitIdle();
}

}  // namespace
}  // namespace tachyon::analysis
