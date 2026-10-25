// QueryExecutor (WP-33): a prioritised thread pool for analysis queries.
//
// Scheduling rule. Two priorities, Foreground (what the user is waiting for) and Background (prefetch). A
// free worker takes the oldest Foreground task, except that after `backgroundEvery` consecutive Foreground
// dequeues while a Background task is waiting, it takes the oldest Background task instead. So Foreground is
// strictly preferred, but Background makes progress at a guaranteed rate of at least 1/(backgroundEvery+1) of
// the dequeues under a saturating Foreground load, and never starves forever. Tasks of one priority start in
// submission order.
//
// Cancellation. Every task gets its own cancel flag, passed to the task as `Cancel`. cancel(handle) removes a
// task that has not started (it never runs; state Cancelled) or raises the flag of a running one (the task
// returns early, normally with ErrorCode::Cancelled; its state still ends as Done).
//
// Stale results. A viewer that issues a new request when the user scrubs the timeline keeps a
// GenerationToken, takes `advance()` for each request and passes the token and the number in SubmitOptions.
// A queued task whose generation is no longer current when a worker reaches it is dropped without running
// (state Superseded), and the viewer drops any finished result for which handle.isCurrent() is false. It
// should also cancel() the handle of the request it supersedes so that a running one stops early.
//
// The executor is owned by the caller (no global). Queries given an executor (QueryContext::setExecutor)
// fan their scan out through it; see accumulateNodeWeights for why that cannot deadlock.
#pragma once

#include "analysis/query.h"
#include "core/result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace tachyon::analysis {

enum class Priority : uint8_t { Foreground, Background };

// Queued -> Running -> Done; or Queued -> Cancelled / Superseded (the task never ran).
enum class TaskState : uint8_t { Queued, Running, Done, Cancelled, Superseded };

// Generation counter for the stale-result guard. Lives in the caller (typically one per view).
class GenerationToken {
 public:
  // Thread-safe. Starts a new generation and returns its number (the first is 1).
  uint64_t advance() noexcept { return m_value.fetch_add(1, std::memory_order_acq_rel) + 1; }
  // Thread-safe. The newest generation.
  uint64_t current() const noexcept { return m_value.load(std::memory_order_acquire); }
  // Thread-safe. Whether `generation` is still the newest.
  bool isCurrent(uint64_t generation) const noexcept { return generation == current(); }

 private:
  std::atomic<uint64_t> m_value{0};
};

struct SubmitOptions {
  Priority priority = Priority::Foreground;
  // Stale-result guard: with a guard, the task belongs to `generation` (0 = guard->current() at submit time).
  // The guard must outlive the task.
  const GenerationToken* guard = nullptr;
  uint64_t generation = 0;
  // Called on the thread that finishes the task (a worker, or the thread calling cancel() / the destructor
  // for a task that never ran), after the state is final and waiters are released. Must not block.
  std::function<void(TaskState)> onFinished;
};

namespace detail {
struct TaskControl {
  uint64_t id = 0;
  Priority priority = Priority::Foreground;
  const GenerationToken* guard = nullptr;
  uint64_t generation = 0;
  std::function<void(Cancel)> fn;
  std::function<void(TaskState)> onFinished;
  std::atomic<bool> cancelFlag{false};
  mutable std::mutex mutex;
  std::condition_variable cv;
  TaskState state = TaskState::Queued;  // guarded by mutex
};
}  // namespace detail

// Reference to a submitted task. Cheap to copy; all members are thread-safe.
class TaskHandle {
 public:
  TaskHandle() = default;
  bool valid() const { return static_cast<bool>(m_ctl); }
  uint64_t id() const { return m_ctl ? m_ctl->id : 0; }
  TaskState state() const;
  // Done, Cancelled or Superseded.
  bool finished() const;
  // Whether cancel() was called (or the executor is shutting down).
  bool cancelRequested() const { return m_ctl && m_ctl->cancelFlag.load(std::memory_order_relaxed); }
  uint64_t generation() const { return m_ctl ? m_ctl->generation : 0; }
  // False when the task's guard has moved on to a newer generation (the result is stale). True without guard.
  bool isCurrent() const { return m_ctl && (!m_ctl->guard || m_ctl->guard->isCurrent(m_ctl->generation)); }
  // Blocks until the task has finished.
  void wait() const;
  // Blocks until the task has finished or `timeout` elapsed; true if finished.
  bool waitFor(std::chrono::milliseconds timeout) const;

 private:
  friend class QueryExecutor;
  explicit TaskHandle(std::shared_ptr<detail::TaskControl> c) : m_ctl(std::move(c)) {}
  std::shared_ptr<detail::TaskControl> m_ctl;
};

// A task that produces a Result<T>.
template <typename T>
class QueryFuture {
 public:
  QueryFuture() = default;
  // Thread-safe.
  const TaskHandle& handle() const { return m_handle; }
  // Thread-safe. Waits for the task; the error is Cancelled when the task never ran.
  const Result<T>& get() const {
    m_handle.wait();
    return m_slot->value;
  }

 private:
  friend class QueryExecutor;
  struct Slot {
    Result<T> value{makeError(ErrorCode::Cancelled, "task did not run")};
  };
  TaskHandle m_handle;
  std::shared_ptr<Slot> m_slot;
};

class QueryExecutor {
 public:
  struct Options {
    unsigned threads = 0;           // 0 = hardware_concurrency (at least 1)
    unsigned backgroundEvery = 16;  // see the scheduling rule above; at least 1
  };

  // Thread-safe (constructor starts the workers).
  explicit QueryExecutor(Options options);
  QueryExecutor() : QueryExecutor(Options{}) {}
  // Cancels queued tasks, raises the cancel flag of running ones and joins the workers.
  ~QueryExecutor();
  QueryExecutor(const QueryExecutor&) = delete;
  QueryExecutor& operator=(const QueryExecutor&) = delete;

  // Thread-safe. Queues `fn`; it receives the task's cancel token. `fn` must not throw out of the library
  // (an escaping exception is swallowed).
  TaskHandle submit(Priority priority, std::function<void(Cancel)> fn);
  TaskHandle submit(const SubmitOptions& options, std::function<void(Cancel)> fn);

  // Thread-safe. Queues a task whose result the caller reads with QueryFuture::get().
  template <typename T>
  QueryFuture<T> submitQuery(const SubmitOptions& options, std::function<Result<T>(Cancel)> fn) {
    QueryFuture<T> f;
    f.m_slot = std::make_shared<typename QueryFuture<T>::Slot>();
    f.m_handle = submit(options, [slot = f.m_slot, fn = std::move(fn)](Cancel c) { slot->value = fn(c); });
    return f;
  }

  // Thread-safe. Cancels the task as described in the header comment. No-op for a finished task.
  void cancel(const TaskHandle& handle);

  // All: thread-safe.
  unsigned threadCount() const { return static_cast<unsigned>(m_workers.size()); }
  size_t queued(Priority priority) const;
  size_t running() const;
  // Blocks until no task is queued or running.
  void waitIdle() const;

  // Thread-safe. The priority of the task running on the calling thread, or Foreground when the caller is not
  // an executor worker; helper tasks spawned by a task inherit it.
  static Priority currentPriority();
  // Thread-safe. The executor whose worker the calling thread is, or nullptr.
  static QueryExecutor* current();

 private:
  void workerMain();
  std::shared_ptr<detail::TaskControl> dequeueLocked();
  void finish(const std::shared_ptr<detail::TaskControl>& t, TaskState state);

  Options m_options;
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_cv;      // workers wait for tasks
  mutable std::condition_variable m_idleCv;  // waitIdle
  std::deque<std::shared_ptr<detail::TaskControl>> m_queues[2];
  std::vector<std::shared_ptr<detail::TaskControl>> m_runningTasks;
  unsigned m_foregroundStreak = 0;
  uint64_t m_nextId = 1;
  bool m_stop = false;
  std::vector<std::thread> m_workers;
};

}  // namespace tachyon::analysis
