// JobQueue: background jobs with progress and cancellation; completions are delivered on the main thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::viewer {

enum class JobOutcome : uint8_t { Completed, Cancelled };

// Passed to the job body on its worker thread.
class JobContext {
 public:
  // Thread-safe.
  bool cancelled() const { return m_cancel.load(std::memory_order_relaxed); }
  // Thread-safe. `fraction` is clamped to [0,1].
  void setProgress(float fraction);
  // Thread-safe. Flag suitable for APIs that take `const std::atomic<bool>*` (e.g. model::LoadOptions).
  const std::atomic<bool>* cancelFlag() const { return &m_cancel; }

 private:
  friend class JobQueue;
  std::atomic<bool> m_cancel{false};
  std::atomic<float> m_progress{0.0f};
};

// One worker thread per job. All members other than JobContext's are main-thread only.
class JobQueue {
 public:
  using JobId = uint64_t;
  using Work = std::function<void(JobContext&)>;
  using Done = std::function<void(JobId, JobOutcome)>;

  struct Info {
    JobId id = 0;
    std::string label;
    float progress = 0.0f;
    bool cancelRequested = false;
  };

  JobQueue() = default;
  JobQueue(const JobQueue&) = delete;
  JobQueue& operator=(const JobQueue&) = delete;
  // Main-thread only. Cancels and joins every job; pending completions are dropped.
  ~JobQueue();

  // Main-thread only. Starts `work` on a new thread. `done` runs later from pump() on the main thread, with
  // Cancelled if cancel() was requested before the job finished.
  JobId submit(std::string label, Work work, Done done = {});
  // Main-thread only. Requests cancellation; the job body must poll JobContext::cancelled().
  void cancel(JobId id);
  // Main-thread only.
  void cancelAll();
  // Main-thread only. Joins finished jobs and invokes their completions in submission order. Completions may
  // submit or cancel jobs. Returns the number of completions run.
  size_t pump();

  // Main-thread only. Jobs submitted and not yet completed through pump().
  size_t activeCount() const { return m_jobs.size(); }
  // Main-thread only. Snapshot for the status bar.
  std::vector<Info> active() const;

 private:
  struct Job {
    JobId id = 0;
    std::string label;
    JobContext context;
    Done done;
    std::thread thread;
    std::atomic<bool> finished{false};
  };

  std::list<std::unique_ptr<Job>> m_jobs;
  JobId m_nextId = 1;
};

}  // namespace tachyon::viewer
