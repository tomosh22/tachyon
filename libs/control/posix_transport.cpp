// Unix stream socket transport (Linux).
#include "core/platform.h"

#if !TACHYON_PLATFORM_WINDOWS

#include "control/framed_connection.h"
#include "control/transport.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace tachyon::control {

namespace {

Error sysError(ErrorCode code, const std::string& what) {
  return makeError(code, what + ": " + std::strerror(errno));
}

Result<sockaddr_un> makeAddr(const std::string& path) {
  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof(addr.sun_path))
    return makeError(ErrorCode::InvalidArgument, "socket path empty or too long: " + path);
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  return addr;
}

class PosixConnection final : public detail::FramedConnection {
 public:
  explicit PosixConnection(int fd) : m_fd(fd) {}
  ~PosixConnection() override { ::close(m_fd); }

  void close() override {
    if (!m_closed.exchange(true)) ::shutdown(m_fd, SHUT_RDWR);  // fd itself is closed in the destructor
  }

 protected:
  Result<size_t> readSome(char* buffer, size_t n, int timeoutMs) override {
    for (;;) {
      if (m_closed.load()) return size_t{0};
      pollfd pfd{m_fd, POLLIN, 0};
      const int pr = ::poll(&pfd, 1, timeoutMs);
      if (pr < 0) {
        if (errno == EINTR) continue;
        return sysError(ErrorCode::IoError, "poll");
      }
      if (pr == 0) return makeError(ErrorCode::Timeout, "read timed out");
      const ssize_t r = ::recv(m_fd, buffer, n, 0);
      if (r < 0) {
        if (errno == EINTR) continue;
        if (errno == ECONNRESET) return size_t{0};
        return sysError(ErrorCode::IoError, "recv");
      }
      return static_cast<size_t>(r);
    }
  }

  Result<void> writeAll(const char* data, size_t n) override {
    while (n > 0) {
      if (m_closed.load()) return makeError(ErrorCode::IoError, "connection closed");
      const ssize_t w = ::send(m_fd, data, n, MSG_NOSIGNAL);
      if (w < 0) {
        if (errno == EINTR) continue;
        return sysError(ErrorCode::IoError, "send");
      }
      data += w;
      n -= static_cast<size_t>(w);
    }
    return ok();
  }

 private:
  int m_fd;
  std::atomic<bool> m_closed{false};
};

class PosixListener final : public IListener {
 public:
  PosixListener(int fd, int wakeRead, int wakeWrite, std::string path)
      : m_fd(fd), m_wakeRead(wakeRead), m_wakeWrite(wakeWrite), m_path(std::move(path)) {}
  ~PosixListener() override {
    close();
    ::close(m_fd);
    ::close(m_wakeRead);
    ::close(m_wakeWrite);
    ::unlink(m_path.c_str());
  }

  Result<std::unique_ptr<IConnection>> accept() override {
    for (;;) {
      if (m_closed.load()) return makeError(ErrorCode::Cancelled, "listener closed");
      pollfd pfds[2] = {{m_fd, POLLIN, 0}, {m_wakeRead, POLLIN, 0}};
      if (::poll(pfds, 2, -1) < 0) {
        if (errno == EINTR) continue;
        return sysError(ErrorCode::IoError, "poll");
      }
      if (m_closed.load() || (pfds[1].revents & POLLIN) != 0)
        return makeError(ErrorCode::Cancelled, "listener closed");
      if ((pfds[0].revents & POLLIN) == 0) continue;
      const int cfd = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
      if (cfd < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
        return sysError(ErrorCode::IoError, "accept");
      }
      return std::unique_ptr<IConnection>(new PosixConnection(cfd));
    }
  }

  void close() override {
    if (!m_closed.exchange(true)) {
      const char b = 1;
      if (::write(m_wakeWrite, &b, 1) < 0) {
        // The wake pipe is only a hint; m_closed is re-checked by accept().
      }
    }
  }

  std::string address() const override { return m_path; }

 private:
  int m_fd, m_wakeRead, m_wakeWrite;
  std::string m_path;
  std::atomic<bool> m_closed{false};
};

}  // namespace

std::string defaultAgentAddress() {
  if (const char* env = std::getenv("TACHYON_AGENT_PIPE"); env != nullptr && *env != '\0') return env;
  const std::string runDir = "/run/user/" + std::to_string(::getuid());
  std::error_code ec;
  if (std::filesystem::is_directory(runDir, ec)) return runDir + "/tachyon-agent.sock";
  return "/tmp/tachyon-agent.sock";
}

std::string instrumentationAddress(const std::string& agentAddress, uint32_t pid) {
  const auto dir = std::filesystem::path(agentAddress).parent_path();
  return (dir / ("tachyon-inst-" + std::to_string(pid) + ".sock")).string();
}

Result<std::unique_ptr<IListener>> listenOn(const std::string& address) {
  auto addr = makeAddr(address);
  if (!addr.ok()) return std::move(addr).error();

  // A live agent on the path means Busy; a stale socket file is replaced.
  if (auto existing = connectTo(address); existing.ok())
    return makeError(ErrorCode::Busy, "another agent is already listening on " + address);

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return sysError(ErrorCode::IoError, "socket");
  ::unlink(address.c_str());
  const mode_t oldMask = ::umask(0177);
  const int br = ::bind(fd, reinterpret_cast<const sockaddr*>(&*addr), sizeof(sockaddr_un));
  ::umask(oldMask);
  if (br < 0) {
    Error e = sysError(ErrorCode::IoError, "bind " + address);
    ::close(fd);
    return e;
  }
  if (::listen(fd, 16) < 0) {
    Error e = sysError(ErrorCode::IoError, "listen");
    ::close(fd);
    ::unlink(address.c_str());
    return e;
  }
  int wake[2];
  if (::pipe2(wake, O_CLOEXEC) < 0) {
    Error e = sysError(ErrorCode::IoError, "pipe2");
    ::close(fd);
    ::unlink(address.c_str());
    return e;
  }
  return std::unique_ptr<IListener>(new PosixListener(fd, wake[0], wake[1], address));
}

Result<std::unique_ptr<IConnection>> connectTo(const std::string& address) {
  auto addr = makeAddr(address);
  if (!addr.ok()) return std::move(addr).error();
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return sysError(ErrorCode::IoError, "socket");
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&*addr), sizeof(sockaddr_un)) < 0) {
    Error e = sysError(errno == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError, "connect " + address);
    ::close(fd);
    return e;
  }
  return std::unique_ptr<IConnection>(new PosixConnection(fd));
}

}  // namespace tachyon::control

#endif  // !TACHYON_PLATFORM_WINDOWS
