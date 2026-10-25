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
    raw->writerThread = std::thread([this, raw] { writerLoop(raw); });
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
    // Through the same queue as events, so an event emitted while handling the request precedes its response.
    if (!enqueue(*entry, encodeResponse(resp))) break;
  }
  conn.close();
  closeOutbound(*entry);
  m_handler.onDisconnect(entry->id);
  entry->finished.store(true);
}

void Server::broadcast(const Event& event) {
  const std::string line = encodeEvent(event);
  // The client list lock is held while queueing: enqueue() only appends to a deque, it never touches the
  // transport (except to close an overflowing client), so this cannot block on a slow reader.
  std::lock_guard lock(m_mutex);
  for (auto& c : m_clients)
    if (!c->finished.load()) enqueue(*c, line);
}

size_t Server::clientCount() const {
  std::lock_guard lock(m_mutex);
  size_t n = 0;
  for (auto& c : m_clients) n += c->finished.load() ? 0 : 1;
  return n;
}

}  // namespace tachyon::control
