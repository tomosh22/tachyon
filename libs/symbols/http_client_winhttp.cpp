// WinHTTP backend of IHttpClient (Windows). Uses the automatic proxy configuration.
#include "symbols/http_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <fstream>
#include <vector>

namespace tachyon::symbols {

namespace {

struct InternetHandle {
  HINTERNET h = nullptr;
  InternetHandle() = default;
  explicit InternetHandle(HINTERNET handle) : h(handle) {}
  InternetHandle(const InternetHandle&) = delete;
  InternetHandle& operator=(const InternetHandle&) = delete;
  ~InternetHandle() {
    if (h) WinHttpCloseHandle(h);
  }
};

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

Error winError(const char* what, DWORD code) {
  const ErrorCode ec = code == ERROR_WINHTTP_TIMEOUT ? ErrorCode::Timeout : ErrorCode::IoError;
  return makeError(ec, std::string(what) + " failed, error " + std::to_string(code));
}

class WinHttpClient final : public RetryingHttpClient {
 public:
  explicit WinHttpClient(const HttpOptions& options) : RetryingHttpClient(options) {
    m_session.h = WinHttpOpen(L"tachyon-symbols/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (m_session.h) {
      const int connect = static_cast<int>(options.connectTimeoutMs);
      const int total = static_cast<int>(options.totalTimeoutMs);
      WinHttpSetTimeouts(m_session.h, connect, connect, total, total);
    }
  }

 protected:
  Result<int> transfer(const std::string& url, const std::filesystem::path& partPath,
                       const std::atomic<bool>* cancel, const HttpProgressFn& progress) override {
    if (!m_session.h) return makeError(ErrorCode::Internal, "WinHttpOpen failed");
    const std::wstring wurl = widen(url);

    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts)) {
      return makeError(ErrorCode::InvalidArgument, "bad URL");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";

    InternetHandle connection(WinHttpConnect(m_session.h, host.c_str(), parts.nPort, 0));
    if (!connection.h) return winError("WinHttpConnect", GetLastError());
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request.h) return winError("WinHttpOpenRequest", GetLastError());

    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
      return winError("WinHTTP request", GetLastError());
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
      return winError("WinHttpQueryHeaders", GetLastError());
    }
    uint64_t total = 0;
    DWORD lengthValue = 0;
    DWORD lengthSize = sizeof(lengthValue);
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &lengthValue, &lengthSize,
                            WINHTTP_NO_HEADER_INDEX)) {
      total = lengthValue;
    }

    std::ofstream out(partPath, std::ios::binary | std::ios::trunc);
    if (!out) return makeError(ErrorCode::IoError, "cannot create " + partPath.string());
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options().totalTimeoutMs);
    std::vector<char> buffer(64 * 1024);
    uint64_t received = 0;
    for (;;) {
      if (cancel && cancel->load(std::memory_order_relaxed))
        return makeError(ErrorCode::Cancelled, "cancelled");
      if (std::chrono::steady_clock::now() > deadline) return makeError(ErrorCode::Timeout, "total timeout");
      DWORD got = 0;
      if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
        return winError("WinHttpReadData", GetLastError());
      }
      if (got == 0) break;
      out.write(buffer.data(), static_cast<std::streamsize>(got));
      if (!out) return makeError(ErrorCode::IoError, "write failed: " + partPath.string());
      received += got;
      if (progress && !progress(received, total)) return makeError(ErrorCode::Cancelled, "aborted");
    }
    out.close();
    if (!out) return makeError(ErrorCode::IoError, "write failed: " + partPath.string());
    return static_cast<int>(status);
  }

 private:
  InternetHandle m_session;
};

}  // namespace

std::shared_ptr<IHttpClient> makeWinHttpClient(const HttpOptions& options) {
  return std::make_shared<WinHttpClient>(options);
}

}  // namespace tachyon::symbols
