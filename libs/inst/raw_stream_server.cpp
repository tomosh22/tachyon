#include "inst/raw_stream_server.h"

#include "core/platform.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// sddl.h needs windows.h first.
#include <sddl.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <system_error>
#endif

namespace tachyon::inst {

namespace {

constexpr int kReadPollMs = 20;
constexpr size_t kReadBufferBytes = 64 * 1024;

int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

enum class ReadStatus { Data, Timeout, Closed };

#if TACHYON_PLATFORM_WINDOWS

using ConnHandle = HANDLE;
constexpr DWORD kPipeBufferBytes = 64 * 1024;

Error winError(ErrorCode code, const std::string& what, DWORD err = GetLastError()) {
  return makeError(code, what + " (Win32 error " + std::to_string(err) + ")");
}

std::wstring currentUserSidString() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::wstring result;
  if (size > 0) {
    std::vector<char> buf(size);
    if (GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
      LPWSTR text = nullptr;
      if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text)) {
        result = text;
        LocalFree(text);
      }
    }
  }
  CloseHandle(token);
  return result;
}

// Same DACL as the control pipe (libs/control/windows_transport.cpp): no network logons, SYSTEM,
// Administrators and the agent's own account have full control, other authenticated users get the
// read/write rights without FILE_CREATE_PIPE_INSTANCE.
class PipeSecurity {
 public:
  PipeSecurity() = default;
  ~PipeSecurity() {
    if (m_descriptor != nullptr) LocalFree(m_descriptor);
  }
  PipeSecurity(const PipeSecurity&) = delete;
  PipeSecurity& operator=(const PipeSecurity&) = delete;

  bool init() {
    std::wstring sddl = L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)";
    const std::wstring user = currentUserSidString();
    if (!user.empty()) sddl += L"(A;;GA;;;" + user + L")";
    sddl += L"(A;;0x12019b;;;AU)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &m_descriptor,
                                                              nullptr))
      return false;
    m_attributes.nLength = sizeof(m_attributes);
    m_attributes.lpSecurityDescriptor = m_descriptor;
    m_attributes.bInheritHandle = FALSE;
    return true;
  }
  SECURITY_ATTRIBUTES* attributes() { return &m_attributes; }

 private:
  PSECURITY_DESCRIPTOR m_descriptor = nullptr;
  SECURITY_ATTRIBUTES m_attributes{};
};

HANDLE createInstance(const std::string& name, bool first, PipeSecurity& security) {
  DWORD openMode = PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED;
  if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  return CreateNamedPipeA(name.c_str(), openMode,
                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                          PIPE_UNLIMITED_INSTANCES, 0, kPipeBufferBytes, 0, security.attributes());
}

#else

using ConnHandle = int;

Error sysError(ErrorCode code, const std::string& what) {
  return makeError(code, what + ": " + std::strerror(errno));
}

#endif

}  // namespace

struct RawStreamServer::Impl {
  std::string address;
  SinkFactory factory;

  std::atomic<bool> stopping{false};  // no further accepts
  std::atomic<bool> draining{false};  // connections end after the idle / max time
  std::atomic<int> drainIdleMs{0};
  std::atomic<int> drainMaxMs{0};
  std::atomic<int64_t> drainStartMs{0};
  std::atomic<uint32_t> accepted{0};
  std::atomic<uint32_t> open{0};

  RawServerOptions options;
  std::atomic<uint32_t> rejected{0};

  // A connection's reader thread; `done` is set when the thread is about to end, so it can be joined.
  struct Reader {
    std::thread thread;
    std::atomic<bool> done{false};
  };
  std::mutex mutex;  // guards readers and shutDown
  std::vector<std::unique_ptr<Reader>> readers;
  bool shutDown = false;
  std::thread acceptThread;

#if TACHYON_PLATFORM_WINDOWS
  std::unique_ptr<PipeSecurity> security;
  HANDLE pending = INVALID_HANDLE_VALUE;  // accept thread / shutdown only
  HANDLE stopEvent = nullptr;
#else
  int listenFd = -1;
  int wakeRead = -1;
  int wakeWrite = -1;
#endif

  // ---- platform pieces ----
  ReadStatus readSome(ConnHandle conn, uint8_t* buffer, size_t capacity, size_t& got);
  static void closeConn(ConnHandle conn);
  void acceptLoop();
  void wakeAccept();
  void releaseListener();

  // ---- common ----
  void addConnection(ConnHandle conn);
  uint32_t peerPid(ConnHandle conn) const;  // 0 when unknown
  void reapFinishedLocked();
  void readLoop(ConnHandle conn);
};

#if TACHYON_PLATFORM_WINDOWS

ReadStatus RawStreamServer::Impl::readSome(ConnHandle conn, uint8_t* buffer, size_t capacity, size_t& got) {
  got = 0;
  HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (event == nullptr) return ReadStatus::Closed;
  OVERLAPPED ov{};
  ov.hEvent = event;
  DWORD n = 0;
  ReadStatus status = ReadStatus::Data;
  const DWORD want = static_cast<DWORD>(capacity);
  if (!ReadFile(conn, buffer, want, nullptr, &ov)) {
    const DWORD err = GetLastError();
    if (err != ERROR_IO_PENDING) {
      CloseHandle(event);
      return ReadStatus::Closed;  // ERROR_BROKEN_PIPE and friends: the client went away
    }
    if (WaitForSingleObject(event, kReadPollMs) != WAIT_OBJECT_0) {
      CancelIoEx(conn, &ov);
      GetOverlappedResult(conn, &ov, &n, TRUE);
      CloseHandle(event);
      got = n;  // bytes that completed before the cancel took effect
      return n > 0 ? ReadStatus::Data : ReadStatus::Timeout;
    }
  }
  if (!GetOverlappedResult(conn, &ov, &n, TRUE) || n == 0) status = ReadStatus::Closed;
  CloseHandle(event);
  got = n;
  return status;
}

void RawStreamServer::Impl::closeConn(ConnHandle conn) {
  DisconnectNamedPipe(conn);
  CloseHandle(conn);
}

uint32_t RawStreamServer::Impl::peerPid(ConnHandle conn) const {
  ULONG pid = 0;
  return GetNamedPipeClientProcessId(conn, &pid) ? static_cast<uint32_t>(pid) : 0;
}

void RawStreamServer::Impl::wakeAccept() {
  SetEvent(stopEvent);
}

void RawStreamServer::Impl::acceptLoop() {
  // Runs until a wait for the next client is cancelled by shutdown(); a client that is already connected when
  // shutdown() is requested is still taken first (ConnectNamedPipe then completes at once).
  for (;;) {
    HANDLE instance = pending;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (ConnectNamedPipe(instance, &ov)) {
      connected = true;
    } else {
      const DWORD err = GetLastError();
      // ERROR_NO_DATA: the client connected, wrote and closed before we got here; its data is still in the
      // pipe.
      if (err == ERROR_PIPE_CONNECTED || err == ERROR_NO_DATA) {
        connected = true;
      } else if (err == ERROR_IO_PENDING) {
        const HANDLE handles[2] = {ov.hEvent, stopEvent};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
          DWORD ignored = 0;
          connected =
              GetOverlappedResult(instance, &ov, &ignored, TRUE) != 0 || GetLastError() == ERROR_NO_DATA;
        } else {
          CancelIoEx(instance, &ov);
          DWORD ignored = 0;
          GetOverlappedResult(instance, &ov, &ignored, TRUE);
        }
      }
    }
    CloseHandle(ov.hEvent);
    if (!connected) return;
    // Keep an unconnected instance available so later clients never see "file not found".
    pending = createInstance(address, false, *security);
    addConnection(instance);
    // Out of resources: keep trying to make a listening instance, unless we are asked to stop.
    while (pending == INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(stopEvent, 50) == WAIT_OBJECT_0) return;
      pending = createInstance(address, false, *security);
    }
  }
}

void RawStreamServer::Impl::releaseListener() {
  if (pending != INVALID_HANDLE_VALUE) CloseHandle(pending);
  pending = INVALID_HANDLE_VALUE;
  if (stopEvent != nullptr) CloseHandle(stopEvent);
  stopEvent = nullptr;
}

#else

ReadStatus RawStreamServer::Impl::readSome(ConnHandle conn, uint8_t* buffer, size_t capacity, size_t& got) {
  got = 0;
  for (;;) {
    pollfd pfd{conn, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, kReadPollMs);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return ReadStatus::Closed;
    }
    if (pr == 0) return ReadStatus::Timeout;
    const ssize_t r = ::recv(conn, buffer, capacity, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return ReadStatus::Closed;
    }
    if (r == 0) return ReadStatus::Closed;
    got = static_cast<size_t>(r);
    return ReadStatus::Data;
  }
}

void RawStreamServer::Impl::closeConn(ConnHandle conn) {
  ::close(conn);
}

uint32_t RawStreamServer::Impl::peerPid(ConnHandle conn) const {
  ucred cred{};
  socklen_t len = sizeof(cred);
  if (::getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.pid <= 0) return 0;
  return static_cast<uint32_t>(cred.pid);
}

void RawStreamServer::Impl::wakeAccept() {
  const char b = 1;
  if (::write(wakeWrite, &b, 1) < 0) {
    // The wake pipe is only a hint; the accept loop also re-checks `stopping` after every poll.
  }
}

void RawStreamServer::Impl::acceptLoop() {
  for (;;) {
    const bool stop = stopping.load();
    // After stop was requested still take clients whose connect() already succeeded (they sit in the
    // backlog).
    pollfd pfds[2] = {{listenFd, POLLIN, 0}, {wakeRead, POLLIN, 0}};
    const int pr = ::poll(pfds, 2, stop ? 0 : -1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
      if (fd >= 0) {
        addConnection(fd);
        continue;
      }
      if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
      if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
        // Out of descriptors or memory: the pending client stays queued. Back off (waking up for shutdown)
        // instead of spinning or giving up.
        pollfd wake{wakeRead, POLLIN, 0};
        (void)::poll(&wake, 1, 50);
        if (stopping.load()) return;
        continue;
      }
      return;
    }
    if (stop || (pfds[1].revents & POLLIN) != 0) return;
  }
}

void RawStreamServer::Impl::releaseListener() {
  if (listenFd >= 0) ::close(listenFd);
  if (wakeRead >= 0) ::close(wakeRead);
  if (wakeWrite >= 0) ::close(wakeWrite);
  listenFd = wakeRead = wakeWrite = -1;
  std::error_code ec;
  std::filesystem::remove(address, ec);
}

#endif

void RawStreamServer::Impl::reapFinishedLocked() {
  for (size_t i = 0; i < readers.size();) {
    if (readers[i]->done.load(std::memory_order_acquire)) {
      readers[i]->thread.join();
      readers.erase(readers.begin() + static_cast<std::ptrdiff_t>(i));
    } else {
      ++i;
    }
  }
}

void RawStreamServer::Impl::addConnection(ConnHandle conn) {
  ++accepted;
  if (options.expectedPeerPid != 0) {
    const uint32_t peer = peerPid(conn);
    if (peer != 0 && peer != options.expectedPeerPid) {
      ++rejected;
      closeConn(conn);
      return;
    }
  }
  std::lock_guard lock(mutex);
  reapFinishedLocked();
  if (open.load() >= options.maxConnections) {
    ++rejected;
    closeConn(conn);
    return;
  }
  ++open;
  try {
    auto reader = std::make_unique<Reader>();
    Reader* raw = reader.get();
    reader->thread = std::thread([this, conn, raw] {
      readLoop(conn);
      raw->done.store(true, std::memory_order_release);
    });
    readers.push_back(std::move(reader));
  } catch (const std::exception&) {  // std::system_error (no thread) or bad_alloc
    --open;
    ++rejected;
    closeConn(conn);
  }
}

void RawStreamServer::Impl::readLoop(ConnHandle conn) {
  std::unique_ptr<IStreamSink> sink = factory();
  std::vector<uint8_t> buffer(kReadBufferBytes);
  int64_t lastData = nowMs();
  for (;;) {
    size_t got = 0;
    const ReadStatus status = readSome(conn, buffer.data(), buffer.size(), got);
    if (status == ReadStatus::Closed) break;
    const int64_t now = nowMs();
    if (status == ReadStatus::Data) {
      if (sink) sink->onData(buffer.data(), got);
      lastData = now;
    }
    if (draining.load(std::memory_order_acquire)) {
      if (now - drainStartMs.load() >= drainMaxMs.load()) break;
      if (status == ReadStatus::Timeout && now - lastData >= drainIdleMs.load()) break;
    }
  }
  closeConn(conn);
  if (sink) sink->onClose();
  --open;
}

RawStreamServer::RawStreamServer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Result<std::unique_ptr<RawStreamServer>> RawStreamServer::listen(const std::string& address,
                                                                 SinkFactory factory,
                                                                 RawServerOptions options) {
  auto impl = std::make_unique<Impl>();
  impl->address = address;
  impl->factory = std::move(factory);
  impl->options = options;
#if TACHYON_PLATFORM_WINDOWS
  impl->security = std::make_unique<PipeSecurity>();
  if (!impl->security->init()) return winError(ErrorCode::IoError, "building the pipe security descriptor");
  impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  impl->pending = createInstance(address, true, *impl->security);
  if (impl->pending == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    impl->releaseListener();
    if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY)
      return winError(ErrorCode::Busy, "pipe already in use: " + address, err);
    return winError(ErrorCode::IoError, "CreateNamedPipe " + address, err);
  }
#else
  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (address.empty() || address.size() >= sizeof(addr.sun_path))
    return makeError(ErrorCode::InvalidArgument, "socket path empty or too long: " + address);
  std::memcpy(addr.sun_path, address.c_str(), address.size() + 1);

  // A live listener on the path means Busy; a stale socket file is replaced.
  {
    const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe >= 0) {
      const bool live = ::connect(probe, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
      ::close(probe);
      if (live) return makeError(ErrorCode::Busy, "another listener is already on " + address);
    }
  }
  impl->listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (impl->listenFd < 0) return sysError(ErrorCode::IoError, "socket");
  // Race-free permissions without touching the process-wide umask: bind inside a private (0700) directory
  // made next to the final path, restrict the socket to its owner, then rename it into place. Nobody else can
  // reach the socket through the directory while it is still being set up.
  namespace stdfs = std::filesystem;
  stdfs::path parent = stdfs::path(address).parent_path();
  if (parent.empty()) parent = ".";
  std::string dirTemplate = (parent / ".tachyon-sock-XXXXXX").string();
  if (::mkdtemp(dirTemplate.data()) == nullptr) {
    Error e = sysError(ErrorCode::IoError, "mkdtemp in " + parent.string());
    ::close(impl->listenFd);
    impl->listenFd = -1;
    return e;
  }
  const std::string tempSocket = dirTemplate + "/s";
  auto removeTemp = [&] {
    std::error_code ignored;
    stdfs::remove(tempSocket, ignored);
    stdfs::remove(dirTemplate, ignored);
  };
  sockaddr_un tempAddr;
  std::memset(&tempAddr, 0, sizeof(tempAddr));
  tempAddr.sun_family = AF_UNIX;
  if (tempSocket.size() >= sizeof(tempAddr.sun_path)) {
    removeTemp();
    ::close(impl->listenFd);
    impl->listenFd = -1;
    return makeError(ErrorCode::InvalidArgument, "socket path too long: " + address);
  }
  std::memcpy(tempAddr.sun_path, tempSocket.c_str(), tempSocket.size() + 1);
  bool placed = ::bind(impl->listenFd, reinterpret_cast<const sockaddr*>(&tempAddr), sizeof(tempAddr)) == 0 &&
                ::chmod(tempSocket.c_str(), 0600) == 0 && ::rename(tempSocket.c_str(), address.c_str()) == 0;
  if (!placed) {
    Error e = sysError(ErrorCode::IoError, "bind " + address);
    removeTemp();
    ::close(impl->listenFd);
    impl->listenFd = -1;
    return e;
  }
  removeTemp();  // the directory only
  if (::listen(impl->listenFd, 16) < 0) {
    Error e = sysError(ErrorCode::IoError, "listen");
    impl->releaseListener();
    return e;
  }
  int wake[2];
  if (::pipe2(wake, O_CLOEXEC) < 0) {
    Error e = sysError(ErrorCode::IoError, "pipe2");
    impl->releaseListener();
    return e;
  }
  impl->wakeRead = wake[0];
  impl->wakeWrite = wake[1];
#endif
  Impl* raw = impl.get();
  try {
    impl->acceptThread = std::thread([raw] { raw->acceptLoop(); });
  } catch (const std::exception& e) {
    impl->stopping = true;
    impl->releaseListener();
    return makeError(ErrorCode::OutOfMemory, std::string("cannot start the accept thread: ") + e.what());
  }
  return std::unique_ptr<RawStreamServer>(new RawStreamServer(std::move(impl)));
}

RawStreamServer::~RawStreamServer() {
  shutdown(0, 0);
}

void RawStreamServer::shutdown(int drainIdleMs, int drainMaxMs) {
  Impl& d = *m_impl;
  {
    std::lock_guard lock(d.mutex);
    if (d.shutDown) return;
    d.shutDown = true;
  }
  // Readers started draining as soon as the flag is set; the accept thread may still add a last one.
  d.drainIdleMs.store(drainIdleMs);
  d.drainMaxMs.store(drainMaxMs);
  d.drainStartMs.store(nowMs());
  d.draining.store(true, std::memory_order_release);
  d.stopping.store(true);
  d.wakeAccept();
  if (d.acceptThread.joinable()) d.acceptThread.join();
  std::vector<std::unique_ptr<Impl::Reader>> readers;
  {
    std::lock_guard lock(d.mutex);
    readers.swap(d.readers);
  }
  for (auto& r : readers) r->thread.join();
  d.releaseListener();
}

std::string RawStreamServer::address() const {
  return m_impl->address;
}

uint32_t RawStreamServer::acceptedCount() const {
  return m_impl->accepted.load();
}

uint32_t RawStreamServer::openCount() const {
  return m_impl->open.load();
}

uint32_t RawStreamServer::rejectedCount() const {
  return m_impl->rejected.load();
}

}  // namespace tachyon::inst
