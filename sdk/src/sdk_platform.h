// SDK-internal platform layer: trace clock, thread/process ids, environment, byte channels (pipe / socket /
// file). Deliberately independent of libs/ so the shipped SDK stays small (see sdk_channel.cpp).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define TACHYON_SDK_LIKELY(x) (x)
#define TACHYON_SDK_UNLIKELY(x) (x)
#else
#define TACHYON_SDK_LIKELY(x) __builtin_expect(!!(x), 1)
#define TACHYON_SDK_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

namespace tachyon::sdk {

// Thread-safe. Current trace-clock ticks; identical definition to core::nowTicks() (QPC on Windows,
// CLOCK_MONOTONIC_RAW nanoseconds on Linux) but header-inline so the hot path has no call into libs/core.
inline uint64_t nowTicks() {
#if defined(_WIN32)
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return static_cast<uint64_t>(value.QuadPart);
#else
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#endif
}

// Thread-safe. Trace-clock frequency in Hz.
uint64_t ticksFrequency();
// Thread-safe. OS thread id of the caller (Linux gettid, Windows GetCurrentThreadId).
uint32_t currentThreadId();
// Thread-safe.
uint32_t currentProcessId();
// Thread-safe. Environment variable lookup; returns false when unset.
bool getEnv(const char* name, std::string& out);
// Thread-safe. OS-level name of the calling thread, empty when unavailable.
std::string currentOsThreadName();
// Thread-safe. File name of the running executable without directory and extension ("app" when unknown).
std::string executableName();
// Thread-safe. Current working directory ("." when unknown).
std::string currentDirectory();
// Thread-safe. Default agent control address: $TACHYON_AGENT_PIPE, else \\.\pipe\tachyon-agent (Windows) or
// /run/user/<uid>/tachyon-agent.sock with a fallback to /tmp/tachyon-agent.sock (Linux). Mirrors
// control::defaultAgentAddress() (docs/specs/control-protocol.md).
std::string defaultAgentAddress();

// A connected byte stream or an open output file. Not copyable. Thread safety: an instance may be used by one
// thread at a time (callers serialise), except close() which must not race with other calls.
class Channel {
 public:
  Channel() = default;
  ~Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&& other) noexcept { swap(other); }
  Channel& operator=(Channel&& other) noexcept {
    if (this != &other) {
      close();
      swap(other);
    }
    return *this;
  }
  void swap(Channel& other) noexcept;

  // Connects to a Unix socket / named pipe. Waits at most timeoutMs. Returns false on failure.
  bool connect(const std::string& address, int timeoutMs, bool needRead);
  // Creates/truncates a file for writing.
  bool openFile(const std::string& path);
  bool isOpen() const;
  // Writes all bytes or fails (error, or no progress within timeoutMs). Negative timeout waits forever.
  bool writeAll(const void* data, size_t size, int timeoutMs);
  // Returns bytes read (> 0), 0 on orderly close, -1 on timeout, -2 on error.
  int readSome(void* buffer, size_t size, int timeoutMs);
  void close();

 private:
#if defined(_WIN32)
  void* m_handle = nullptr;  // INVALID_HANDLE_VALUE / nullptr when closed
  void* m_event = nullptr;
  bool m_overlapped = false;
#else
  int m_fd = -1;
  bool m_socket = false;
#endif
};

}  // namespace tachyon::sdk
