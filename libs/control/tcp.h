// TCP transport for the control protocol (WP-61): the same line framing as the local socket / pipe, over IPv4
// or IPv6. Unencrypted: authentication is the token handshake in server.h, nothing more. Winsock on Windows,
// BSD sockets elsewhere.
#pragma once

#include "control/transport.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace tachyon::control {

struct TcpEndpoint {
  std::string host;  // a host name, an IPv4 literal or an IPv6 literal (without brackets)
  uint16_t port = 0;
};

// Thread-safe: pure function. True for "tcp://..." addresses (the form clients use).
bool isTcpAddress(std::string_view address);

// Thread-safe: pure function. "host:port", "[v6-literal]:port" or "tcp://" followed by either. The host must
// be present (an agent never listens on an implicit wildcard); port 0 is accepted (the system picks one).
Result<TcpEndpoint> parseTcpEndpoint(std::string_view text);

// Thread-safe: pure function. "host:port" with IPv6 literals in brackets.
std::string formatTcpEndpoint(const TcpEndpoint& endpoint);

// Thread-safe. Listens on `endpoint` (hostnames are resolved; the first address is used).
// IListener::address() is the bound "ip:port" (so port 0 reports the chosen port). Accepted connections have
// TCP_NODELAY and keep-alive set, report isRemote() == true and their peer in peerAddress(). Fails with Busy
// when the port is taken.
Result<std::unique_ptr<IListener>> listenTcp(const TcpEndpoint& endpoint);

// Thread-safe. Connects within `timeoutMs`, trying every resolved address. TCP_NODELAY is set.
Result<std::unique_ptr<IConnection>> connectTcp(const TcpEndpoint& endpoint, int timeoutMs = 5000);

// Thread-safe: pure function. True when the literal is a loopback address (127.0.0.0/8, ::1).
bool isLoopbackHost(std::string_view host);

}  // namespace tachyon::control
