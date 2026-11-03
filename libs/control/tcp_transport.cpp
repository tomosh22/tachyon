// TCP transport (WP-61). One implementation for BSD sockets and Winsock; the few differences sit in the
// `sock` helpers below. Winsock code is built and exercised by the Windows CI job only.
#include "control/tcp.h"

#include "control/framed_connection.h"
#include "core/platform.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace tachyon::control {

namespace {

using Clock = std::chrono::steady_clock;

#if TACHYON_PLATFORM_WINDOWS

using SockT = SOCKET;
constexpr SockT kBadSock = INVALID_SOCKET;

namespace sock {
int lastError() {
  return WSAGetLastError();
}
bool wouldBlock(int e) {
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
bool interrupted(int e) {
  return e == WSAEINTR;
}
bool addrInUse(int e) {
  return e == WSAEADDRINUSE || e == WSAEACCES;
}
bool reset(int e) {
  return e == WSAECONNRESET || e == WSAECONNABORTED || e == WSAESHUTDOWN || e == WSAENOTCONN;
}
std::string describe(int e) {
  char* text = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(e), 0, reinterpret_cast<LPSTR>(&text), 0, nullptr);
  std::string out = "error " + std::to_string(e);
  if (text != nullptr) {
    out += ": ";
    out += text;
    LocalFree(text);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
  }
  return out;
}
void closeSock(SockT s) {
  ::closesocket(s);
}
void shutdownBoth(SockT s) {
  ::shutdown(s, SD_BOTH);
}
int pollOne(SockT s, short events, int timeoutMs, short& revents) {
  WSAPOLLFD pfd{};
  pfd.fd = s;
  pfd.events = events;
  const int r = ::WSAPoll(&pfd, 1, timeoutMs);
  revents = pfd.revents;
  return r;
}
bool setNonBlocking(SockT s, bool on) {
  u_long mode = on ? 1 : 0;
  return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}
// Winsock needs WSAStartup before the first socket call; done once per process.
void ensureInit() {
  static const bool started = [] {
    WSADATA data;
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
}
}  // namespace sock

constexpr short kPollIn = POLLRDNORM;
constexpr short kPollOut = POLLWRNORM;

#else

using SockT = int;
constexpr SockT kBadSock = -1;

namespace sock {
int lastError() {
  return errno;
}
bool wouldBlock(int e) {
  return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS || e == EALREADY;
}
bool interrupted(int e) {
  return e == EINTR;
}
bool addrInUse(int e) {
  return e == EADDRINUSE || e == EACCES;
}
bool reset(int e) {
  return e == ECONNRESET || e == EPIPE || e == ENOTCONN;
}
std::string describe(int e) {
  return std::strerror(e);
}
void closeSock(SockT s) {
  ::close(s);
}
void shutdownBoth(SockT s) {
  ::shutdown(s, SHUT_RDWR);
}
int pollOne(SockT s, short events, int timeoutMs, short& revents) {
  pollfd pfd{s, events, 0};
  const int r = ::poll(&pfd, 1, timeoutMs);
  revents = pfd.revents;
  return r;
}
bool setNonBlocking(SockT s, bool on) {
  const int flags = ::fcntl(s, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
}
void ensureInit() {}
}  // namespace sock

constexpr short kPollIn = POLLIN;
constexpr short kPollOut = POLLOUT;

#endif

Error sockError(ErrorCode code, const std::string& what, int err = sock::lastError()) {
  return makeError(code, what + ": " + sock::describe(err));
}

// Longest wait inside one poll: a blocked reader notices close() within this long on every platform.
constexpr int kPollSliceMs = 100;

std::string numericHost(const sockaddr* addr, socklen_t len, uint16_t* portOut) {
  char host[NI_MAXHOST] = {0};
  char service[NI_MAXSERV] = {0};
  if (::getnameinfo(addr, len, host, sizeof(host), service, sizeof(service),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    return {};
  if (portOut != nullptr) *portOut = static_cast<uint16_t>(std::strtoul(service, nullptr, 10));
  std::string h = host;
  // A link-local scope id ("fe80::1%eth0") stays part of the literal.
  return h;
}

std::string formatPeer(const sockaddr* addr, socklen_t len, std::string* hostOut) {
  uint16_t port = 0;
  const std::string host = numericHost(addr, len, &port);
  if (hostOut != nullptr) *hostOut = host;
  if (host.empty()) return {};
  return formatTcpEndpoint(TcpEndpoint{host, port});
}

void tuneConnection(SockT s) {
  const int on = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
  ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&on), sizeof(on));
}

class TcpConnection final : public detail::FramedConnection {
 public:
  TcpConnection(SockT fd, std::string peer, std::string peerHost, bool remote)
      : m_fd(fd), m_peer(std::move(peer)), m_peerHost(std::move(peerHost)), m_remote(remote) {}
  ~TcpConnection() override { sock::closeSock(m_fd); }

  void close() override {
    if (!m_closed.exchange(true)) sock::shutdownBoth(m_fd);  // the descriptor is closed in the destructor
  }
  std::string peerAddress() const override { return m_peer; }
  std::string peerHost() const override { return m_peerHost; }
  bool isRemote() const override { return m_remote; }

 protected:
  Result<size_t> readSome(char* buffer, size_t n, int timeoutMs) override {
    const bool hasDeadline = timeoutMs >= 0;
    const auto deadline = Clock::now() + std::chrono::milliseconds(hasDeadline ? timeoutMs : 0);
    for (;;) {
      if (m_closed.load()) return size_t{0};
      int slice = kPollSliceMs;
      if (hasDeadline) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) return makeError(ErrorCode::Timeout, "read timed out");
        if (left < slice) slice = static_cast<int>(left);
      }
      short revents = 0;
      const int pr = sock::pollOne(m_fd, kPollIn, slice, revents);
      if (pr < 0) {
        if (sock::interrupted(sock::lastError())) continue;
        return sockError(ErrorCode::IoError, "poll");
      }
      if (pr == 0) continue;
#if TACHYON_PLATFORM_WINDOWS
      const int r = ::recv(m_fd, buffer, static_cast<int>(n), 0);
#else
      const ssize_t r = ::recv(m_fd, buffer, n, 0);
#endif
      if (r < 0) {
        const int e = sock::lastError();
        if (sock::interrupted(e) || sock::wouldBlock(e)) continue;
        if (sock::reset(e)) return size_t{0};
        return sockError(ErrorCode::IoError, "recv", e);
      }
      return static_cast<size_t>(r);
    }
  }

  Result<void> writeAll(const char* data, size_t n) override {
    while (n > 0) {
      if (m_closed.load()) return makeError(ErrorCode::IoError, "connection closed");
      // Wait for room with a bounded slice so a peer that stopped reading cannot pin this thread forever
      // once close() is called.
      short revents = 0;
      const int pr = sock::pollOne(m_fd, kPollOut, kPollSliceMs, revents);
      if (pr < 0) {
        if (sock::interrupted(sock::lastError())) continue;
        return sockError(ErrorCode::IoError, "poll");
      }
      if (pr == 0) continue;
#if TACHYON_PLATFORM_WINDOWS
      const int w = ::send(m_fd, data, static_cast<int>(n > (1u << 20) ? (1u << 20) : n), 0);
#else
      const ssize_t w = ::send(m_fd, data, n, MSG_NOSIGNAL);
#endif
      if (w < 0) {
        const int e = sock::lastError();
        if (sock::interrupted(e) || sock::wouldBlock(e)) continue;
        return sockError(ErrorCode::IoError, "send", e);
      }
      data += w;
      n -= static_cast<size_t>(w);
    }
    return ok();
  }

 private:
  SockT m_fd;
  std::string m_peer;
  std::string m_peerHost;
  bool m_remote;
  std::atomic<bool> m_closed{false};
};

class AddrInfo {
 public:
  AddrInfo() = default;
  ~AddrInfo() {
    if (m_list != nullptr) ::freeaddrinfo(m_list);
  }
  AddrInfo(const AddrInfo&) = delete;
  AddrInfo& operator=(const AddrInfo&) = delete;

  Result<void> resolve(const TcpEndpoint& endpoint, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = (passive ? AI_PASSIVE : 0) | AI_NUMERICSERV;
    const std::string port = std::to_string(endpoint.port);
    const int rc = ::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &m_list);
    if (rc != 0) {
      m_list = nullptr;
#if TACHYON_PLATFORM_WINDOWS
      return makeError(ErrorCode::NotFound,
                       "cannot resolve " + endpoint.host + " (error " + std::to_string(rc) + ")");
#else
      return makeError(ErrorCode::NotFound, "cannot resolve " + endpoint.host + ": " + ::gai_strerror(rc));
#endif
    }
    return ok();
  }
  const addrinfo* first() const { return m_list; }

 private:
  addrinfo* m_list = nullptr;
};

class TcpListener final : public IListener {
 public:
  TcpListener(SockT fd, std::string address) : m_fd(fd), m_address(std::move(address)) {}
  ~TcpListener() override {
    close();
    sock::closeSock(m_fd);
  }

  Result<std::unique_ptr<IConnection>> accept() override {
    for (;;) {
      if (m_closed.load()) return makeError(ErrorCode::Cancelled, "listener closed");
      short revents = 0;
      const int pr = sock::pollOne(m_fd, kPollIn, kPollSliceMs, revents);
      if (pr < 0) {
        if (sock::interrupted(sock::lastError())) continue;
        return sockError(ErrorCode::IoError, "poll");
      }
      if (pr == 0 || (revents & kPollIn) == 0) continue;
      sockaddr_storage peer{};
      socklen_t peerLen = sizeof(peer);
      const SockT cfd = ::accept(m_fd, reinterpret_cast<sockaddr*>(&peer), &peerLen);
      if (cfd == kBadSock) {
        const int e = sock::lastError();
        if (sock::interrupted(e) || sock::wouldBlock(e)) continue;
#if !TACHYON_PLATFORM_WINDOWS
        if (e == ECONNABORTED) continue;
        if (e == EMFILE || e == ENFILE || e == ENOMEM || e == ENOBUFS) {
          // Out of descriptors: the client stays queued; back off instead of spinning.
          short ignored = 0;
          (void)sock::pollOne(m_fd, 0, 50, ignored);
          continue;
        }
#endif
        return sockError(ErrorCode::IoError, "accept", e);
      }
#if !TACHYON_PLATFORM_WINDOWS
      ::fcntl(cfd, F_SETFD, FD_CLOEXEC);
#endif
      sock::setNonBlocking(cfd, false);
      tuneConnection(cfd);
      std::string host;
      const std::string text = formatPeer(reinterpret_cast<const sockaddr*>(&peer), peerLen, &host);
      return std::unique_ptr<IConnection>(new TcpConnection(cfd, text, host, /*remote=*/true));
    }
  }

  void close() override { m_closed.store(true); }
  std::string address() const override { return m_address; }

 private:
  SockT m_fd;
  std::string m_address;
  std::atomic<bool> m_closed{false};
};

}  // namespace

bool isTcpAddress(std::string_view address) {
  return address.size() > 6 && address.substr(0, 6) == "tcp://";
}

Result<TcpEndpoint> parseTcpEndpoint(std::string_view text) {
  if (isTcpAddress(text)) text.remove_prefix(6);
  if (!text.empty() && text.back() == '/') text.remove_suffix(1);
  TcpEndpoint out;
  std::string_view port;
  if (!text.empty() && text.front() == '[') {
    const size_t bracket = text.find(']');
    if (bracket == std::string_view::npos || bracket + 1 >= text.size() || text[bracket + 1] != ':')
      return makeError(ErrorCode::InvalidArgument, "expected [ipv6-address]:port");
    out.host = std::string(text.substr(1, bracket - 1));
    port = text.substr(bracket + 2);
  } else {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos)
      return makeError(ErrorCode::InvalidArgument, "expected host:port (got '" + std::string(text) + "')");
    if (text.find(':') != colon)
      return makeError(ErrorCode::InvalidArgument,
                       "an IPv6 address needs brackets: [address]:port (got '" + std::string(text) + "')");
    out.host = std::string(text.substr(0, colon));
    port = text.substr(colon + 1);
  }
  if (out.host.empty())
    return makeError(ErrorCode::InvalidArgument,
                     "the host is missing; name it explicitly (127.0.0.1, 0.0.0.0, a host name)");
  if (port.empty() || port.size() > 5)
    return makeError(ErrorCode::InvalidArgument, "bad port '" + std::string(port) + "'");
  uint32_t value = 0;
  for (const char c : port) {
    if (c < '0' || c > '9')
      return makeError(ErrorCode::InvalidArgument, "bad port '" + std::string(port) + "'");
    value = value * 10 + static_cast<uint32_t>(c - '0');
  }
  if (value > 65535) return makeError(ErrorCode::InvalidArgument, "port out of range: " + std::string(port));
  out.port = static_cast<uint16_t>(value);
  return out;
}

std::string formatTcpEndpoint(const TcpEndpoint& endpoint) {
  if (endpoint.host.find(':') != std::string::npos)
    return "[" + endpoint.host + "]:" + std::to_string(endpoint.port);
  return endpoint.host + ":" + std::to_string(endpoint.port);
}

bool isLoopbackHost(std::string_view host) {
  if (host == "::1" || host == "localhost") return true;
  return host.size() >= 4 && host.substr(0, 4) == "127.";
}

Result<std::unique_ptr<IListener>> listenTcp(const TcpEndpoint& endpoint) {
  sock::ensureInit();
  AddrInfo resolved;
  TACHYON_TRY(resolved.resolve(endpoint, /*passive=*/true));
  const addrinfo* ai = resolved.first();
  if (ai == nullptr) return makeError(ErrorCode::NotFound, "no address for " + endpoint.host);

  const SockT fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd == kBadSock) return sockError(ErrorCode::IoError, "socket");
#if !TACHYON_PLATFORM_WINDOWS
  ::fcntl(fd, F_SETFD, FD_CLOEXEC);
  const int reuse = 1;  // restart quickly; on Windows SO_REUSEADDR would allow port hijacking, so not there
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
  const int exclusive = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
#endif
  if (::bind(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen)) != 0) {
    const int e = sock::lastError();
    sock::closeSock(fd);
    return sockError(sock::addrInUse(e) ? ErrorCode::Busy : ErrorCode::IoError,
                     "bind " + formatTcpEndpoint(endpoint), e);
  }
  if (::listen(fd, 32) != 0) {
    const int e = sock::lastError();
    sock::closeSock(fd);
    return sockError(ErrorCode::IoError, "listen", e);
  }
  sock::setNonBlocking(fd, false);
  sockaddr_storage bound{};
  socklen_t boundLen = sizeof(bound);
  std::string address = formatTcpEndpoint(endpoint);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
    const std::string text = formatPeer(reinterpret_cast<const sockaddr*>(&bound), boundLen, nullptr);
    if (!text.empty()) address = text;
  }
  return std::unique_ptr<IListener>(new TcpListener(fd, std::move(address)));
}

Result<std::unique_ptr<IConnection>> connectTcp(const TcpEndpoint& endpoint, int timeoutMs) {
  sock::ensureInit();
  AddrInfo resolved;
  TACHYON_TRY(resolved.resolve(endpoint, /*passive=*/false));
  const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  Error last = makeError(ErrorCode::IoError, "no address to connect to for " + endpoint.host);
  for (const addrinfo* ai = resolved.first(); ai != nullptr; ai = ai->ai_next) {
    const SockT fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd == kBadSock) {
      last = sockError(ErrorCode::IoError, "socket");
      continue;
    }
#if !TACHYON_PLATFORM_WINDOWS
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    sock::setNonBlocking(fd, true);
    int rc = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    int err = rc == 0 ? 0 : sock::lastError();
    if (rc != 0 && sock::wouldBlock(err)) {
      const auto left =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
      short revents = 0;
      const int pr = sock::pollOne(fd, kPollOut, left > 0 ? static_cast<int>(left) : 0, revents);
      if (pr == 0) {
        sock::closeSock(fd);
        last = makeError(ErrorCode::Timeout, "connect to " + formatTcpEndpoint(endpoint) + " timed out");
        continue;
      }
      int soError = 0;
      socklen_t soLen = sizeof(soError);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen);
      err = pr < 0 ? sock::lastError() : soError;
      rc = err == 0 ? 0 : -1;
    }
    if (rc != 0) {
      sock::closeSock(fd);
      last = sockError(ErrorCode::IoError, "connect " + formatTcpEndpoint(endpoint), err);
      continue;
    }
    sock::setNonBlocking(fd, false);
    tuneConnection(fd);
    std::string host;
    const std::string peer = formatPeer(ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen), &host);
    return std::unique_ptr<IConnection>(new TcpConnection(fd, peer, host, /*remote=*/false));
  }
  return last;
}

}  // namespace tachyon::control
