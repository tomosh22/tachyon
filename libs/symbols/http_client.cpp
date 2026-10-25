#include "symbols/http_client.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <system_error>
#include <thread>

namespace tachyon::symbols {

#if defined(_WIN32)
std::shared_ptr<IHttpClient> makeWinHttpClient(const HttpOptions& options);
#else
std::shared_ptr<IHttpClient> makeCurlHttpClient(const HttpOptions& options);
#endif

std::shared_ptr<IHttpClient> makeDefaultHttpClient(const HttpOptions& options) {
#if defined(_WIN32)
  return makeWinHttpClient(options);
#else
  return makeCurlHttpClient(options);
#endif
}

std::string redactUrl(std::string_view url) {
  const size_t scheme = url.find("://");
  const size_t authStart = scheme == std::string_view::npos ? 0 : scheme + 3;
  const size_t authEnd = url.find_first_of("/?#", authStart);
  const size_t end = authEnd == std::string_view::npos ? url.size() : authEnd;
  const size_t at = url.substr(authStart, end - authStart).rfind('@');
  if (at == std::string_view::npos) return std::string(url);
  std::string out(url.substr(0, authStart));
  out += url.substr(authStart + at + 1);
  return out;
}

namespace {

bool isCancelled(const std::atomic<bool>* cancel) {
  return cancel && cancel->load(std::memory_order_relaxed);
}

// Sleeps `ms` in slices; false if cancelled meanwhile.
bool sleepCancellable(uint32_t ms, const std::atomic<bool>* cancel) {
  uint32_t left = ms;
  while (left > 0) {
    if (isCancelled(cancel)) return false;
    const uint32_t slice = std::min<uint32_t>(left, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    left -= slice;
  }
  return !isCancelled(cancel);
}

void removeQuiet(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::remove(p, ec);
}

}  // namespace

Result<HttpStatus> RetryingHttpClient::get(const std::string& url, const std::filesystem::path& destination,
                                           const std::atomic<bool>* cancel, const HttpProgressFn& progress) {
  std::error_code ec;
  if (destination.has_parent_path()) {
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
      return makeError(ErrorCode::IoError,
                       "cannot create " + destination.parent_path().string() + ": " + ec.message());
    }
  }
  std::filesystem::path part = destination;
  part += ".part";
  const std::string shown = redactUrl(url);

  Error last{ErrorCode::IoError, "no attempt made"};
  uint32_t backoff = m_options.initialBackoffMs;
  for (uint32_t attempt = 0; attempt <= m_options.maxRetries; ++attempt) {
    if (attempt > 0) {
      TACHYON_LOG_DEBUG("http: retry {} of {} for {} ({})", attempt, m_options.maxRetries, shown,
                        last.message);
      if (!sleepCancellable(backoff, cancel)) {
        removeQuiet(part);
        return makeError(ErrorCode::Cancelled, "cancelled: " + shown);
      }
      backoff = std::min(backoff * 2, m_options.maxBackoffMs);
    }
    if (isCancelled(cancel)) {
      removeQuiet(part);
      return makeError(ErrorCode::Cancelled, "cancelled: " + shown);
    }

    Result<int> r = transfer(url, part, cancel, progress);
    if (!r.ok()) {
      removeQuiet(part);
      if (r.error().code == ErrorCode::Cancelled) return r.error();
      last = r.error();
      continue;  // Timeout / IoError: retry
    }
    const int code = r.value();
    if (code >= 200 && code < 300) {
      std::error_code sz;
      const uint64_t bytes = static_cast<uint64_t>(std::filesystem::file_size(part, sz));
      std::filesystem::rename(part, destination, ec);
      if (ec) {
        removeQuiet(part);
        return makeError(ErrorCode::IoError, "cannot move " + part.string() + " into place: " + ec.message());
      }
      return HttpStatus{code, sz ? 0 : bytes, attempt + 1};
    }
    removeQuiet(part);
    if (code == 404 || code == 410)
      return makeError(ErrorCode::NotFound, "HTTP " + std::to_string(code) + ": " + shown);
    if (code >= 500 || code == 429 || code == 408) {
      last = Error{ErrorCode::IoError, "HTTP " + std::to_string(code) + ": " + shown};
      continue;
    }
    return makeError(ErrorCode::IoError, "HTTP " + std::to_string(code) + ": " + shown);
  }
  return last;
}

}  // namespace tachyon::symbols
