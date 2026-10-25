// Fan-out helper shared by the query implementations (WP-35). Not part of the public API.
#pragma once

#include "analysis/query_executor.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tachyon::analysis::detail {

// Thread-safe. Runs work(slot) for slot 0 on the calling thread and for slots 1..workers-1 on helpers: tasks
// of `executor` when it is not null (at the calling task's priority), else new threads. Returns when every
// slot that started has finished. Same contract as the fan-out in accumulateNodeWeights: with an executor,
// the caller never waits for a helper that has not started (a late helper finds the work closed and returns
// at once), so this cannot deadlock when the caller itself runs on a busy executor. `work` must pull its
// items from a shared source (an atomic counter), so that a slot that never runs loses nothing.
template <typename Fn>
void runSlots(QueryExecutor* executor, unsigned workers, Fn& work) {
  if (workers <= 1) {
    work(0u);
    return;
  }
  if (executor != nullptr) {
    struct Sync {
      std::mutex mutex;
      std::condition_variable cv;
      unsigned nextSlot = 1;
      unsigned active = 0;
      bool closed = false;
    };
    auto sync = std::make_shared<Sync>();
    std::vector<TaskHandle> helpers;
    helpers.reserve(workers - 1);
    for (unsigned i = 1; i < workers; ++i) {
      helpers.push_back(executor->submit(QueryExecutor::currentPriority(), [sync, workers, &work](Cancel) {
        unsigned slot;
        {
          std::lock_guard lock(sync->mutex);
          if (sync->closed || sync->nextSlot >= workers) return;
          slot = sync->nextSlot++;
          ++sync->active;
        }
        work(slot);
        {
          std::lock_guard lock(sync->mutex);
          --sync->active;
        }
        sync->cv.notify_all();
      }));
    }
    work(0u);
    {
      std::unique_lock lock(sync->mutex);
      sync->closed = true;
      sync->cv.wait(lock, [&] { return sync->active == 0; });
    }
    for (const TaskHandle& h : helpers) executor->cancel(h);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(workers - 1);
  for (unsigned i = 1; i < workers; ++i) pool.emplace_back([&work, i] { work(i); });
  work(0u);
  for (auto& th : pool) th.join();
}

}  // namespace tachyon::analysis::detail
