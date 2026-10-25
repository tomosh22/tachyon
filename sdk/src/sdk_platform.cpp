// SDK-internal platform layer implementation (Linux + Windows).
#include "sdk_platform.h"

#include <stdlib.h>
#include <string.h>

#include <filesystem>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#endif

namespace tachyon::sdk {

// --------------------------------------------------------------------------------------------------------
// Process / thread / environment
// --------------------------------------------------------------------------------------------------------

uint64_t ticksFrequency() {
#if defined(_WIN32)
  static const uint64_t frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
  }();
  return frequency;
#else
  return 1000000000ull;
#endif
}

uint32_t currentThreadId() {
#if defined(_WIN32)
  return static_cast<uint32_t>(GetCurrentThreadId());
#elif defined(__linux__)
  return static_cast<uint32_t>(syscall(SYS_gettid));
#else
  return static_cast<uint32_t>(pthread_self());
#endif
}

uint32_t currentProcessId() {
#if defined(_WIN32)
  return static_cast<uint32_t>(GetCurrentProcessId());
#else
  return static_cast<uint32_t>(getpid());
#endif
}

bool getEnv(const char* name, std::string& out) {
#if defined(_WIN32)
  char stackBuffer[512];
  const DWORD n = GetEnvironmentVariableA(name, stackBuffer, sizeof(stackBuffer));
  if (n == 0) return false;
  if (n < sizeof(stackBuffer)) {
    out.assign(stackBuffer, n);
    return true;
  }
  std::string big(n, '\0');
  const DWORD m = GetEnvironmentVariableA(name, &big[0], n);
  if (m == 0 || m >= n) return false;
  big.resize(m);
  out = std::move(big);
  return true;
#else
  const char* v = getenv(name);
  if (v == nullptr) return false;
  out = v;
  return true;
#endif
}

std::string currentOsThreadName() {
#if defined(_WIN32)
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const GetThreadDescriptionFn fn = [] {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    return k != nullptr ? reinterpret_cast<GetThreadDescriptionFn>(
                              reinterpret_cast<void*>(GetProcAddress(k, "GetThreadDescription")))
                        : nullptr;
  }();
  if (fn == nullptr) return {};
  PWSTR wide = nullptr;
  if (FAILED(fn(GetCurrentThread(), &wide)) || wide == nullptr) return {};
  std::string result;
  const int need = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (need > 1) {
    result.resize(static_cast<size_t>(need));
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &result[0], need, nullptr, nullptr);
    result.resize(static_cast<size_t>(need - 1));
  }
  LocalFree(wide);
  return result;
#elif defined(__linux__)
  char name[32] = {0};
  if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) return {};
  return name;
#else
  return {};
#endif
}

std::string executableName() {
  std::string path;
#if defined(_WIN32)
  char buffer[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
  if (n > 0 && n < sizeof(buffer)) path.assign(buffer, n);
#elif defined(__linux__)
  char buffer[4096];
  const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n > 0) path.assign(buffer, static_cast<size_t>(n));
#endif
  if (path.empty()) return "app";
  const std::string stem = std::filesystem::path(path).stem().string();
  return stem.empty() ? "app" : stem;
}

std::string currentDirectory() {
  std::error_code ec;
  const std::filesystem::path p = std::filesystem::current_path(ec);
  if (ec) return ".";
  return p.string();
}

std::string defaultAgentAddress() {
  std::string env;
  if (getEnv("TACHYON_AGENT_PIPE", env) && !env.empty()) return env;
#if defined(_WIN32)
  return R"(\\.\pipe\tachyon-agent)";
#else
  const std::string runDir = "/run/user/" + std::to_string(static_cast<unsigned long>(getuid()));
  std::error_code ec;
  if (std::filesystem::is_directory(runDir, ec)) return runDir + "/tachyon-agent.sock";
  return "/tmp/tachyon-agent.sock";
#endif
}

// --------------------------------------------------------------------------------------------------------
// Channel
// --------------------------------------------------------------------------------------------------------

void Channel::swap(Channel& other) noexcept {
#if defined(_WIN32)
  std::swap(m_handle, other.m_handle);
  std::swap(m_event, other.m_event);
  std::swap(m_overlapped, other.m_overlapped);
#else
  std::swap(m_fd, other.m_fd);
  std::swap(m_socket, other.m_socket);
#endif
}

#if defined(_WIN32)

namespace {
HANDLE asHandle(void* h) {
  return h != nullptr ? static_cast<HANDLE>(h) : INVALID_HANDLE_VALUE;
}
}  // namespace

Channel::~Channel() {
  close();
}

bool Channel::isOpen() const {
  return m_handle != nullptr;
}

void Channel::close() {
  if (m_handle != nullptr) {
    CloseHandle(static_cast<HANDLE>(m_handle));
    m_handle = nullptr;
  }
  if (m_event != nullptr) {
    CloseHandle(static_cast<HANDLE>(m_event));
    m_event = nullptr;
  }
}

bool Channel::connect(const std::string& address, int timeoutMs, bool needRead) {
  close();
  const ULONGLONG start = GetTickCount64();
  HANDLE h = INVALID_HANDLE_VALUE;
  for (;;) {
    DWORD access = needRead ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_WRITE;
    h = CreateFileA(address.c_str(), access, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h != INVALID_HANDLE_VALUE) break;
    const DWORD err = GetLastError();
    if (err != ERROR_PIPE_BUSY) return false;
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (timeoutMs >= 0 && elapsed >= static_cast<ULONGLONG>(timeoutMs)) return false;
    const DWORD remaining = timeoutMs < 0 ? NMPWAIT_WAIT_FOREVER : static_cast<DWORD>(timeoutMs - elapsed);
    WaitNamedPipeA(address.c_str(), remaining == 0 ? 1 : remaining);
  }
  DWORD mode = PIPE_READMODE_BYTE;
  SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
  m_handle = h;
  m_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_overlapped = true;
  if (m_event == nullptr) {
    close();
    return false;
  }
  return true;
}

bool Channel::openFile(const std::string& path) {
  close();
  HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  m_handle = h;
  m_overlapped = false;
  return true;
}

bool Channel::writeAll(const void* data, size_t size, int timeoutMs) {
  if (m_handle == nullptr) return false;
  const HANDLE h = asHandle(m_handle);
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    const DWORD want = static_cast<DWORD>(size > (1u << 20) ? (1u << 20) : size);
    DWORD wrote = 0;
    if (!m_overlapped) {
      if (!WriteFile(h, p, want, &wrote, nullptr)) return false;
    } else {
      OVERLAPPED ov{};
      ov.hEvent = static_cast<HANDLE>(m_event);
      ResetEvent(ov.hEvent);
      if (!WriteFile(h, p, want, nullptr, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        const DWORD wr =
            WaitForSingleObject(ov.hEvent, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
        if (wr != WAIT_OBJECT_0) {
          CancelIoEx(h, &ov);
          GetOverlappedResult(h, &ov, &wrote, TRUE);
          return false;
        }
      }
      if (!GetOverlappedResult(h, &ov, &wrote, TRUE)) return false;
    }
    if (wrote == 0) return false;
    p += wrote;
    size -= wrote;
  }
  return true;
}

int Channel::readSome(void* buffer, size_t size, int timeoutMs) {
  if (m_handle == nullptr || !m_overlapped) return -2;
  const HANDLE h = asHandle(m_handle);
  OVERLAPPED ov{};
  ov.hEvent = static_cast<HANDLE>(m_event);
  ResetEvent(ov.hEvent);
  DWORD got = 0;
  const DWORD want = static_cast<DWORD>(size);
  if (!ReadFile(h, buffer, want, nullptr, &ov)) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) return 0;
    if (err != ERROR_IO_PENDING) return -2;
    const DWORD wr = WaitForSingleObject(ov.hEvent, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
    if (wr != WAIT_OBJECT_0) {
      CancelIoEx(h, &ov);
      GetOverlappedResult(h, &ov, &got, TRUE);
      if (got > 0) return static_cast<int>(got);
      return -1;
    }
  }
  if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
    const DWORD err = GetLastError();
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) return 0;
    return -2;
  }
  return got == 0 ? 0 : static_cast<int>(got);
}

#else  // POSIX

Channel::~Channel() {
  close();
}

bool Channel::isOpen() const {
  return m_fd >= 0;
}

void Channel::close() {
  if (m_fd >= 0) {
    ::close(m_fd);
    m_fd = -1;
  }
}

bool Channel::connect(const std::string& address, int timeoutMs, bool needRead) {
  (void)timeoutMs;  // Unix socket connects complete (or fail) immediately; a full backlog fails with EAGAIN.
  (void)needRead;
  close();
  sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (address.empty() || address.size() >= sizeof(addr.sun_path)) return false;
  memcpy(addr.sun_path, address.c_str(), address.size() + 1);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return false;
  int rc;
  do {
    rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  } while (rc < 0 && errno == EINTR);
  if (rc < 0) {
    ::close(fd);
    return false;
  }
  m_fd = fd;
  m_socket = true;
  return true;
}

bool Channel::openFile(const std::string& path) {
  close();
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  m_fd = fd;
  m_socket = false;
  return true;
}

bool Channel::writeAll(const void* data, size_t size, int timeoutMs) {
  if (m_fd < 0) return false;
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    ssize_t w;
    if (m_socket) {
      w = ::send(m_fd, p, size, MSG_NOSIGNAL);
    } else {
      w = ::write(m_fd, p, size);
    }
    if (w < 0) {
      if (errno == EINTR) continue;
      if (m_socket && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd{m_fd, POLLOUT, 0};
        const int pr = ::poll(&pfd, 1, timeoutMs);
        if (pr < 0 && errno != EINTR) return false;
        if (pr == 0) return false;  // no progress within the timeout
        continue;
      }
      return false;
    }
    if (w == 0) return false;
    p += w;
    size -= static_cast<size_t>(w);
  }
  return true;
}

int Channel::readSome(void* buffer, size_t size, int timeoutMs) {
  if (m_fd < 0 || !m_socket) return -2;
  for (;;) {
    pollfd pfd{m_fd, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr < 0) {
      if (errno == EINTR) continue;
      return -2;
    }
    if (pr == 0) return -1;
    const ssize_t r = ::recv(m_fd, buffer, size, 0);
    if (r < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (errno == ECONNRESET) return 0;
      return -2;
    }
    return static_cast<int>(r);
  }
}

#endif

}  // namespace tachyon::sdk
