#include "control/server.h"

#include "core/log.h"

namespace tachyon::control {

Server::Server(std::unique_ptr<IListener> listener, IRequestHandler& handler)
    : m_listener(std::move(listener)), m_handler(handler) {}

Server::~Server() {
  stop();
}

void Server::start() {
  m_acceptThread = std::thread([this] { acceptLoop(); });
}

void Server::stop() {
  std::vector<std::unique_ptr<ClientEntry>> clients;
  {
    std::lock_guard lock(m_mutex);
    if (m_stopped) return;
    m_stopped = true;
  }
  m_listener->close();
  if (m_acceptThread.joinable()) m_acceptThread.join();
  {
    std::lock_guard lock(m_mutex);
    clients = std::move(m_clients);
    m_clients.clear();
  }
  for (auto& c : clients) c->connection->close();
  for (auto& c : clients)
    if (c->thread.joinable()) c->thread.join();
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
  for (auto& c : done)
    if (c->thread.joinable()) c->thread.join();
}

void Server::acceptLoop() {
  for (;;) {
    auto conn = m_listener->accept();
    if (!conn.ok()) {
      if (conn.error().code != ErrorCode::Cancelled)
        TACHYON_LOG_ERROR("control: accept failed: {}", conn.error().toString());
      return;
    }
    reapFinished();
    auto entry = std::make_unique<ClientEntry>();
    entry->connection = std::shared_ptr<IConnection>(std::move(*conn));
    ClientEntry* raw = entry.get();
    {
      std::lock_guard lock(m_mutex);
      if (m_stopped) return;
      entry->id = m_nextId++;
      m_clients.push_back(std::move(entry));
    }
    m_handler.onConnect(raw->id);
    raw->thread = std::thread([this, raw] { clientLoop(raw); });
  }
}

void Server::clientLoop(ClientEntry* entry) {
  IConnection& conn = *entry->connection;
  std::string line;
  for (;;) {
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
    Response resp = req.ok() ? m_handler.handle(entry->id, *req) : makeErrorResponse(id, req.error());
    resp.id = id;
    if (!conn.writeLine(encodeResponse(resp)).ok()) break;
  }
  conn.close();
  m_handler.onDisconnect(entry->id);
  entry->finished.store(true);
}

void Server::broadcast(const Event& event) {
  const std::string line = encodeEvent(event);
  std::vector<std::shared_ptr<IConnection>> targets;
  {
    std::lock_guard lock(m_mutex);
    for (auto& c : m_clients)
      if (!c->finished.load()) targets.push_back(c->connection);
  }
  for (auto& t : targets)
    if (!t->writeLine(line).ok()) t->close();
}

size_t Server::clientCount() const {
  std::lock_guard lock(m_mutex);
  size_t n = 0;
  for (auto& c : m_clients) n += c->finished.load() ? 0 : 1;
  return n;
}

}  // namespace tachyon::control
