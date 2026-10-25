// Minimal HTTP GET-to-file client behind an interface (WP-21): libcurl on Linux, WinHTTP on Windows, fakes in
// tests.
#pragma once

#include "core/result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace tachyon::symbols {

struct HttpOptions {
  uint32_t connectTimeoutMs = 10000;  // TCP/TLS connect
  uint32_t totalTimeoutMs = 120000;   // per attempt, whole transfer
  uint32_t maxRetries = 3;            // extra attempts after the first, on 5xx / 429 / timeouts / IO errors
  uint32_t initialBackoffMs = 250;    // doubled per retry
  uint32_t maxBackoffMs = 4000;
};

struct HttpStatus {
  int code = 0;           // final HTTP status (200 on success)
  uint64_t bytes = 0;     // bytes written to the destination
  uint32_t attempts = 1;  // transfers performed, including retries
};

// Called with (bytesReceived, totalBytesOrZero) from the transferring thread; returning false aborts the
// transfer with ErrorCode::Cancelled.
using HttpProgressFn = std::function<bool(uint64_t received, uint64_t total)>;

class IHttpClient {
 public:
  virtual ~IHttpClient() = default;
  // Implementations are thread-safe. Downloads `url` to `destination` through `<destination>.part` and an
  // atomic rename; the part file is removed on every failure. 404 -> NotFound immediately; other 4xx ->
  // IoError; 5xx, timeouts and transport errors are retried with backoff and then reported as IoError /
  // Timeout. `cancel` (optional) is polled; progress (optional) may abort.
  virtual Result<HttpStatus> get(const std::string& url, const std::filesystem::path& destination,
                                 const std::atomic<bool>* cancel, const HttpProgressFn& progress) = 0;
};

// Shared retry / backoff / `.part` handling; backends implement one transfer attempt.
class RetryingHttpClient : public IHttpClient {
 public:
  // Thread-safe.
  Result<HttpStatus> get(const std::string& url, const std::filesystem::path& destination,
                         const std::atomic<bool>* cancel, const HttpProgressFn& progress) final;

  const HttpOptions& options() const { return m_options; }

 protected:
  explicit RetryingHttpClient(HttpOptions options) : m_options(options) {}
  // One attempt: truncate-write the body to `partPath` and return the HTTP status code (any status is a
  // success of the transport). Transport failures return Timeout / IoError, aborts return Cancelled.
  virtual Result<int> transfer(const std::string& url, const std::filesystem::path& partPath,
                               const std::atomic<bool>* cancel, const HttpProgressFn& progress) = 0;

 private:
  HttpOptions m_options;
};

// Thread-safe: pure function. Removes `user:pass@` from the authority of a URL for logging.
std::string redactUrl(std::string_view url);

// Thread-safe. The platform client (libcurl on Linux, WinHTTP on Windows); honours the proxy environment.
std::shared_ptr<IHttpClient> makeDefaultHttpClient(const HttpOptions& options = {});

}  // namespace tachyon::symbols
