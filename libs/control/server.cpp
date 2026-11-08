#include "control/server.h"

#include "control/sha256.h"
#include "core/log.h"

#include <algorithm>

namespace tachyon::control {

Server::Server(std::unique_ptr<IListener> listener, IRequestHandler& handler) : m_handler(handler) {
  auto entry = std::make_unique<ListenerEntry>();
  entry->listener = std::move(listener);
  m_listeners.push_back(std::move(entry));
}

Server::~Server() {
  stop();
}

void Server::addRemoteListener(std::unique_ptr<IListener> listener, AuthPolicy policy) {
  auto entry = std::make_unique<ListenerEntry>();
  entry->listener = std::move(listener);
  entry->remote = true;
  entry->policy = std::move(policy);
  m_listeners.push_back(std::move(entry));
}

std::string Server::remoteAddress() const {
  for (const auto& l : m_listeners)
    if (l->remote) return l->listener->address();
  return {};
}

void Server::start() {
  m_handler.onServerStarted(*this);
  for (auto& l : m_listeners) {
    ListenerEntry* raw = l.get();
    raw->acceptThread = std::thread([this, raw] { acceptLoop(raw); });
  }
}

void Server::stop() {
  std::vector<std::unique_ptr<ClientEntry>> clients;
  {
    std::lock_guard lock(m_mutex);
    if (m_stopped) return;
    m_stopped = true;
  }
  m_handler.onServerStopping();
  for (auto& l : m_listeners) l->listener->close();
  for (auto& l : m_listeners)
    if (l->acceptThread.joinable()) l->acceptThread.join();
  {
    std::lock_guard lock(m_mutex);
    clients = std::move(m_clients);
    m_clients.clear();
  }
  for (auto& c : clients) {
    c->connection->close();
    closeOutbound(*c);
  }
  for (auto& c : clients) {
    if (c->thread.joinable()) c->thread.join();
    if (c->writerThread.joinable()) c->writerThread.join();
  }
}

bool Server::enqueue(ClientEntry& entry, std::string line) {
  {
    std::lock_guard lock(entry.outMutex);
    if (entry.outClosed) return false;
    if (entry.outBytes > 0 && entry.outBytes + line.size() > kMaxQueuedBytesPerClient) {
      // The client is not reading: drop it rather than let it hold anything up.
      entry.outClosed = true;
    } else {
      entry.outBytes += line.size();
      entry.outQueue.push_back(std::move(line));
      entry.outCv.notify_one();
      return true;
    }
  }
  TACHYON_LOG_WARN("control: client {} is not reading (outbound queue over {} bytes); dropping it", entry.id,
                   kMaxQueuedBytesPerClient);
  entry.connection->close();
  entry.outCv.notify_all();
  return false;
}

void Server::closeOutbound(ClientEntry& entry) {
  {
    std::lock_guard lock(entry.outMutex);
    entry.outClosed = true;
  }
  entry.outCv.notify_all();
}

void Server::writerLoop(ClientEntry* entry) {
  for (;;) {
    std::string line;
    {
      std::unique_lock lock(entry->outMutex);
      entry->outCv.wait(lock, [&] { return entry->outClosed || !entry->outQueue.empty(); });
      if (entry->outQueue.empty()) return;  // closed and drained (or dropped)
      line = std::move(entry->outQueue.front());
      entry->outQueue.pop_front();
      entry->outBytes -= line.size();
    }
    if (!entry->connection->writeLine(line).ok()) {
      entry->connection->close();
      closeOutbound(*entry);
      return;
    }
  }
}

void Server::reapFinished() {
  std::vector<std::unique_ptr<ClientEntry>> done;
  {
    std::lock_guard lock(m_mutex);
    for (auto it = m_clients.begin(); it != m_clients.end();) {
      if ((*it)->finished.load()) {
        done.push_back(std::move(*it));
        it = m_clients.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& c : done) {
    if (c->thread.joinable()) c->thread.join();
    if (c->writerThread.joinable()) c->writerThread.join();
  }
}

void Server::acceptLoop(ListenerEntry* listener) {
  for (;;) {
    auto conn = listener->listener->accept();
    if (!conn.ok()) {
      if (conn.error().code != ErrorCode::Cancelled)
        TACHYON_LOG_ERROR("control: accept failed: {}", conn.error().toString());
      return;
    }
    reapFinished();
    auto entry = std::make_unique<ClientEntry>();
    entry->connection = std::shared_ptr<IConnection>(std::move(*conn));
    entry->remote = listener->remote;
    if (entry->remote) {
      entry->policy = &listener->policy;
      entry->authed.store(false);
      // Too many half-open connections (or too many clients): refuse before spending two threads on this one.
      if (m_pending.load() >= listener->policy.maxPending) {
        TACHYON_LOG_WARN("control: refusing {}: too many unauthenticated connections",
                         entry->connection->peerAddress());
        entry->connection->close();
        continue;
      }
      m_pending.fetch_add(1);
    }
    ClientEntry* raw = entry.get();
    {
      std::lock_guard lock(m_mutex);
      if (m_stopped) {
        if (entry->remote) m_pending.fetch_sub(1);
        return;
      }
      entry->id = m_nextId++;
      m_clients.push_back(std::move(entry));
    }
    // Local clients are known to the handler at once; remote ones after the handshake (admit()).
    if (!raw->remote) admit(*raw);
    raw->writerThread = std::thread([this, raw] { writerLoop(raw); });
    raw->thread = std::thread([this, raw] { clientLoop(raw); });
  }
}

void Server::admit(ClientEntry& entry) {
  ClientInfo info;
  info.remote = entry.remote;
  info.peer = entry.connection->peerAddress();
  entry.announced = true;
  m_handler.onClientAccepted(entry.id, info);
  m_handler.onConnect(entry.id);
}

bool Server::FailureLimiter::blocked(const std::string& host, uint32_t maxFailures, int windowMs) {
  std::lock_guard lock(m_mutex);
  const auto now = Clock::now();
  const auto window = std::chrono::milliseconds(windowMs);
  auto it = m_failures.find(host);
  if (it == m_failures.end()) return false;
  auto& q = it->second;
  while (!q.empty() && now - q.front() > window) q.pop_front();
  if (q.empty()) {
    m_failures.erase(it);
    return false;
  }
  return q.size() >= maxFailures;
}

void Server::FailureLimiter::record(const std::string& host, int windowMs) {
  std::lock_guard lock(m_mutex);
  const auto now = Clock::now();
  const auto window = std::chrono::milliseconds(windowMs);
  if (m_failures.size() > 4096) {  // bound the table: forget peers whose window has passed
    for (auto it = m_failures.begin(); it != m_failures.end();) {
      if (it->second.empty() || now - it->second.back() > window) {
        it = m_failures.erase(it);
      } else {
        ++it;
      }
    }
  }
  auto& q = m_failures[host];
  while (!q.empty() && now - q.front() > window) q.pop_front();
  q.push_back(now);
}

bool Server::authenticate(ClientEntry& entry) {
  const AuthPolicy& policy = *entry.policy;
  IConnection& conn = *entry.connection;
  const std::string host = conn.peerHost();
  const std::string peer = conn.peerAddress();

  auto reject = [&](const std::string& reason, std::optional<int64_t> id, bool count) {
    if (count) m_limiter.record(host, policy.failureWindowMs);
    TACHYON_LOG_WARN("control: authentication failed from {}: {}", peer, reason);
    // Written directly: nothing else is queued for an unauthenticated client.
    (void)conn.writeLine(encodeResponse(
        makeErrorResponse(id, makeError(ErrorCode::NoPrivilege, "authentication failed: " + reason))));
    return false;
  };

  if (m_limiter.blocked(host, policy.maxFailures, policy.failureWindowMs)) {
    // Take the client's auth line (it sends it at once) before answering: closing a socket that still holds
    // unread data resets the connection on some systems (Windows), and the answer would be lost.
    std::string ignored;
    (void)conn.readLine(ignored, std::min(policy.handshakeTimeoutMs, 1000));
    return reject("too many failed attempts from this address; try again later", std::nullopt, false);
  }

  std::string line;
  auto got = conn.readLine(line, policy.handshakeTimeoutMs);
  if (!got.ok()) {
    if (got.error().code == ErrorCode::Timeout)
      return reject("no auth line within " + std::to_string(policy.handshakeTimeoutMs) + " ms", std::nullopt,
                    true);
    return reject("bad first line", std::nullopt, true);
  }
  if (!*got) {
    // Closed without a word (a port probe, or the client gave up): counts, but nothing to answer.
    m_limiter.record(host, policy.failureWindowMs);
    TACHYON_LOG_WARN("control: authentication failed from {}: connection closed before the auth line", peer);
    return false;
  }
  std::optional<int64_t> id;
  auto req = decodeRequest(line, &id);
  if (!req.ok()) return reject("the first line must be an auth request", id, true);
  const auto* auth = std::get_if<AuthRequest>(&req->body);
  if (auth == nullptr)
    return reject("authentication required: send {\"cmd\":\"auth\",\"token\":...} first", id, true);
  if (policy.token.empty() || !constantTimeEquals(auth->token, policy.token))
    return reject("wrong token", id, true);

  if (clientCount() >= policy.maxClients) return reject("too many clients are connected", id, false);

  entry.authed.store(true);
  if (!enqueue(entry, encodeResponse(makeOkResponse(id)))) return false;
  TACHYON_LOG_INFO("control: client {} authenticated from {}", entry.id, peer);
  return true;
}

void Server::clientLoop(ClientEntry* entry) {
  IConnection& conn = *entry->connection;
  bool proceed = true;
  if (entry->remote) {
    proceed = authenticate(*entry);
    m_pending.fetch_sub(1);
    if (proceed) admit(*entry);
  }
  std::string line;
  while (proceed) {
    auto r = conn.readLine(line, -1);
    if (!r.ok()) {
      if (r.error().code == ErrorCode::InvalidArgument) {
        TACHYON_LOG_WARN("control: client {} sent an over-long line; disconnecting", entry->id);
        (void)conn.writeLine(encodeResponse(
            makeErrorResponse(std::nullopt, makeError(ErrorCode::Internal, r.error().message))));
      }
      break;
    }
    if (!*r) break;
    if (line.empty()) continue;

    std::optional<int64_t> id;
    auto req = decodeRequest(line, &id);
    Response resp;
    if (!req.ok()) {
      resp = makeErrorResponse(id, req.error());
    } else if (std::holds_alternative<AuthRequest>(req->body)) {
      resp = makeOkResponse(id);  // already admitted (local socket, or the handshake is done)
    } else {
      resp = m_handler.handle(entry->id, *req);
    }
    resp.id = id;
    // Through the same queue as events, so an event emitted while handling the request precedes its response.
    if (!enqueue(*entry, encodeResponse(resp))) break;
    if (req.ok()) m_handler.afterResponse(entry->id, *req, resp);
  }
  // Give a rejection written just before this point time to leave before the connection goes away.
  conn.close();
  closeOutbound(*entry);
  if (entry->announced) m_handler.onDisconnect(entry->id);
  entry->finished.store(true);
}

void Server::broadcast(const Event& event) {
  const std::string line = encodeEvent(event);
  // The client list lock is held while queueing: enqueue() only appends to a deque, it never touches the
  // transport (except to close an overflowing client), so this cannot block on a slow reader.
  std::lock_guard lock(m_mutex);
  for (auto& c : m_clients)
    if (!c->finished.load() && c->authed.load()) enqueue(*c, line);
}

Server::SendStatus Server::trySendLine(ClientId client, std::string line, size_t maxQueuedBytes) {
  // Same locking as broadcast(): the client list lock is held while queueing, which only appends to a deque.
  std::lock_guard lock(m_mutex);
  for (auto& c : m_clients) {
    if (c->id != client) continue;
    if (c->finished.load() || !c->authed.load()) return SendStatus::Gone;
    std::lock_guard out(c->outMutex);
    if (c->outClosed) return SendStatus::Gone;
    if (c->outBytes > 0 && c->outBytes + line.size() > maxQueuedBytes) return SendStatus::Full;
    c->outBytes += line.size();
    c->outQueue.push_back(std::move(line));
    c->outCv.notify_one();
    return SendStatus::Sent;
  }
  return SendStatus::Gone;
}

size_t Server::clientCount() const {
  std::lock_guard lock(m_mutex);
  size_t n = 0;
  for (auto& c : m_clients) n += (c->finished.load() || !c->authed.load()) ? 0 : 1;
  return n;
}

}  // namespace tachyon::control
