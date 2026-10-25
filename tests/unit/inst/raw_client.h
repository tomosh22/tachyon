// Test-only raw byte client for the instrumentation pipe / socket (what the SDK does on its data path).
#pragma once

#include "core/platform.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace tachyon::inst::testing_util {

class RawClient {
 public:
  RawClient() = default;
  RawClient(const RawClient&) = delete;
  RawClient& operator=(const RawClient&) = delete;
  ~RawClient() { close(); }

  // Connects, retrying for up to timeoutMs (the listener may still be starting).
  bool connect(const std::string& address, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
      if (tryConnect(address)) return true;
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  // Writes all bytes; false when the peer is gone.
  bool send(const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (size > 0) {
#if TACHYON_PLATFORM_WINDOWS
      DWORD wrote = 0;
      if (!WriteFile(m_handle, p, static_cast<DWORD>(size), &wrote, nullptr)) return false;
      p += wrote;
      size -= wrote;
#else
      const ssize_t w = ::send(m_fd, p, size, MSG_NOSIGNAL);
      if (w <= 0) return false;
      p += w;
      size -= static_cast<size_t>(w);
#endif
    }
    return true;
  }

  // Writes in pieces of at most `piece` bytes, to exercise frame reassembly.
  bool sendChunked(const std::vector<uint8_t>& bytes, size_t piece) {
    for (size_t off = 0; off < bytes.size(); off += piece) {
      const size_t n = piece < bytes.size() - off ? piece : bytes.size() - off;
      if (!send(bytes.data() + off, n)) return false;
    }
    return true;
  }

  void close() {
#if TACHYON_PLATFORM_WINDOWS
    if (m_handle != INVALID_HANDLE_VALUE) CloseHandle(m_handle);
    m_handle = INVALID_HANDLE_VALUE;
#else
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
#endif
  }

 private:
  bool tryConnect(const std::string& address) {
#if TACHYON_PLATFORM_WINDOWS
    m_handle = CreateFileA(address.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(m_handle, &mode, nullptr, nullptr);
    return true;
#else
    m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_fd < 0) return false;
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, address.c_str(), address.size() + 1);
    if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) return true;
    ::close(m_fd);
    m_fd = -1;
    return false;
#endif
  }

#if TACHYON_PLATFORM_WINDOWS
  HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
  int m_fd = -1;
#endif
};

}  // namespace tachyon::inst::testing_util
