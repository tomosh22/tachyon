#include "control/client.h"

#include "control/tcp.h"

#include <chrono>
#include <thread>

namespace tachyon::control {

namespace {
using Clock = std::chrono::steady_clock;

// Remaining milliseconds until deadline, or -1 for "no deadline".
int remainingMs(bool hasDeadline, Clock::time_point deadline) {
  if (!hasDeadline) return -1;
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
  return left < 0 ? 0 : static_cast<int>(left);
}
}  // namespace

Result<Client> Client::connect(const std::string& address, const std::string& token) {
  const bool tcp = isTcpAddress(address);
  if (tcp && token.empty())
    return makeError(ErrorCode::InvalidArgument,
                     "a tcp:// agent needs a token (--token-file or TACHYON_AGENT_TOKEN_FILE)");
  auto conn = connectTo(address);
  if (!conn.ok()) return std::move(conn).error();
  Client client(std::move(*conn));
  if (tcp) {
    auto reply = client.call(AuthRequest{token}, 10000);
    if (!reply.ok()) return std::move(reply).error();
    if (!reply->ok) {
      client.close();
      return makeError(reply->code == ErrorCode::Ok ? ErrorCode::NoPrivilege : reply->code, reply->error);
    }
  }
  return client;
}

Result<Client> Client::connectWithRetry(const std::string& address, int timeoutMs, const std::string& token) {
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    auto c = connect(address, token);
    if (c.ok() || Clock::now() >= deadline) return c;
    // A wrong token or a bad address will not get better.
    if (c.error().code == ErrorCode::NoPrivilege || c.error().code == ErrorCode::InvalidArgument) return c;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

void Client::close() {
  if (m_connection) m_connection->close();
}

Result<Response> Client::call(RequestBody body, int timeoutMs) {
  if (!m_connection) return makeError(ErrorCode::IoError, "client is not connected");
  const Cmd cmd = cmdOf(body);
  Request req;
  req.id = m_nextId++;
  req.body = std::move(body);
  TACHYON_TRY(m_connection->writeLine(encodeRequest(req)));

  const bool hasDeadline = timeoutMs >= 0;
  const auto deadline = Clock::now() + std::chrono::milliseconds(hasDeadline ? timeoutMs : 0);
  std::string line;
  for (;;) {
    auto got = m_connection->readLine(line, remainingMs(hasDeadline, deadline));
    if (!got.ok()) return std::move(got).error();
    if (!*got) return makeError(ErrorCode::IoError, "connection closed by agent");
    auto msg = decodeServerMessage(line, cmd);
    if (!msg.ok()) return std::move(msg).error();
    if (auto* ev = std::get_if<Event>(&*msg)) {
      if (m_liveHandler && isLiveEvent(*ev)) {
        m_liveHandler(*ev);
      } else {
        m_events.push_back(std::move(*ev));
      }
      continue;
    }
    Response& resp = std::get<Response>(*msg);
    if (resp.id && *resp.id != req.id) continue;  // stale response; ignore
    return std::move(resp);
  }
}

Result<Event> Client::nextEvent(int timeoutMs) {
  if (!m_events.empty()) {
    Event e = std::move(m_events.front());
    m_events.pop_front();
    return e;
  }
  if (!m_connection) return makeError(ErrorCode::IoError, "client is not connected");
  const bool hasDeadline = timeoutMs >= 0;
  const auto deadline = Clock::now() + std::chrono::milliseconds(hasDeadline ? timeoutMs : 0);
  std::string line;
  for (;;) {
    auto got = m_connection->readLine(line, remainingMs(hasDeadline, deadline));
    if (!got.ok()) return std::move(got).error();
    if (!*got) return makeError(ErrorCode::IoError, "connection closed by agent");
    auto msg = decodeServerMessage(line, std::nullopt);
    if (!msg.ok()) return std::move(msg).error();
    if (auto* ev = std::get_if<Event>(&*msg)) {
      if (m_liveHandler && isLiveEvent(*ev)) {
        m_liveHandler(*ev);
        continue;
      }
      return std::move(*ev);
    }
    // A response with no outstanding request: ignore.
  }
}

Result<Event> Client::waitForEvent(const std::function<bool(const Event&)>& pred, int timeoutMs) {
  const bool hasDeadline = timeoutMs >= 0;
  const auto deadline = Clock::now() + std::chrono::milliseconds(hasDeadline ? timeoutMs : 0);
  for (;;) {
    auto e = nextEvent(remainingMs(hasDeadline, deadline));
    if (!e.ok()) return e;
    if (pred(*e)) return e;
  }
}

}  // namespace tachyon::control
