// Runs the "N longest waits" scan on the viewer JobQueue with a generation counter (WP-46): a result is only
// accepted when it belongs to the newest request, so a filter change while a scan is running never shows the
// older answer. Main-thread API; the scan itself touches only the immutable model.
#pragma once

#include "viewer/interaction/interaction_model.h"
#include "viewer/job_queue.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace tachyon::viewer::interaction {

class LongestWaitsRunner {
 public:
  LongestWaitsRunner();
  ~LongestWaitsRunner();
  LongestWaitsRunner(const LongestWaitsRunner&) = delete;
  LongestWaitsRunner& operator=(const LongestWaitsRunner&) = delete;

  // Main-thread only. Starts a scan (cancelling the one in flight) and returns its generation. With `jobs`
  // null the scan runs synchronously and the result is accepted immediately. `onDone` (main thread, from
  // JobQueue::pump) runs when an accepted result arrived. The model is kept alive by the job.
  uint64_t request(std::shared_ptr<const model::TraceModel> model, const WaitQuery& query, JobQueue* jobs,
                   std::function<void()> onDone = {});
  // Main-thread only. Cancels the scan in flight and invalidates its result; keeps the last rows.
  void cancel(JobQueue* jobs);
  // Main-thread only. Drops rows and any pending result (new document).
  void reset(JobQueue* jobs);

  // Main-thread only.
  uint64_t generation() const;        // generation of the newest request (0 = none)
  uint64_t resultGeneration() const;  // generation the rows belong to
  bool pending() const;               // the newest request has not delivered yet
  uint64_t staleDiscarded() const;    // results thrown away because a newer request existed
  const std::vector<WaitEpisode>& rows() const;
  uint64_t totalWaits() const;
  const WaitQuery& query() const;  // query of the newest request

 private:
  struct State;
  std::shared_ptr<State> m_state;
  JobQueue::JobId m_job = 0;
};

}  // namespace tachyon::viewer::interaction
