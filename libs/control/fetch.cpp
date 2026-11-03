#include "control/fetch.h"

#include "control/sha256.h"

#include <fstream>
#include <system_error>

namespace tachyon::control {

namespace {

namespace fs = std::filesystem;

// One pass over the remote file starting at the size of the .part file.
struct Attempt {
  Result<FetchResult> result = makeError(ErrorCode::Internal, "not run");
  bool stale = false;     // the .part is longer than the remote file
  bool mismatch = false;  // checksum failure
  uint64_t resumedFrom = 0;
};

Attempt runAttempt(Client& client, const std::string& remotePath, const fs::path& part, const fs::path& dest,
                   const FetchOptions& options) {
  Attempt attempt;
  std::error_code ec;
  uint64_t offset = 0;
  if (fs::exists(part, ec)) {
    const auto size = fs::file_size(part, ec);
    if (!ec) offset = size;
  }
  const uint64_t resumedFrom = offset;
  attempt.resumedFrom = offset;
  // Opened at the first byte (or at the end for an empty file): a request the agent refuses leaves no file.
  std::ofstream out;
  auto ensureOpen = [&]() -> bool {
    if (out.is_open()) return true;
    out.open(part, std::ios::binary | std::ios::app);
    if (!out) {
      attempt.result = makeError(ErrorCode::IoError, "cannot open " + part.string() + " for writing");
      return false;
    }
    return true;
  };
  uint64_t total = 0;
  std::string expectedHash;
  for (;;) {
    if ((options.cancel != nullptr && options.cancel->load()) ||
        (options.progress && !options.progress(offset, total))) {
      attempt.result = makeError(ErrorCode::Cancelled, "fetch cancelled");
      return attempt;
    }
    FetchRequest req;
    req.path = remotePath;
    req.offset = offset;
    req.length = kMaxFetchChunkBytes;
    auto reply = client.call(req, options.callTimeoutMs);
    if (!reply.ok()) {
      attempt.result = std::move(reply).error();
      return attempt;
    }
    if (!reply->ok) {
      attempt.result = makeError(reply->code, reply->error);
      return attempt;
    }
    const auto* body = std::get_if<FetchBody>(&reply->body);
    if (body == nullptr || body->offset != offset) {
      attempt.result = makeError(ErrorCode::CorruptData, "unexpected fetch response");
      return attempt;
    }
    total = body->size;
    if (!body->data.empty()) {
      if (!ensureOpen()) return attempt;
      out.write(body->data.data(), static_cast<std::streamsize>(body->data.size()));
      if (!out) {
        attempt.result = makeError(ErrorCode::IoError, "write to " + part.string() + " failed");
        return attempt;
      }
      offset += body->data.size();
    }
    if (!body->sha256.empty()) expectedHash = body->sha256;
    if (offset > total) {
      // The .part is longer than the remote file (it belonged to something else), or the file shrank.
      attempt.result =
          makeError(ErrorCode::CorruptData, "the local partial file is longer than the remote file");
      attempt.stale = true;
      return attempt;
    }
    if (offset == total) break;
    if (body->data.empty()) {
      attempt.result = makeError(ErrorCode::CorruptData, "the agent returned no data before the end of file");
      return attempt;
    }
  }
  if (!ensureOpen()) return attempt;  // an empty remote file still produces an (empty) .part
  out.close();
  if (options.progress) (void)options.progress(offset, total);

  if (expectedHash.empty()) {
    // The final chunk always carries the hash; this only happens when the file was already complete and the
    // last request returned no data. Ask for it explicitly.
    FetchRequest req;
    req.path = remotePath;
    req.offset = offset;
    req.length = 0;
    req.want_sha256 = true;
    auto reply = client.call(req, options.callTimeoutMs);
    if (!reply.ok()) {
      attempt.result = std::move(reply).error();
      return attempt;
    }
    if (!reply->ok) {
      attempt.result = makeError(reply->code, reply->error);
      return attempt;
    }
    if (const auto* body = std::get_if<FetchBody>(&reply->body)) expectedHash = body->sha256;
  }
  auto actual = sha256OfFile(part);
  if (!actual.ok()) {
    attempt.result = std::move(actual).error();
    return attempt;
  }
  if (expectedHash.empty() || *actual != expectedHash) {
    attempt.mismatch = true;
    attempt.result =
        makeError(ErrorCode::CorruptData, "SHA-256 mismatch after receiving " + remotePath + " (expected " +
                                              expectedHash + ", got " + *actual + ")");
    return attempt;
  }
  fs::rename(part, dest, ec);
  if (ec) {
    attempt.result = makeError(
        ErrorCode::IoError, "cannot rename " + part.string() + " to " + dest.string() + ": " + ec.message());
    return attempt;
  }
  FetchResult done;
  done.path = dest;
  done.size = total;
  done.resumedFrom = resumedFrom;
  done.sha256 = *actual;
  attempt.result = std::move(done);
  return attempt;
}

}  // namespace

Result<FetchResult> fetchFile(Client& client, const std::string& remotePath, const fs::path& destination,
                              const FetchOptions& options) {
  std::error_code ec;
  if (destination.has_parent_path()) fs::create_directories(destination.parent_path(), ec);
  const fs::path part = destination.string() + ".part";
  bool restarted = false;
  for (int pass = 0; pass < 2; ++pass) {
    Attempt attempt = runAttempt(client, remotePath, part, destination, options);
    if (attempt.result.ok()) {
      attempt.result->restarted = restarted;
      return std::move(attempt.result);
    }
    // A resumed transfer that went wrong (stale or damaged .part) starts over once; a fresh one that fails
    // its checksum is an error.
    const bool recoverable = pass == 0 && options.restartOnMismatch &&
                             (attempt.stale || (attempt.mismatch && attempt.resumedFrom > 0));
    if (attempt.mismatch || attempt.stale) fs::remove(part, ec);
    if (!recoverable) return std::move(attempt.result);
    restarted = true;
  }
  return makeError(ErrorCode::Internal, "fetch failed");
}

Result<std::vector<ProcessEntry>> fetchProcessList(Client& client, int callTimeoutMs) {
  std::vector<ProcessEntry> all;
  uint32_t offset = 0;
  for (int guard = 0; guard < 1000; ++guard) {
    ProcessesRequest req;
    req.offset = offset;
    auto reply = client.call(req, callTimeoutMs);
    if (!reply.ok()) return std::move(reply).error();
    if (!reply->ok) return makeError(reply->code, reply->error);
    const auto* body = std::get_if<ProcessesBody>(&reply->body);
    if (body == nullptr) return makeError(ErrorCode::CorruptData, "unexpected processes response");
    for (const ProcessEntry& p : body->processes) all.push_back(p);
    offset += static_cast<uint32_t>(body->processes.size());
    if (body->processes.empty() || offset >= body->total) return all;
  }
  return all;
}

}  // namespace tachyon::control
