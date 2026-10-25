// libcurl backend of IHttpClient (Linux). libcurl reads the proxy environment variables itself.
#include "symbols/http_client.h"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace tachyon::symbols {

namespace {

void ensureCurlGlobalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct TransferState {
  FILE* file = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  const HttpProgressFn* progress = nullptr;
  bool aborted = false;
  bool writeFailed = false;
};

size_t writeCallback(char* data, size_t size, size_t count, void* user) {
  auto* st = static_cast<TransferState*>(user);
  const size_t n = size * count;
  if (n == 0) return 0;
  if (std::fwrite(data, 1, n, st->file) != n) {
    st->writeFailed = true;
    return 0;
  }
  return n;
}

int xferCallback(void* user, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
  auto* st = static_cast<TransferState*>(user);
  if (st->cancel && st->cancel->load(std::memory_order_relaxed)) {
    st->aborted = true;
    return 1;
  }
  if (st->progress && *st->progress &&
      !(*st->progress)(static_cast<uint64_t>(dlNow), static_cast<uint64_t>(dlTotal))) {
    st->aborted = true;
    return 1;
  }
  return 0;
}

class CurlHttpClient final : public RetryingHttpClient {
 public:
  explicit CurlHttpClient(const HttpOptions& options) : RetryingHttpClient(options) {
    ensureCurlGlobalInit();
  }

 protected:
  Result<int> transfer(const std::string& url, const std::filesystem::path& partPath,
                       const std::atomic<bool>* cancel, const HttpProgressFn& progress) override {
    TransferState st;
    st.cancel = cancel;
    st.progress = &progress;
    st.file = std::fopen(partPath.string().c_str(), "wb");
    if (!st.file) return makeError(ErrorCode::IoError, "cannot create " + partPath.string());

    CURL* curl = curl_easy_init();
    if (!curl) {
      std::fclose(st.file);
      return makeError(ErrorCode::Internal, "curl_easy_init failed");
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options().connectTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(options().totalTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tachyon-symbols/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &st);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &st);
    // libcurl only consults its compiled-in CA path; honour the conventional environment overrides too.
    for (const char* var : {"CURL_CA_BUNDLE", "SSL_CERT_FILE"}) {
      const char* ca = std::getenv(var);
      if (ca && *ca) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
        break;
      }
    }

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    std::string detail = curl_easy_strerror(rc);
    curl_easy_cleanup(curl);
    const bool flushed = std::fclose(st.file) == 0;

    if (st.aborted) return makeError(ErrorCode::Cancelled, "transfer aborted");
    if (rc == CURLE_OPERATION_TIMEDOUT) return makeError(ErrorCode::Timeout, "timeout: " + detail);
    if (rc != CURLE_OK) return makeError(ErrorCode::IoError, "transfer failed: " + detail);
    if (st.writeFailed || !flushed)
      return makeError(ErrorCode::IoError, "write failed: " + partPath.string());
    return static_cast<int>(status);
  }
};

}  // namespace

std::shared_ptr<IHttpClient> makeCurlHttpClient(const HttpOptions& options) {
  return std::make_shared<CurlHttpClient>(options);
}

}  // namespace tachyon::symbols
