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
  // Thread-safe. Connects once. See connectWithRetry for waiting on a starting agent. A "tcp://host:port"
  // address (WP-61) needs `token` (auth.h); the handshake is done before this returns, and a refused token
  // fails with ErrorCode::NoPrivilege. For local addresses `token` is ignored.
  static Result<Client> connect(const std::string& address, const std::string& token = {});
  // Thread-safe. Retries for up to timeoutMs while the agent is not yet listening (connection failures only:
  // a refused token is final).
  static Result<Client> connectWithRetry(const std::string& address, int timeoutMs,
                                         const std::string& token = {});

  Client(Client&&) noexcept = default;
  Client& operator=(Client&&) noexcept = default;

  // Sends a request and waits for the response with the matching id; events received meanwhile are queued.
  // The Response may carry ok=false; the Result fails only on transport/protocol errors or timeout.
  Result<Response> call(RequestBody body, int timeoutMs = 10000);

  // Returns the next event (queued first), waiting up to timeoutMs (<0: forever).
  Result<Event> nextEvent(int timeoutMs);

  // Waits for an event satisfying pred (earlier events are discarded). Fails with Timeout.
  Result<Event> waitForEvent(const std::function<bool(const Event&)>& pred, int timeoutMs);

  // WP-65a. Live events (`live`, `live_gap`, `live_end`) are handed to `handler` as they are read, by
  // whatever call is reading (call, nextEvent, waitForEvent), instead of being queued; a client that streams
  // a live capture can therefore keep waiting in call() without letting batches pile up. The handler runs on
  // the calling thread and must not call back into this Client. An empty handler restores queueing.
  using LiveHandler = std::function<void(const Event&)>;
  void setLiveHandler(LiveHandler handler) { m_liveHandler = std::move(handler); }

  // Closes the connection.
  void close();

 private:
  explicit Client(std::unique_ptr<IConnection> connection) : m_connection(std::move(connection)) {}

  std::unique_ptr<IConnection> m_connection;
  std::deque<Event> m_events;
  LiveHandler m_liveHandler;
  int64_t m_nextId = 1;
};

}  // namespace tachyon::control
