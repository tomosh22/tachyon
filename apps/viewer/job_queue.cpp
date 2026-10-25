#include "viewer/job_queue.h"

#include <algorithm>
#include <utility>

namespace tachyon::viewer {

void JobContext::setProgress(float fraction) {
  m_progress.store(std::clamp(fraction, 0.0f, 1.0f), std::memory_order_relaxed);
}

JobQueue::~JobQueue() {
  cancelAll();
  for (auto& job : m_jobs) {
    if (job->thread.joinable()) job->thread.join();
  }
}

JobQueue::JobId JobQueue::submit(std::string label, Work work, Done done) {
  auto job = std::make_unique<Job>();
  Job* raw = job.get();
  raw->id = m_nextId++;
  raw->label = std::move(label);
  raw->done = std::move(done);
  raw->thread = std::thread([raw, work = std::move(work)]() mutable {
    if (work) work(raw->context);
    raw->context.setProgress(1.0f);
    raw->finished.store(true, std::memory_order_release);
  });
  const JobId id = raw->id;
  m_jobs.push_back(std::move(job));
  return id;
}

void JobQueue::cancel(JobId id) {
  for (auto& job : m_jobs) {
    if (job->id == id) job->context.m_cancel.store(true, std::memory_order_relaxed);
  }
}

void JobQueue::cancelAll() {
  for (auto& job : m_jobs) job->context.m_cancel.store(true, std::memory_order_relaxed);
}

size_t JobQueue::pump() {
  // Detach finished jobs first so completions can freely submit or cancel.
  std::vector<std::unique_ptr<Job>> finished;
  for (auto it = m_jobs.begin(); it != m_jobs.end();) {
    if ((*it)->finished.load(std::memory_order_acquire)) {
      finished.push_back(std::move(*it));
      it = m_jobs.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& job : finished) {
    if (job->thread.joinable()) job->thread.join();
    if (job->done) {
      const JobOutcome outcome = job->context.cancelled() ? JobOutcome::Cancelled : JobOutcome::Completed;
      job->done(job->id, outcome);
    }
  }
  return finished.size();
}

std::vector<JobQueue::Info> JobQueue::active() const {
  std::vector<Info> out;
  out.reserve(m_jobs.size());
  for (const auto& job : m_jobs) {
    out.push_back({job->id, job->label, job->context.m_progress.load(std::memory_order_relaxed),
                   job->context.cancelled()});
  }
  return out;
}

}  // namespace tachyon::viewer
