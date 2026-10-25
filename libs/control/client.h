// Control protocol client: synchronous request/response with buffered unsolicited events.
#pragma once

#include "control/protocol.h"
#include "control/transport.h"

#include <deque>
#include <functional>
#include <memory>

namespace tachyon::control {

// Not thread-safe: use one Client per thread.
class Client {
 public:
  // Thread-safe. Connects once. See connectWithRetry for waiting on a starting agent.
  static Result<Client> connect(const std::string& address);
  // Thread-safe. Retries for up to timeoutMs while the agent is not yet listening.
  static Result<Client> connectWithRetry(const std::string& address, int timeoutMs);

  Client(Client&&) noexcept = default;
  Client& operator=(Client&&) noexcept = default;

  // Sends a request and waits for the response with the matching id; events received meanwhile are queued.
  // The Response may carry ok=false; the Result fails only on transport/protocol errors or timeout.
  Result<Response> call(RequestBody body, int timeoutMs = 10000);

  // Returns the next event (queued first), waiting up to timeoutMs (<0: forever).
  Result<Event> nextEvent(int timeoutMs);

  // Waits for an event satisfying pred (earlier events are discarded). Fails with Timeout.
  Result<Event> waitForEvent(const std::function<bool(const Event&)>& pred, int timeoutMs);

  // Closes the connection.
  void close();

 private:
  explicit Client(std::unique_ptr<IConnection> connection) : m_connection(std::move(connection)) {}

  std::unique_ptr<IConnection> m_connection;
  std::deque<Event> m_events;
  int64_t m_nextId = 1;
};

}  // namespace tachyon::control
