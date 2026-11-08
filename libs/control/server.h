// Control protocol server: accepts clients, decodes requests, dispatches to a handler, broadcasts events.
#pragma once

#include "control/protocol.h"
#include "control/transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::control {

using ClientId = uint64_t;

// What the server knows about a client when it is admitted (WP-61).
struct ClientInfo {
  bool remote = false;  // came in over the network (IConnection::isRemote); authenticated by token
  std::string peer;     // "ip:port" for remote clients
};

class IRequestHandler {
 public:
  virtual ~IRequestHandler() = default;
  // Called from the accept thread / client thread respectively; must be thread-safe.
  virtual void onConnect(ClientId) {}
  virtual void onDisconnect(ClientId) {}
  // WP-61. Called right before onConnect, once the client is admitted (for a remote client: after its token
  // was accepted), with what is known about it. A handler that restricts remote clients records it here.
  virtual void onClientAccepted(ClientId, const ClientInfo&) {}
  // Thread-safe requirement: called concurrently from one thread per connected client. The returned
  // response (its id is overwritten with the request id) is written back to that client.
  virtual Response handle(ClientId client, const Request& request) = 0;
  // WP-65a. Called from the client's thread right after the response to `request` was queued for the client
  // (so anything sent to the client from here on follows the response). The live subscription starts here.
  virtual void afterResponse(ClientId, const Request&, const Response&) {}
  // WP-65a. Server::start() hands the handler the server (to send to a single client); Server::stop() tells
  // it before anything is closed. After onServerStopping returns the handler must not use the server again.
  virtual void onServerStarted(class Server&) {}
  virtual void onServerStopping() {}
};

// Longest outbound backlog per client (queued responses + events, in bytes). A client that lets more than
// this pile up (it does not read) is disconnected; see Server.
constexpr size_t kMaxQueuedBytesPerClient = 2 * 1024 * 1024;

// WP-61. Authentication of the clients of a network listener.
struct AuthPolicy {
  // The shared secret every client has to present in its first line (auth.h). Must not be empty.
  std::string token;
  // A client that has not sent a valid auth line within this long is dropped.
  int handshakeTimeoutMs = 5000;
  // Failed handshakes (wrong token, no auth line, timeout, garbage) from one peer address within the window
  // that make the server refuse that address until the window has passed.
  uint32_t maxFailures = 5;
  int failureWindowMs = 60000;
  // Connections that have not authenticated yet, at most (the rest are closed at once); and authenticated
  // network clients, at most.
  uint32_t maxPending = 16;
  uint32_t maxClients = 32;
};

// One accept thread per listener plus a reader thread and a writer thread per client. Handler must outlive
// the server.
//
// All traffic to a client (responses and broadcast events, in order) goes through a per-client queue drained
// by that client's writer thread, so a client that stops reading can never block the thread that produces an
// event or another client's request: its queue grows to kMaxQueuedBytesPerClient and the client is dropped.
//
// Clients of a network listener (addRemoteListener) must authenticate first: the first line is
// {"cmd":"auth","token":"..."} within AuthPolicy::handshakeTimeoutMs. Until then they receive no events and
// the handler does not know them. A failure is answered with an ENOPRIV error response, the connection is
// closed, and the peer address is logged (never the token) and counted against the rate limit. Local clients
// (the primary listener) never authenticate.
class Server {
 public:
  // Thread-safe.
  Server(std::unique_ptr<IListener> listener, IRequestHandler& handler);
  // Thread-safe: calls stop().
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Not thread-safe, call before start(). WP-61: adds a listener whose clients must authenticate.
  void addRemoteListener(std::unique_ptr<IListener> listener, AuthPolicy policy);

  // Not thread-safe with stop(). Starts the accept threads.
  void start();
  // Thread-safe, idempotent. Closes the listeners and all clients and joins every thread. Must not be called
  // from a handler callback.
  void stop();

  // Thread-safe, never blocks on a client. Queues the event line for every connected (and authenticated)
  // client; clients whose queue overflows or whose connection fails are dropped.
  void broadcast(const Event& event);

  enum class SendStatus {
    Sent,
    Full,  // the client's backlog would exceed the limit: nothing was queued
    Gone,  // no such client (disconnected, dropped, or not authenticated)
  };
  // Thread-safe (WP-65a), never blocks on the client. Queues `line` (one encoded message, no newline) for the
  // client if its backlog after queueing stays within `maxQueuedBytes`; a client with an empty backlog always
  // accepts one line. Unlike broadcast() a full queue does not drop the client: the caller decides (live
  // events are dropped first, see docs/specs/live.md).
  SendStatus trySendLine(ClientId client, std::string line, size_t maxQueuedBytes);

  // Thread-safe. Authenticated / local clients.
  size_t clientCount() const;
  // Thread-safe. Address of the primary (local) listener.
  std::string address() const { return m_listeners.front()->listener->address(); }
  // Thread-safe. Address of the first remote listener ("ip:port"), empty if there is none. Valid after
  // addRemoteListener; port 0 reports the port the system chose.
  std::string remoteAddress() const;

 private:
  struct ListenerEntry {
    std::unique_ptr<IListener> listener;
    bool remote = false;
    AuthPolicy policy;
    std::thread acceptThread;
  };

  struct ClientEntry {
    ClientId id = 0;
    std::shared_ptr<IConnection> connection;
    std::thread thread;        // reads requests
    std::thread writerThread;  // drains outQueue
    std::atomic<bool> finished{false};
    bool remote = false;
    const AuthPolicy* policy = nullptr;  // remote clients; the listener entry outlives the client
    std::atomic<bool> authed{true};
    bool announced = false;  // the handler knows this client (onClientAccepted was called)

    std::mutex outMutex;
    std::condition_variable outCv;
    std::deque<std::string> outQueue;
    size_t outBytes = 0;
    bool outClosed = false;
  };

  // Failed-handshake bookkeeping per peer address.
  class FailureLimiter {
   public:
    // True when `host` has used up its failures in the window.
    bool blocked(const std::string& host, uint32_t maxFailures, int windowMs);
    void record(const std::string& host, int windowMs);

   private:
    using Clock = std::chrono::steady_clock;
    std::mutex m_mutex;
    std::map<std::string, std::deque<Clock::time_point>> m_failures;
  };

  // Queues a line for the client's writer; false (and the connection is closed) on overflow.
  static bool enqueue(ClientEntry& entry, std::string line);
  static void closeOutbound(ClientEntry& entry);
  void writerLoop(ClientEntry* entry);

  void acceptLoop(ListenerEntry* listener);
  void clientLoop(ClientEntry* entry);
  // Remote clients: runs the handshake. True when the client may proceed.
  bool authenticate(ClientEntry& entry);
  void admit(ClientEntry& entry);
  void reapFinished();

  std::vector<std::unique_ptr<ListenerEntry>> m_listeners;  // [0] is the local one
  IRequestHandler& m_handler;
  mutable std::mutex m_mutex;
  std::vector<std::unique_ptr<ClientEntry>> m_clients;
  ClientId m_nextId = 1;
  bool m_stopped = false;
  std::atomic<uint32_t> m_pending{0};  // remote connections still in their handshake
  FailureLimiter m_limiter;
};

}  // namespace tachyon::control
