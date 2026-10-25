// Named pipe transport (Windows). Overlapped I/O so that close() can interrupt blocked operations.
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#include "control/framed_connection.h"
#include "control/transport.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// sddl.h needs windows.h first.
#include <sddl.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <vector>

namespace tachyon::control {

namespace {

constexpr DWORD kPipeBufferBytes = 64 * 1024;

Error winError(ErrorCode code, const std::string& what, DWORD err = GetLastError()) {
  return makeError(code, what + " (Win32 error " + std::to_string(err) + ")");
}

class PipeConnection final : public detail::FramedConnection {
 public:
  explicit PipeConnection(HANDLE pipe)
      : m_pipe(pipe),
        m_readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        m_writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
        m_closeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~PipeConnection() override {
    CloseHandle(m_pipe);
    CloseHandle(m_readEvent);
    CloseHandle(m_writeEvent);
    CloseHandle(m_closeEvent);
  }

  void close() override {
    if (!m_closed.exchange(true)) SetEvent(m_closeEvent);
  }

 protected:
  Result<size_t> readSome(char* buffer, size_t n, int timeoutMs) override {
    if (m_closed.load()) return size_t{0};
    OVERLAPPED ov{};
    ov.hEvent = m_readEvent;
    ResetEvent(m_readEvent);
    const DWORD want = static_cast<DWORD>(n);
    DWORD got = 0;
    if (!ReadFile(m_pipe, buffer, want, nullptr, &ov)) {
      const DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) return size_t{0};
      if (err != ERROR_IO_PENDING) return winError(ErrorCode::IoError, "ReadFile", err);
      const HANDLE handles[2] = {m_readEvent, m_closeEvent};
      const DWORD wr =
          WaitForMultipleObjects(2, handles, FALSE, timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
      if (wr != WAIT_OBJECT_0) {
        CancelIoEx(m_pipe, &ov);
        GetOverlappedResult(m_pipe, &ov, &got, TRUE);
        if (got > 0) return static_cast<size_t>(got);  // completed before the cancel took effect
        if (wr == WAIT_TIMEOUT) return makeError(ErrorCode::Timeout, "read timed out");
        return size_t{0};  // closed
      }
    }
    if (!GetOverlappedResult(m_pipe, &ov, &got, TRUE)) {
      const DWORD err = GetLastError();
      if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_OPERATION_ABORTED)
        return size_t{0};
      return winError(ErrorCode::IoError, "GetOverlappedResult", err);
    }
    return static_cast<size_t>(got);
  }

  Result<void> writeAll(const char* data, size_t n) override {
    while (n > 0) {
      if (m_closed.load()) return makeError(ErrorCode::IoError, "connection closed");
      OVERLAPPED ov{};
      ov.hEvent = m_writeEvent;
      ResetEvent(m_writeEvent);
      const DWORD want = static_cast<DWORD>(n > kPipeBufferBytes ? kPipeBufferBytes : n);
      DWORD wrote = 0;
      if (!WriteFile(m_pipe, data, want, nullptr, &ov)) {
        const DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) return winError(ErrorCode::IoError, "WriteFile", err);
        const HANDLE handles[2] = {m_writeEvent, m_closeEvent};
        if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
          CancelIoEx(m_pipe, &ov);
          GetOverlappedResult(m_pipe, &ov, &wrote, TRUE);
          return makeError(ErrorCode::IoError, "connection closed");
        }
      }
      if (!GetOverlappedResult(m_pipe, &ov, &wrote, TRUE)) return winError(ErrorCode::IoError, "write");
      data += wrote;
      n -= wrote;
    }
    return ok();
  }

 private:
  HANDLE m_pipe;
  HANDLE m_readEvent, m_writeEvent, m_closeEvent;
  std::atomic<bool> m_closed{false};
};

// SDDL for the agent pipe. The agent is elevated, the client (CLI / viewer) is usually the interactive user's
// non-elevated process, so the default DACL (full control for creator, SYSTEM, Administrators; read for
// Everyone and Anonymous) is replaced by:
//   D:P                  protected: nothing is inherited from a parent object
//   (D;;GA;;;NU)         deny every network logon (belt and braces next to PIPE_REJECT_REMOTE_CLIENTS)
//   (A;;GA;;;SY)         SYSTEM
//   (A;;GA;;;BA)         Administrators
//   (A;;GA;;;<user>)     the account that runs the agent (needed so a non-elevated agent can create the next
//                        pipe instance)
//   (A;;0x12019b;;;AU)   Authenticated Users: FILE_GENERIC_READ | FILE_GENERIC_WRITE without
//                        FILE_APPEND_DATA (= FILE_CREATE_PIPE_INSTANCE), so a client can talk to the agent
//                        but cannot create a competing instance of the pipe. Anonymous and guests are not
//                        Authenticated Users. Clients must therefore open the pipe with kPipeClientAccess,
//                        not GENERIC_READ | GENERIC_WRITE.
constexpr DWORD kPipeClientAccess = FILE_GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES;

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

std::wstring pipeSddl() {
  std::wstring sddl = L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)";
  const std::wstring user = currentUserSidString();
  if (!user.empty()) sddl += L"(A;;GA;;;" + user + L")";
  sddl += L"(A;;0x12019b;;;AU)";
  return sddl;
}

// The security descriptor is built once per listener: a new pipe instance is created after every accept and
// clients get ERROR_PIPE_BUSY while none is pending, so that window must stay as short as possible.
class PipeSecurity {
 public:
  PipeSecurity() = default;
  ~PipeSecurity() {
    if (m_descriptor != nullptr) LocalFree(m_descriptor);
  }
  PipeSecurity(const PipeSecurity&) = delete;
  PipeSecurity& operator=(const PipeSecurity&) = delete;

  bool init() {
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(pipeSddl().c_str(), SDDL_REVISION_1,
                                                              &m_descriptor, nullptr))
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
  DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
  if (first) openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
  return CreateNamedPipeA(
      name.c_str(), openMode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 0, security.attributes());
}

class PipeListener final : public IListener {
 public:
  PipeListener(std::string name, HANDLE firstInstance, std::unique_ptr<PipeSecurity> security)
      : m_name(std::move(name)),
        m_security(std::move(security)),
        m_pending(firstInstance),
        m_closeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~PipeListener() override {
    if (m_pending != INVALID_HANDLE_VALUE) CloseHandle(m_pending);
    CloseHandle(m_closeEvent);
  }

  Result<std::unique_ptr<IConnection>> accept() override {
    if (m_closed.load()) return makeError(ErrorCode::Cancelled, "listener closed");
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
        const HANDLE handles[2] = {ov.hEvent, m_closeEvent};
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
    if (!connected) return makeError(ErrorCode::Cancelled, "listener closed");

    // Keep an unconnected instance available at all times so clients never see "file not found".
    m_pending = createInstance(m_name, false, *m_security);
    if (m_pending == INVALID_HANDLE_VALUE) {
      const Error e = winError(ErrorCode::IoError, "CreateNamedPipe");
      CloseHandle(instance);
      return e;
    }
    return std::unique_ptr<IConnection>(new PipeConnection(instance));
  }

  void close() override {
    if (!m_closed.exchange(true)) SetEvent(m_closeEvent);
  }

  std::string address() const override { return m_name; }

 private:
  std::string m_name;
  std::unique_ptr<PipeSecurity> m_security;
  HANDLE m_pending;  // only touched by the accept() thread and the destructor
  HANDLE m_closeEvent;
  std::atomic<bool> m_closed{false};
};

}  // namespace

std::string defaultAgentAddress() {
  char* env = nullptr;
  size_t len = 0;
  std::string result = R"(\\.\pipe\tachyon-agent)";
  if (_dupenv_s(&env, &len, "TACHYON_AGENT_PIPE") == 0 && env != nullptr) {
    if (*env != '\0') result = env;
    free(env);
  }
  return result;
}

std::string instrumentationAddress(const std::string& agentAddress, uint32_t pid) {
  (void)agentAddress;
  return R"(\\.\pipe\tachyon-inst-)" + std::to_string(pid);
}

Result<std::unique_ptr<IListener>> listenOn(const std::string& address) {
  auto security = std::make_unique<PipeSecurity>();
  if (!security->init()) return winError(ErrorCode::IoError, "building the pipe security descriptor");
  HANDLE h = createInstance(address, true, *security);
  if (h == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY)
      return winError(ErrorCode::Busy, "pipe already in use: " + address, err);
    return winError(ErrorCode::IoError, "CreateNamedPipe " + address, err);
  }
  return std::unique_ptr<IListener>(new PipeListener(address, h, std::move(security)));
}

Result<std::unique_ptr<IConnection>> connectTo(const std::string& address) {
  // ERROR_PIPE_BUSY: every instance is connected, or the server is between accepting a client and creating
  // the next instance (WaitNamedPipe then fails at once). Keep trying for a few seconds.
  const ULONGLONG deadline = GetTickCount64() + 5000;
  do {
    HANDLE h = CreateFileA(address.c_str(), kPipeClientAccess, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      DWORD mode = PIPE_READMODE_BYTE;
      SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
      return std::unique_ptr<IConnection>(new PipeConnection(h));
    }
    const DWORD err = GetLastError();
    if (err != ERROR_PIPE_BUSY)
      return winError(err == ERROR_FILE_NOT_FOUND ? ErrorCode::NotFound : ErrorCode::IoError,
                      "connect " + address, err);
    if (!WaitNamedPipeA(address.c_str(), 100)) Sleep(5);
  } while (GetTickCount64() < deadline);
  return makeError(ErrorCode::Busy, "pipe busy: " + address);
}

}  // namespace tachyon::control

#endif  // TACHYON_PLATFORM_WINDOWS
