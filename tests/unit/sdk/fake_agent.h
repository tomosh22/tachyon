// Test-only fake agent for the SDK client runtime: a real control-protocol server (tachyon::control::Server)
// that answers inst_hello / flush, plus a raw byte listener on the instrumentation pipe/socket it returns,
// which records (or forwards) everything the SDK streams.
#pragma once

#include "control/protocol.h"
#include "control/server.h"
#include "control/transport.h"
#include "core/platform.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace tachyon::sdk_testing {

inline unsigned long currentPidValue() {
#if TACHYON_PLATFORM_WINDOWS
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

// Unique control address for this process/test.
inline std::string uniqueAgentAddress(const std::string& tag) {
  static std::atomic<int> counter{0};
  const std::string suffix = std::to_string(currentPidValue()) + "-" + tag + "-" + std::to_string(counter++);
#if TACHYON_PLATFORM_WINDOWS
  return R"(\\.\pipe\tachyon-sdktest-)" + suffix;
#else
  return "/tmp/tachyon-sdktest-" + suffix + ".sock";
#endif
}

// Receives the raw instrumentation byte stream. Chunks are delivered to `onBytes` (if set) or accumulated.
class RawStreamListener {
 public:
  using Sink = std::function<void(const uint8_t*, size_t)>;

  RawStreamListener(const std::string& address, Sink sink) : m_address(address), m_sink(std::move(sink)) {
#if TACHYON_PLATFORM_WINDOWS
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_pending = createInstance(true);
    m_ok = m_pending != INVALID_HANDLE_VALUE;
#else
    m_listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, address.c_str(), address.size() + 1);
    ::unlink(address.c_str());
    m_ok = m_listenFd >= 0 && ::bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
           ::listen(m_listenFd, 4) == 0;
    int wake[2] = {-1, -1};
    if (::pipe(wake) == 0) {
      m_wakeRead = wake[0];
      m_wakeWrite = wake[1];
    }
#endif
    if (m_ok) m_acceptThread = std::thread([this] { acceptLoop(); });
  }

  ~RawStreamListener() {
    m_stop = true;
#if TACHYON_PLATFORM_WINDOWS
    SetEvent(m_stopEvent);
#else
    const char b = 1;
    if (m_wakeWrite >= 0 && ::write(m_wakeWrite, &b, 1) < 0) {
    }
#endif
    if (m_acceptThread.joinable()) m_acceptThread.join();
    for (auto& t : m_readers) t.join();
#if TACHYON_PLATFORM_WINDOWS
    if (m_pending != INVALID_HANDLE_VALUE) CloseHandle(m_pending);
    CloseHandle(m_stopEvent);
#else
    if (m_listenFd >= 0) ::close(m_listenFd);
    if (m_wakeRead >= 0) ::close(m_wakeRead);
    if (m_wakeWrite >= 0) ::close(m_wakeWrite);
    ::unlink(m_address.c_str());
#endif
  }
  RawStreamListener(const RawStreamListener&) = delete;
  RawStreamListener& operator=(const RawStreamListener&) = delete;

  bool ok() const { return m_ok; }
  int connectionCount() const { return m_connections.load(); }
  int closedCount() const { return m_closed.load(); }
  // Closes every accepted connection from the server side (simulates the agent going away).
  void dropConnections() { m_dropGeneration++; }

 private:
#if TACHYON_PLATFORM_WINDOWS
  HANDLE createInstance(bool first) {
    DWORD openMode = PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED;
    if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    return CreateNamedPipeA(m_address.c_str(), openMode,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
  }

  void acceptLoop() {
    while (!m_stop) {
      HANDLE instance = m_pending;
      OVERLAPPED ov{};
      ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      bool connected = false;
      if (ConnectNamedPipe(instance, &ov)) {
        connected = true;
      } else {
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
          connected = true;
        } else if (err == ERROR_IO_PENDING) {
          const HANDLE handles[2] = {ov.hEvent, m_stopEvent};
          if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
            DWORD ignored = 0;
            connected = GetOverlappedResult(instance, &ov, &ignored, TRUE) != 0;
          } else {
            CancelIoEx(instance, &ov);
            DWORD ignored = 0;
            GetOverlappedResult(instance, &ov, &ignored, TRUE);
          }
        }
      }
      CloseHandle(ov.hEvent);
      if (!connected) return;
      m_pending = createInstance(false);
      ++m_connections;
      m_readers.emplace_back([this, instance] { readLoop(instance); });
      if (m_pending == INVALID_HANDLE_VALUE) return;
    }
  }

  void readLoop(HANDLE pipe) {
    HANDLE readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<uint8_t> buffer(1 << 16);
    const unsigned dropAt = m_dropGeneration.load();
    for (;;) {
      if (m_dropGeneration.load() != dropAt) break;
      OVERLAPPED ov{};
      ov.hEvent = readEvent;
      ResetEvent(readEvent);
      DWORD got = 0;
      if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) break;
        const HANDLE handles[2] = {readEvent, m_stopEvent};
        // Wake up periodically to notice dropConnections().
        const DWORD wr = WaitForMultipleObjects(2, handles, FALSE, 50);
        if (wr != WAIT_OBJECT_0) {
          CancelIoEx(pipe, &ov);
          GetOverlappedResult(pipe, &ov, &got, TRUE);
          if (wr == WAIT_OBJECT_0 + 1) break;
          if (got > 0) m_sink(buffer.data(), got);
          continue;
        }
      }
      if (!GetOverlappedResult(pipe, &ov, &got, TRUE) || got == 0) break;
      m_sink(buffer.data(), got);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    CloseHandle(readEvent);
    ++m_closed;
  }
#else
  void acceptLoop() {
    while (!m_stop) {
      pollfd pfds[2] = {{m_listenFd, POLLIN, 0}, {m_wakeRead, POLLIN, 0}};
      if (::poll(pfds, 2, -1) < 0) continue;
      if (m_stop || (pfds[1].revents & POLLIN) != 0) return;
      if ((pfds[0].revents & POLLIN) == 0) continue;
      const int fd = ::accept(m_listenFd, nullptr, nullptr);
      if (fd < 0) continue;
      ++m_connections;
      m_readers.emplace_back([this, fd] { readLoop(fd); });
    }
  }

  void readLoop(int fd) {
    std::vector<uint8_t> buffer(1 << 16);
    const unsigned dropAt = m_dropGeneration.load();
    while (!m_stop && m_dropGeneration.load() == dropAt) {
      pollfd pfd{fd, POLLIN, 0};
      const int pr = ::poll(&pfd, 1, 20);
      if (pr <= 0) continue;
      const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
      if (n <= 0) break;
      m_sink(buffer.data(), static_cast<size_t>(n));
    }
    ::close(fd);
    ++m_closed;
  }
#endif

  std::string m_address;
  Sink m_sink;
  bool m_ok = false;
  std::atomic<bool> m_stop{false};
  std::atomic<int> m_connections{0};
  std::atomic<int> m_closed{0};
  std::atomic<unsigned> m_dropGeneration{0};
  std::thread m_acceptThread;
  std::vector<std::thread> m_readers;  // only touched by the accept thread until it is joined
#if TACHYON_PLATFORM_WINDOWS
  HANDLE m_pending = INVALID_HANDLE_VALUE;
  HANDLE m_stopEvent = nullptr;
#else
  int m_listenFd = -1;
  int m_wakeRead = -1;
  int m_wakeWrite = -1;
#endif
};

class FakeAgent final : public control::IRequestHandler {
 public:
  // `sink` receives the instrumentation byte stream (called from reader threads, one at a time per
  // connection); when null the bytes are accumulated and available through bytes(). `address` is the control
  // address (see uniqueAgentAddress).
  explicit FakeAgent(const std::string& address, RawStreamListener::Sink sink = nullptr)
      : m_address(address) {
    m_instAddress = control::instrumentationAddress(m_address, static_cast<uint32_t>(currentPidValue()));
    RawStreamListener::Sink s = sink ? std::move(sink) : [this](const uint8_t* p, size_t n) {
      std::lock_guard<std::mutex> lock(m_bytesMutex);
      m_bytes.insert(m_bytes.end(), p, p + n);
    };
    m_inst = std::make_unique<RawStreamListener>(m_instAddress, std::move(s));
    auto listener = control::listenOn(m_address);
    if (listener.ok()) {
      m_server = std::make_unique<control::Server>(std::move(*listener), *this);
      m_server->start();
    }
  }
  ~FakeAgent() override {
    m_server.reset();
    m_inst.reset();
  }

  bool ok() const { return m_inst->ok() && m_server != nullptr; }
  const std::string& address() const { return m_address; }
  const std::string& instAddress() const { return m_instAddress; }

  std::vector<uint8_t> bytes() const {
    std::lock_guard<std::mutex> lock(m_bytesMutex);
    return m_bytes;
  }
  size_t byteCount() const {
    std::lock_guard<std::mutex> lock(m_bytesMutex);
    return m_bytes.size();
  }
  bool waitForBytes(size_t n, int timeoutMs) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (byteCount() < n) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
  }
  int instConnections() const { return m_inst->connectionCount(); }
  int instClosed() const { return m_inst->closedCount(); }
  bool waitForInstClosed(int count, int timeoutMs) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (m_inst->closedCount() < count) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
  }
  void dropInstConnections() { m_inst->dropConnections(); }

  // Recorded requests.
  uint32_t helloPid() const { return m_helloPid.load(); }
  uint32_t helloApiVersion() const { return m_helloApi.load(); }
  int helloCount() const { return m_helloCount.load(); }
  int flushCount() const { return m_flushCount.load(); }
  std::string lastFlushName() const {
    std::lock_guard<std::mutex> lock(m_bytesMutex);
    return m_flushName;
  }
  uint32_t lastFlushSeconds() const { return m_flushSeconds.load(); }
  void setFlushOk(bool ok) { m_flushOk = ok; }
  // Sends an unsolicited event to every connected control client.
  void broadcastEvent() { m_server->broadcast(control::StateEvent{capture::CaptureState::Capturing, 0.5}); }

  control::Response handle(control::ClientId, const control::Request& request) override {
    if (const auto* hello = std::get_if<control::InstHelloRequest>(&request.body)) {
      m_helloPid = hello->pid;
      m_helloApi = hello->api_version;
      ++m_helloCount;
      control::InstHelloBody body;
      body.pipe = m_instAddress;
      return control::makeOkResponse(std::nullopt, body);
    }
    if (const auto* flush = std::get_if<control::FlushRequest>(&request.body)) {
      {
        std::lock_guard<std::mutex> lock(m_bytesMutex);
        m_flushName = flush->name;
      }
      m_flushSeconds = flush->seconds;
      ++m_flushCount;
      if (m_flushOk.load()) {
        control::CaptureSummary summary;
        summary.path = "flight.tachyon";
        return control::makeOkResponse(std::nullopt, summary);
      }
      return control::makeErrorResponse(std::nullopt, makeError(ErrorCode::NotFound, "not capturing"));
    }
    return control::makeErrorResponse(std::nullopt, makeError(ErrorCode::Internal, "unsupported"));
  }

 private:
  std::string m_address;
  std::string m_instAddress;
  mutable std::mutex m_bytesMutex;
  std::vector<uint8_t> m_bytes;
  std::string m_flushName;
  std::atomic<uint32_t> m_helloPid{0}, m_helloApi{0}, m_flushSeconds{0};
  std::atomic<int> m_helloCount{0}, m_flushCount{0};
  std::atomic<bool> m_flushOk{true};
  std::unique_ptr<RawStreamListener> m_inst;
  std::unique_ptr<control::Server> m_server;
};

}  // namespace tachyon::sdk_testing
