#include "analysis/query_executor.h"

#include <algorithm>
#include <new>

namespace tachyon::analysis {

namespace {
thread_local QueryExecutor* t_executor = nullptr;
thread_local Priority t_priority = Priority::Foreground;

bool isFinal(TaskState s) {
  return s == TaskState::Done || s == TaskState::Cancelled || s == TaskState::Superseded;
}
}  // namespace

TaskState TaskHandle::state() const {
  if (!m_ctl) return TaskState::Cancelled;
  std::lock_guard lock(m_ctl->mutex);
  return m_ctl->state;
}

bool TaskHandle::finished() const {
  return isFinal(state());
}

void TaskHandle::wait() const {
  if (!m_ctl) return;
  std::unique_lock lock(m_ctl->mutex);
  m_ctl->cv.wait(lock, [&] { return isFinal(m_ctl->state); });
}

bool TaskHandle::waitFor(std::chrono::milliseconds timeout) const {
  if (!m_ctl) return true;
  std::unique_lock lock(m_ctl->mutex);
  return m_ctl->cv.wait_for(lock, timeout, [&] { return isFinal(m_ctl->state); });
}

QueryExecutor::QueryExecutor(Options options) : m_options(options) {
  unsigned n = options.threads != 0 ? options.threads : std::thread::hardware_concurrency();
  n = std::max(1u, n);
  m_options.threads = n;
  m_options.backgroundEvery = std::max(1u, options.backgroundEvery);
  m_workers.reserve(n);
  for (unsigned i = 0; i < n; ++i) m_workers.emplace_back([this] { workerMain(); });
}

QueryExecutor::~QueryExecutor() {
  std::vector<std::shared_ptr<detail::TaskControl>> dropped;
  {
    std::lock_guard lock(m_mutex);
    m_stop = true;
    for (auto& q : m_queues) {
      for (auto& t : q) dropped.push_back(std::move(t));
      q.clear();
    }
    for (auto& t : m_runningTasks) t->cancelFlag.store(true, std::memory_order_relaxed);
  }
  m_cv.notify_all();
  m_idleCv.notify_all();
  for (auto& t : dropped) {
    t->cancelFlag.store(true, std::memory_order_relaxed);
    finish(t, TaskState::Cancelled);
  }
  for (std::thread& w : m_workers) w.join();
}

TaskHandle QueryExecutor::submit(Priority priority, std::function<void(Cancel)> fn) {
  SubmitOptions o;
  o.priority = priority;
  return submit(o, std::move(fn));
}

TaskHandle QueryExecutor::submit(const SubmitOptions& options, std::function<void(Cancel)> fn) {
  auto t = std::make_shared<detail::TaskControl>();
  t->priority = options.priority;
  t->guard = options.guard;
  t->generation =
      options.guard != nullptr && options.generation == 0 ? options.guard->current() : options.generation;
  t->fn = std::move(fn);
  t->onFinished = options.onFinished;
  bool rejected = false;
  {
    std::lock_guard lock(m_mutex);
    if (m_stop) {
      rejected = true;
    } else {
      t->id = m_nextId++;
      m_queues[static_cast<size_t>(options.priority)].push_back(t);
    }
  }
  if (rejected) {
    t->cancelFlag.store(true, std::memory_order_relaxed);
    finish(t, TaskState::Cancelled);
  } else {
    m_cv.notify_one();
  }
  return TaskHandle(std::move(t));
}

void QueryExecutor::cancel(const TaskHandle& handle) {
  if (!handle.m_ctl) return;
  const std::shared_ptr<detail::TaskControl>& t = handle.m_ctl;
  t->cancelFlag.store(true, std::memory_order_relaxed);
  bool removed = false;
  {
    std::lock_guard lock(m_mutex);
    auto& q = m_queues[static_cast<size_t>(t->priority)];
    const auto it = std::find(q.begin(), q.end(), t);
    if (it != q.end()) {
      q.erase(it);
      removed = true;
      // The queue may just have become empty: wake waitIdle() (the worker only notifies when it finishes).
      if (m_queues[0].empty() && m_queues[1].empty() && m_runningTasks.empty()) m_idleCv.notify_all();
    }
  }
  if (removed) finish(t, TaskState::Cancelled);
}

size_t QueryExecutor::queued(Priority priority) const {
  std::lock_guard lock(m_mutex);
  return m_queues[static_cast<size_t>(priority)].size();
}

size_t QueryExecutor::running() const {
  std::lock_guard lock(m_mutex);
  return m_runningTasks.size();
}

void QueryExecutor::waitIdle() const {
  std::unique_lock lock(m_mutex);
  m_idleCv.wait(lock, [&] { return m_queues[0].empty() && m_queues[1].empty() && m_runningTasks.empty(); });
}

Priority QueryExecutor::currentPriority() {
  return t_priority;
}

QueryExecutor* QueryExecutor::current() {
  return t_executor;
}

std::shared_ptr<detail::TaskControl> QueryExecutor::dequeueLocked() {
  auto& fg = m_queues[static_cast<size_t>(Priority::Foreground)];
  auto& bg = m_queues[static_cast<size_t>(Priority::Background)];
  std::deque<std::shared_ptr<detail::TaskControl>>* from = nullptr;
  if (!fg.empty() && (bg.empty() || m_foregroundStreak < m_options.backgroundEvery)) {
    from = &fg;
    ++m_foregroundStreak;
  } else if (!bg.empty()) {
    from = &bg;
    m_foregroundStreak = 0;
  } else {
    return nullptr;
  }
  auto t = std::move(from->front());
  from->pop_front();
  return t;
}

void QueryExecutor::finish(const std::shared_ptr<detail::TaskControl>& t, TaskState state) {
  {
    std::lock_guard lock(t->mutex);
    t->state = state;
  }
  t->cv.notify_all();
  if (t->onFinished) {
    try {
      t->onFinished(state);
    } catch (...) {
      // A throwing completion callback must not take the pool (or the caller of cancel()) down.
    }
  }
}

void QueryExecutor::workerMain() {
  t_executor = this;
  for (;;) {
    std::shared_ptr<detail::TaskControl> t;
    {
      std::unique_lock lock(m_mutex);
      for (;;) {
        if (m_stop) return;
        t = dequeueLocked();
        if (t) break;
        m_cv.wait(lock);
      }
      m_runningTasks.push_back(t);
    }
    bool skip = false;
    TaskState skipState = TaskState::Cancelled;
    if (t->cancelFlag.load(std::memory_order_relaxed)) {
      skip = true;
    } else if (t->guard != nullptr && !t->guard->isCurrent(t->generation)) {
      skip = true;
      skipState = TaskState::Superseded;
    }
    if (skip) {
      t->fn = nullptr;
      finish(t, skipState);
    } else {
      {
        std::lock_guard lock(t->mutex);
        t->state = TaskState::Running;
      }
      t_priority = t->priority;
      try {
        t->fn(Cancel{&t->cancelFlag});
      } catch (const std::bad_alloc&) {
        t->failure.store(static_cast<unsigned>(ErrorCode::OutOfMemory), std::memory_order_relaxed);
      } catch (...) {
        t->failure.store(static_cast<unsigned>(ErrorCode::Internal), std::memory_order_relaxed);
      }
      t_priority = Priority::Foreground;
      t->fn = nullptr;  // release the captures before waiters are released
      finish(t, TaskState::Done);
    }
    {
      std::lock_guard lock(m_mutex);
      m_runningTasks.erase(std::find(m_runningTasks.begin(), m_runningTasks.end(), t));
      if (m_queues[0].empty() && m_queues[1].empty() && m_runningTasks.empty()) m_idleCv.notify_all();
    }
  }
}

}  // namespace tachyon::analysis
