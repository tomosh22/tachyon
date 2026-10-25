// LatestQuery: the newest-wins guard of one view's analysis query (extracted from the Call Graph by WP-44).
#pragma once

#include "analysis/query_executor.h"
#include "core/result.h"

#include <functional>
#include <memory>
#include <optional>

namespace tachyon::viewer::common {

// The newest query of one view. issue() cancels and supersedes the previous one; poll() delivers the result
// of the newest request exactly once and never the result of a superseded one.
template <typename T>
class LatestQuery {
 public:
  using Value = std::shared_ptr<const T>;
  using Submitter = std::function<analysis::QueryFuture<Value>(const analysis::SubmitOptions&)>;

  LatestQuery() : m_token(std::make_shared<analysis::GenerationToken>()) {}
  LatestQuery(const LatestQuery&) = delete;
  LatestQuery& operator=(const LatestQuery&) = delete;
  // Cancels the request in flight (superseded ones were cancelled when they were replaced). The token is kept
  // alive by the queued tasks themselves.
  ~LatestQuery() { cancel(); }

  // Main-thread only. Starts a new generation, cancels the previous request and submits the new one through
  // `submit` (which receives the options carrying the guard and the generation). Foreground priority.
  void issue(analysis::QueryExecutor& executor, const Submitter& submit) {
    cancel();
    m_executor = &executor;
    analysis::SubmitOptions options;
    options.priority = analysis::Priority::Foreground;
    options.guard = m_token.get();
    options.generation = m_token->advance();
    options.onFinished = [token = m_token](analysis::TaskState) {};  // keeps the guard alive while queued
    m_future = submit(options);
    m_delivered = false;
    ++m_issued;
  }

  // Main-thread only. Cancels the request in flight and forgets it (a later poll() returns nothing).
  void cancel() {
    if (m_executor && m_future.handle().valid() && !m_future.handle().finished())
      m_executor->cancel(m_future.handle());
    m_token->advance();  // whatever is still queued or running is stale now
    m_future = {};
    m_delivered = true;
  }

  // Main-thread only. True while the newest request has not finished.
  bool pending() const { return m_future.handle().valid() && !m_delivered && !m_future.handle().finished(); }
  // Main-thread only. The newest request's result once it finished (then pending() is false). Nothing while
  // it runs, after it was delivered, or when it was cancelled / superseded.
  std::optional<Result<Value>> poll() {
    if (m_delivered || !m_future.handle().valid() || !m_future.handle().finished()) return std::nullopt;
    m_delivered = true;
    if (!m_future.handle().isCurrent()) return std::nullopt;  // stale
    const Result<Value>& r = m_future.get();
    if (!r.ok() && r.error().code == ErrorCode::Cancelled) return std::nullopt;
    return r;
  }

  // Number of requests issued so far (tests).
  uint64_t issued() const { return m_issued; }

 private:
  std::shared_ptr<analysis::GenerationToken> m_token;
  analysis::QueryExecutor* m_executor = nullptr;
  analysis::QueryFuture<Value> m_future;
  bool m_delivered = true;
  uint64_t m_issued = 0;
};

}  // namespace tachyon::viewer::common
