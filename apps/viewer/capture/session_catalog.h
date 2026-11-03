// SessionCatalog (WP-41): the data behind the Session Explorer. The list is the recent files plus every
// *.tachyon in the captures directory; per file the metadata of the CaptureInfo chunk (mode, target, sampling
// rate, duration, events lost, agent version) and the file size. Everything that touches the disk runs on a
// worker thread; results are cached by (path, mtime, size). A file that is missing or unreadable becomes a
// row with a state, never an error.
#pragma once

#include "capture/capture_backend.h"
#include "viewer/capture/process_launcher.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tachyon::viewer {

enum class SessionState : uint8_t {
  Pending,     // listed, metadata not read yet
  Ok,          // header and CaptureInfo read (CaptureInfo may be absent: fields stay empty)
  Missing,     // the file does not exist (a stale recent-files entry)
  Unreadable,  // exists but is not a readable trace
};

struct SessionInfo {
  std::filesystem::path path;
  SessionState state = SessionState::Pending;
  std::string error;  // Unreadable: why
  uint64_t sizeBytes = 0;
  int64_t mtime = 0;  // seconds since the Unix epoch (the capture time: the file is final when it is written)
  bool partial = false;
  bool hasCaptureInfo = false;
  std::string mode;  // "launch" / "attach" / "system" / "process" (converter wording), "" if unknown
  std::string exe;   // target executable, "" if unknown
  uint32_t pid = 0;  // target pid, 0 if unknown
  uint32_t samplingHz = 0;
  double durationS = 0.0;
  uint64_t eventsLost = 0;
  std::string agentVersion;
  bool recent = false;  // listed in Settings::recentFiles
  bool inCapturesDir = false;
};

enum class SessionSort : uint8_t { Time, Name, Size };

// Thread-safe (reads the filesystem). Reads one file's row: stat + trace header + CaptureInfo. Never throws.
SessionInfo readSessionInfo(const std::filesystem::path& path);

// Thread-safe: pure functions on the rows.
// Sorted copy: Time (newest first unless `ascending`), Name (case-insensitive file name), Size (largest first
// unless `ascending`). Rows that are not Ok sort after the others for Time/Size.
std::vector<SessionInfo> sortSessions(std::vector<SessionInfo> rows, SessionSort key, bool ascending);
// Rows whose file name, path, mode, target or agent version contain `text` (case-insensitive; empty = all).
std::vector<SessionInfo> filterSessions(const std::vector<SessionInfo>& rows, const std::string& text);

// Thread-safe (reads the filesystem). Files `deleteSession` would remove for `trace`: the trace itself, then
// "<trace>.etl", "<trace>.perfspool" and "<trace>.inst" (the names the capture backends and the agent give
// them), each only if it exists and sits next to the trace.
std::vector<std::filesystem::path> sessionFilesFor(const std::filesystem::path& trace);
// Thread-safe (writes the filesystem). Removes sessionFilesFor(trace). Stops at the first failure; `removed`
// (if given) lists what was deleted.
Result<void> deleteSession(const std::filesystem::path& trace,
                           std::vector<std::filesystem::path>* removed = nullptr);

// Thread-safe: pure function. What "Reveal" runs: xdg-open on the parent directory (Linux) /
// `explorer /select,<file>` (Windows).
LaunchSpec revealSpec(const std::filesystem::path& file);

class SessionCatalog {
 public:
  SessionCatalog();
  // Main-thread only. Joins the worker.
  ~SessionCatalog();
  SessionCatalog(const SessionCatalog&) = delete;
  SessionCatalog& operator=(const SessionCatalog&) = delete;

  // Thread-safe. Replaces the pending request: list `recent` (any path, missing ones become Missing rows) and
  // the *.tachyon files of `capturesDir` (empty = none). Returns at once.
  void refresh(std::vector<std::filesystem::path> recent, std::filesystem::path capturesDir);
  // Thread-safe. Called by the worker after it published new rows (set before the first refresh()).
  void setOnUpdate(std::function<void()> onUpdate);
  // Thread-safe. True and `rows` replaced if the published rows changed since the last poll().
  bool poll(std::vector<SessionInfo>& rows);
  // Thread-safe. True while a refresh is queued or running.
  bool busy() const;
  // Thread-safe. Blocks until the queue is empty and the worker idle (tests).
  void waitIdle();
  // Thread-safe. Number of CaptureInfo reads done so far (cache misses; tests).
  uint64_t metadataReads() const { return m_reads.load(); }

 private:
  struct Request {
    std::vector<std::filesystem::path> recent;
    std::filesystem::path dir;
  };
  struct CacheEntry {
    int64_t mtime = 0;
    uint64_t size = 0;
    SessionInfo info;
  };

  void workerMain();
  void process(const Request& request);
  void publish(std::vector<SessionInfo> rows);

  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::condition_variable m_idleCv;
  std::optional<Request> m_pending;
  bool m_running = false;
  bool m_quit = false;
  std::vector<SessionInfo> m_rows;
  bool m_changed = false;
  std::function<void()> m_onUpdate;
  std::atomic<uint64_t> m_reads{0};
  std::map<std::string, CacheEntry> m_cache;  // worker only
  std::thread m_worker;
};

}  // namespace tachyon::viewer
