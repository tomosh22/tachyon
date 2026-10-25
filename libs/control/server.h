// Control protocol server: accepts clients, decodes requests, dispatches to a handler, broadcasts events.
#pragma once

#include "control/protocol.h"
#include "control/transport.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tachyon::control {

using ClientId = uint64_t;

class IRequestHandler {
 public:
  virtual ~IRequestHandler() = default;
  // Called from the accept thread / client thread respectively; must be thread-safe.
  virtual void onConnect(ClientId) {}
  virtual void onDisconnect(ClientId) {}
  // Thread-safe requirement: called concurrently from one thread per connected client. The returned
  // response (its id is overwritten with the request id) is written back to that client.
  virtual Response handle(ClientId client, const Request& request) = 0;
};

// One accept thread plus one thread per client. Handler must outlive the server.
class Server {
 public:
  // Thread-safe.
  Server(std::unique_ptr<IListener> listener, IRequestHandler& handler);
  // Thread-safe: calls stop().
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Not thread-safe with stop(). Starts the accept thread.
  void start();
  // Thread-safe, idempotent. Closes the listener and all clients and joins every thread. Must not be called
  // from a handler callback.
  void stop();

  // Thread-safe. Sends the event line to every connected client; clients that fail to receive are dropped.
  void broadcast(const Event& event);

  // Thread-safe.
  size_t clientCount() const;
  // Thread-safe.
  std::string address() const { return m_listener->address(); }

 private:
  struct ClientEntry {
    ClientId id = 0;
    std::shared_ptr<IConnection> connection;
    std::thread thread;
    std::atomic<bool> finished{false};
  };

  void acceptLoop();
  void clientLoop(ClientEntry* entry);
  void reapFinished();

  std::unique_ptr<IListener> m_listener;
  IRequestHandler& m_handler;
  std::thread m_acceptThread;
  mutable std::mutex m_mutex;
  std::vector<std::unique_ptr<ClientEntry>> m_clients;
  ClientId m_nextId = 1;
  bool m_stopped = false;
};

}  // namespace tachyon::control
