// Client side of the `fetch` command (WP-61): streams a trace the agent produced into a local file, resuming
// an interrupted transfer, and verifies it against the SHA-256 the agent computed.
#pragma once

#include "control/client.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace tachyon::control {

struct FetchOptions {
  // Called after every chunk with the bytes present locally and the size of the remote file. Return false to
  // cancel (the .part file is kept for a later resume). May be empty.
  std::function<bool(uint64_t done, uint64_t total)> progress;
  // Optional cancellation flag, polled between chunks like `progress`.
  const std::atomic<bool>* cancel = nullptr;
  // Timeout of one request.
  int callTimeoutMs = 30000;
  // A resumed transfer whose checksum does not match (the remote file changed, or the .part was damaged)
  // is discarded and fetched again from the start, once. A fresh transfer that mismatches is an error.
  bool restartOnMismatch = true;
};

struct FetchResult {
  std::filesystem::path path;  // the finished local file
  uint64_t size = 0;
  uint64_t resumedFrom = 0;  // bytes that were already in the .part file when the (last) attempt began
  bool restarted = false;    // a mismatch or a stale .part forced a second attempt from byte 0
  std::string sha256;        // hex, verified
};

// Not thread-safe with other use of `client`. Fetches `remotePath` (a path the agent reported in
// capture_done, flushed or a flush response) into `destination`, writing `<destination>.part` first and
// renaming it when the checksum matches. Existing `<destination>.part` content is resumed from its size.
// Fails with
//   NotFound / NoPrivilege ...  what the agent answered (it only serves files it produced),
//   CorruptData                 the SHA-256 of the received bytes differs from the agent's,
//   Cancelled                   progress() returned false or the flag was set,
//   IoError / transport errors  (the .part file stays, so calling again resumes).
Result<FetchResult> fetchFile(Client& client, const std::string& remotePath,
                              const std::filesystem::path& destination, const FetchOptions& options = {});

// Not thread-safe with other use of `client`. Fetches the whole process list of the agent (repeating the
// `processes` request until every entry has arrived).
Result<std::vector<ProcessEntry>> fetchProcessList(Client& client, int callTimeoutMs = 10000);

}  // namespace tachyon::control
