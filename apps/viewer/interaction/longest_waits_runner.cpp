#include "viewer/interaction/longest_waits_runner.h"

#include <utility>

namespace tachyon::viewer::interaction {

struct LongestWaitsRunner::State {
  uint64_t generation = 0;
  uint64_t resultGeneration = 0;
  bool pending = false;
  uint64_t stale = 0;
  uint64_t total = 0;
  std::vector<WaitEpisode> rows;
  WaitQuery query;
};

LongestWaitsRunner::LongestWaitsRunner() : m_state(std::make_shared<State>()) {}
LongestWaitsRunner::~LongestWaitsRunner() = default;

uint64_t LongestWaitsRunner::request(std::shared_ptr<const model::TraceModel> model, const WaitQuery& query,
                                     JobQueue* jobs, std::function<void()> onDone) {
  State& s = *m_state;
  if (jobs && m_job != 0) jobs->cancel(m_job);
  const uint64_t gen = ++s.generation;
  s.query = query;
  if (!model) {
    s.rows.clear();
    s.total = 0;
    s.resultGeneration = gen;
    s.pending = false;
    return gen;
  }
  if (!jobs) {
    WaitScanResult r = longestWaits(*model, query, nullptr);
    s.rows = std::move(r.rows);
    s.total = r.totalWaits;
    s.resultGeneration = gen;
    s.pending = false;
    return gen;
  }
  s.pending = true;
  // The job owns its result; the completion (main thread) moves it into the state only when still current.
  auto result = std::make_shared<WaitScanResult>();
  std::shared_ptr<State> state = m_state;
  m_job = jobs->submit(
      "Longest waits",
      [model, query, result](JobContext& ctx) { *result = longestWaits(*model, query, ctx.cancelFlag()); },
      [state, result, gen, onDone = std::move(onDone)](JobQueue::JobId, JobOutcome outcome) {
        if (gen != state->generation) {
          ++state->stale;  // a newer request (or reset) exists
          return;
        }
        state->pending = false;
        if (outcome != JobOutcome::Completed || result->cancelled) return;
        state->rows = std::move(result->rows);
        state->total = result->totalWaits;
        state->resultGeneration = gen;
        if (onDone) onDone();
      });
  return gen;
}

void LongestWaitsRunner::cancel(JobQueue* jobs) {
  if (jobs && m_job != 0) jobs->cancel(m_job);
  ++m_state->generation;  // whatever is in flight is stale now
  m_state->pending = false;
}

void LongestWaitsRunner::reset(JobQueue* jobs) {
  cancel(jobs);
  m_state->rows.clear();
  m_state->total = 0;
  m_state->resultGeneration = 0;
}

uint64_t LongestWaitsRunner::generation() const {
  return m_state->generation;
}
uint64_t LongestWaitsRunner::resultGeneration() const {
  return m_state->resultGeneration;
}
bool LongestWaitsRunner::pending() const {
  return m_state->pending;
}
uint64_t LongestWaitsRunner::staleDiscarded() const {
  return m_state->stale;
}
const std::vector<WaitEpisode>& LongestWaitsRunner::rows() const {
  return m_state->rows;
}
uint64_t LongestWaitsRunner::totalWaits() const {
  return m_state->total;
}
const WaitQuery& LongestWaitsRunner::query() const {
  return m_state->query;
}

}  // namespace tachyon::viewer::interaction
