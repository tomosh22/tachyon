// Line-framed byte-stream transports for the agent control protocol: Unix stream sockets on Linux, named
// pipes on Windows. Both sit behind IConnection / IListener.
#pragma once

#include "core/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace tachyon::control {

// Longest accepted line (excluding the newline). Longer lines are a protocol error: readLine fails with
// ErrorCode::InvalidArgument and the connection should be dropped.
constexpr size_t kMaxLineBytes = 1024 * 1024;

class IConnection {
 public:
  virtual ~IConnection() = default;

  // Thread-safe with respect to writeLine/close, but only one thread may read at a time. Reads one
  // '\n'-terminated line (terminator and an optional preceding '\r' are stripped). Returns true on a line,
  // false on orderly EOF/close. timeoutMs < 0 waits forever; on timeout fails with ErrorCode::Timeout and
  // keeps any partial data buffered.
  virtual Result<bool> readLine(std::string& line, int timeoutMs = -1) = 0;

  // Thread-safe (serialised internally). Appends the '\n' terminator.
  virtual Result<void> writeLine(std::string_view line) = 0;

  // Thread-safe, idempotent. Unblocks any thread blocked in readLine/writeLine; later operations fail/EOF.
  virtual void close() = 0;
};

class IListener {
 public:
  virtual ~IListener() = default;

  // Thread-safe. Blocks until a client connects; fails with ErrorCode::Cancelled once close() was called.
  virtual Result<std::unique_ptr<IConnection>> accept() = 0;

  // Thread-safe, idempotent. Unblocks accept().
  virtual void close() = 0;

  // Thread-safe. The address this listener is bound to (socket path / pipe name).
  virtual std::string address() const = 0;
};

// Thread-safe: reads the environment. Agent address: $TACHYON_AGENT_PIPE if set, else
// \\.\pipe\tachyon-agent (Windows) or /run/user/<uid>/tachyon-agent.sock, falling back to
// /tmp/tachyon-agent.sock when /run/user/<uid> does not exist (Linux).
std::string defaultAgentAddress();

// Thread-safe: pure function. Address of the per-process instrumentation pipe/socket for `pid`, in the same
// namespace as agentAddress (Windows: \\.\pipe\tachyon-inst-<pid>; Linux: next to the agent socket).
std::string instrumentationAddress(const std::string& agentAddress, uint32_t pid);

// Thread-safe. Starts listening. Fails with Busy if another live agent already owns the address.
Result<std::unique_ptr<IListener>> listenOn(const std::string& address);

// Thread-safe. Connects to a listening agent.
Result<std::unique_ptr<IConnection>> connectTo(const std::string& address);

}  // namespace tachyon::control
