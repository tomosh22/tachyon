// Append-only spool of raw instrumentation wire frames for one process. No decoding happens on the ingest
// path: complete frames are checked for their boundaries and appended as they came. A spool file is byte for
// byte a valid `.tachyon-inst` stream (it starts with the client's StreamHeader), so a spool that outlives a
// failed merge can be merged later with `tachyon-cli merge`.
//
// Size bound: once the file would exceed the cap, the spool becomes `overflowed`: every later frame is
// dropped (a prefix of the stream keeps StringDefs ahead of their uses) and counted. close() appends one
// synthetic `Dropped` frame carrying the count, which decoding turns into the usual `tachyon.dropped` marker.
#pragma once

#include "core/result.h"
#include "inst/frame_stream.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace tachyon::inst {

constexpr uint64_t kDefaultMaxSpoolBytes = 1ull << 30;  // 1 GiB per process

struct SpoolStats {
  uint64_t framesWritten = 0;
  uint64_t bytesWritten = 0;        // file size including buffered bytes
  uint64_t framesDropped = 0;       // frames refused because of the cap
  uint64_t recordsDropped = 0;      // records those frames carried (a Dropped frame counts its own count)
  uint64_t truncatedTailBytes = 0;  // bytes of an incomplete last frame when a connection ended
  uint64_t corruptBytes = 0;        // bytes discarded after an implausible frame length
  uint32_t connections = 0;         // streams that delivered data
  bool overflowed = false;
};

class Spool {
 public:
  // Thread-safe: creates (truncates) `file`; its directory must exist.
  static Result<std::unique_ptr<Spool>> create(const std::filesystem::path& file, uint64_t maxBytes);
  // Thread-safe: closes the file (see close()).
  ~Spool();
  Spool(const Spool&) = delete;
  Spool& operator=(const Spool&) = delete;

  // Thread-safe. Appends one complete frame (header + payload), or drops and counts it past the cap.
  void appendFrame(const uint8_t* frame, size_t bytes);
  // Thread-safe. A stream ended with `tailBytes` of an incomplete frame; they are discarded.
  void noteTruncatedTail(size_t tailBytes);
  // Thread-safe. A stream became corrupt; `discarded` bytes were thrown away.
  void noteCorrupt(uint64_t discarded);
  // Thread-safe. A new stream (connection) started delivering.
  void noteConnection();

  // Thread-safe. Writes buffered bytes to the file and returns its size. Safe while appends continue; the
  // returned size is a frame boundary.
  Result<uint64_t> flush();
  // Thread-safe, idempotent. Appends the Dropped frame when frames were dropped, flushes, closes the file.
  Result<void> close();

  // Thread-safe.
  SpoolStats stats() const;
  // Thread-safe.
  const std::filesystem::path& path() const { return m_path; }

 private:
  Spool(std::filesystem::path path, std::ofstream file, uint64_t maxBytes);
  Result<void> flushLocked();

  const std::filesystem::path m_path;
  const uint64_t m_maxBytes;
  mutable std::mutex m_mutex;
  std::ofstream m_file;
  std::vector<uint8_t> m_buffer;
  SpoolStats m_stats;
  bool m_closed = false;
  bool m_ioError = false;
  // First dropped record, for the synthetic Dropped frame.
  bool m_haveFirstDrop = false;
  uint64_t m_firstDropTs = 0;
  uint32_t m_firstDropTid = 0;
  uint64_t m_lastTs = 0;  // newest timestamp seen in a stored frame
};

}  // namespace tachyon::inst
