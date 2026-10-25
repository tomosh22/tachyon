// Listener for the per-process instrumentation pipe (Windows named pipe) / socket (Linux Unix socket).
// Unlike tachyon::control's line-framed transport this one hands over raw bytes: the SDK only ever writes,
// the agent only ever reads.
#pragma once

#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tachyon::inst {

// Receives the bytes of one connection. Both callbacks run on that connection's reader thread.
class IStreamSink {
 public:
  virtual ~IStreamSink() = default;
  virtual void onData(const uint8_t* data, size_t size) = 0;
  // Called exactly once, after the last onData, when the connection ended or was closed by shutdown().
  virtual void onClose() = 0;
};

// Called on the accept thread for every new connection.
using SinkFactory = std::function<std::unique_ptr<IStreamSink>()>;

struct RawServerOptions {
  // Connections open at the same time; further ones are closed at once and counted in rejectedCount().
  uint32_t maxConnections = 16;
  // When non-zero, a connection whose peer process id is known (Linux SO_PEERCRED, Windows
  // GetNamedPipeClientProcessId) and differs from this one is closed and counted as rejected.
  uint32_t expectedPeerPid = 0;
};

class RawStreamServer {
 public:
  // Thread-safe. Starts listening on `address` (a stale socket file is replaced). Fails with Busy if a live
  // listener already owns the address. On Windows the pipe DACL matches the control pipe.
  static Result<std::unique_ptr<RawStreamServer>> listen(const std::string& address, SinkFactory factory,
                                                         RawServerOptions options = {});

  // Thread-safe: calls shutdown(0, 0).
  ~RawStreamServer();
  RawStreamServer(const RawStreamServer&) = delete;
  RawStreamServer& operator=(const RawStreamServer&) = delete;

  // Thread-safe, idempotent. Stops accepting (clients that already connected are still accepted), keeps
  // reading every open connection until it has been silent for `drainIdleMs` or has been open `drainMaxMs`
  // since this call, then closes all connections, runs their onClose and joins every thread.
  void shutdown(int drainIdleMs, int drainMaxMs);

  // Thread-safe.
  std::string address() const;
  // Thread-safe. Connections accepted so far.
  uint32_t acceptedCount() const;
  // Thread-safe. Connections currently open.
  uint32_t openCount() const;
  // Thread-safe. Connections refused: over maxConnections, wrong peer pid, or no thread could be created.
  uint32_t rejectedCount() const;

 private:
  struct Impl;
  explicit RawStreamServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> m_impl;
};

}  // namespace tachyon::inst
