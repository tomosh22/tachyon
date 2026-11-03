// Spool of raw instrumentation wire frames for one process. No decoding happens on the ingest path:
// complete frames are checked for their boundaries and appended as they came. Every file a spool writes is
// byte for byte a valid `.tachyon-inst` stream (it starts with the client's StreamHeader), so a spool that
// outlives a failed merge can be merged later with `tachyon-cli merge`.
//
// Append-only mode (default): once the file would exceed the cap, the spool becomes `overflowed`: every later
// frame is dropped (a prefix of the stream keeps StringDefs ahead of their uses) and counted. close() appends
// one synthetic `Dropped` frame carrying the count, which decoding turns into the usual `tachyon.dropped`
// marker.
//
// Ring mode (WP-60, flight recorder; SpoolOptions::ringBytes > 0): the spool keeps the NEWEST data within a
// byte bound. It writes a sequence of segment files `<stem>.ring-NNNNNN<ext>`; when the current segment is
// full (segmentBytes) a new one is started and the oldest segments are deleted until the retained bytes fit
// ringBytes again. Every segment is a stand-alone stream: it begins with a copy of the latest StreamHeader,
// every StringDef seen since it (bounded by kMaxRingPrefixBytes) and the latest ThreadName of every thread,
// so frames of a retained segment never lose the strings they refer to. Frames are never dropped for size;
// the records in discarded segments are simply gone (not counted as dropped). A snapshot pins the segments it
// lists, so the merge of a flush can read them while the capture keeps rotating.
//
// A segment holds at least segmentBytes / 2 of payload on top of its prefix, so a prefix close to the
// kMaxRingPrefixBytes cap cannot make segments rotate after every frame (WP-66). When the prefix cap is hit,
// further StringDefs / ThreadNames are not re-emitted (counted in prefixOverflows; the agent reports it as a
// capture warning). A segment file that cannot be created (disk full, permissions) does not stop the spool:
// it keeps appending to the current segment, counts the failure (segmentOpenFailures, lastOpenError) and
// tries again after another segmentBytes / 4 of data.
#pragma once

#include "core/result.h"
#include "inst/frame_stream.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tachyon::inst {

constexpr uint64_t kDefaultMaxSpoolBytes = 1ull << 30;       // 1 GiB per process
constexpr uint64_t kDefaultMaxTotalSpoolBytes = 4ull << 30;  // 4 GiB per capture, all processes together
constexpr uint64_t kMinRingBytes = 64ull << 20;              // flight recorder: at least 64 MiB per process
constexpr uint64_t kMaxRingPrefixBytes = 8ull << 20;         // string defs re-emitted per segment, at most
constexpr size_t kMaxRingThreadNames = 4096;                 // thread names re-emitted per segment, at most

// Disk budget shared by the spools of one capture.
class SpoolBudget {
 public:
  explicit SpoolBudget(uint64_t limit) : m_limit(limit) {}
  // Thread-safe. Takes `bytes` from the budget; false (and nothing taken) when they do not fit.
  bool reserve(uint64_t bytes) {
    uint64_t used = m_used.load(std::memory_order_relaxed);
    do {
      if (used + bytes > m_limit) return false;
    } while (!m_used.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
  }
  // Thread-safe. Takes `bytes` whether or not they fit (data that is written regardless).
  void force(uint64_t bytes) { m_used.fetch_add(bytes, std::memory_order_relaxed); }
  // Thread-safe. Gives `bytes` back (ring spools that deleted a segment).
  void release(uint64_t bytes) {
    uint64_t used = m_used.load(std::memory_order_relaxed);
    uint64_t next;
    do {
      next = used > bytes ? used - bytes : 0;
    } while (!m_used.compare_exchange_weak(used, next, std::memory_order_relaxed));
  }
  // Thread-safe.
  uint64_t used() const { return m_used.load(std::memory_order_relaxed); }

 private:
  const uint64_t m_limit;
  std::atomic<uint64_t> m_used{0};
};

struct SpoolOptions {
  // > 0: ring mode with this many retained bytes (the `maxBytes` argument of create() is then ignored).
  uint64_t ringBytes = 0;
  // Ring mode: size at which a segment is closed. 0: ringBytes / 8 (at least 64 KiB, at most ringBytes / 2).
  uint64_t segmentBytes = 0;
};

// One file of a spool and its size at a frame boundary.
struct SpoolSegment {
  std::filesystem::path file;
  uint64_t bytes = 0;
};

struct SpoolStats {
  uint64_t framesWritten = 0;
  uint64_t bytesWritten = 0;    // frame bytes accepted, including buffered ones (ring: ever, not retained)
  uint64_t framesDropped = 0;   // frames refused because of the cap
  uint64_t recordsDropped = 0;  // records those frames carried (a Dropped frame counts its own count)
  uint64_t truncatedTailBytes = 0;  // bytes of an incomplete last frame when a connection ended
  uint64_t corruptBytes = 0;        // bytes discarded after an implausible frame length
  uint64_t skippedFrames = 0;       // oversized frames skipped by their length
  uint32_t connections = 0;         // streams that delivered data
  bool overflowed = false;
  // Ring mode.
  uint64_t retainedBytes = 0;        // bytes of the segments on disk now (prefixes included)
  uint64_t segmentsStarted = 0;      // segments created so far
  uint64_t segmentsDiscarded = 0;    // oldest segments deleted so far
  uint64_t bytesDiscarded = 0;       // their size
  uint64_t prefixOverflows = 0;      // string defs / thread names that did not fit the re-emitted prefix
  uint64_t segmentOpenFailures = 0;  // rotations that could not create the next segment file
  std::string lastOpenError;         // the message of the latest of them ("" when none)
};

// The files of a spool at one moment, kept alive (not deleted by rotation) while `lease` lives.
struct SpoolSnapshot {
  std::vector<SpoolSegment> segments;  // oldest first
  std::shared_ptr<void> lease;
};

class Spool {
 public:
  // Thread-safe: creates (truncates) `file`; its directory must exist. In ring mode `file` is the base name
  // of the segments (see the class comment).
  // `budget` (optional) is shared with the other spools of the capture; frames that do not fit it are dropped
  // like frames past the per-spool cap (ring mode: the oldest segments go first).
  static Result<std::unique_ptr<Spool>> create(const std::filesystem::path& file, uint64_t maxBytes,
                                               std::shared_ptr<SpoolBudget> budget = nullptr,
                                               SpoolOptions options = {});
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
  // Thread-safe. Oversized frames were skipped by their length.
  void noteSkipped(uint64_t frames);
  // Thread-safe. A new stream (connection) started delivering.
  void noteConnection();

  // Thread-safe. Writes buffered bytes to the file and returns its size (ring mode: of the newest segment).
  // Safe while appends continue; the returned size is a frame boundary.
  Result<uint64_t> flush();
  // Thread-safe. Writes buffered bytes out and returns the files of the spool, oldest first, each with its
  // size at a frame boundary. Ring mode: the segments are pinned until the lease is released.
  Result<SpoolSnapshot> snapshot();
  // Thread-safe, idempotent. Appends the Dropped frame when frames were dropped, flushes, closes the file.
  Result<void> close();

  // Thread-safe.
  SpoolStats stats() const;
  // Thread-safe. Append-only: the file; ring mode: the newest segment.
  std::filesystem::path path() const;
  // Thread-safe. The files that exist now (not flushed), oldest first.
  std::vector<SpoolSegment> segments() const;
  // Thread-safe.
  bool ringMode() const { return m_ring; }

 private:
  struct PinState;

  Spool(std::filesystem::path path, uint64_t maxBytes, std::shared_ptr<SpoolBudget> budget,
        SpoolOptions options);
  Result<void> openSegmentLocked();
  // The size the next segment may reach before it rotates (see the class comment).
  uint64_t segmentLimitLocked(uint64_t prefix) const;
  Result<void> flushLocked();
  void appendRingLocked(const uint8_t* frame, size_t bytes, const FramePeek& peek);
  bool notePrefixLocked(const uint8_t* frame, size_t bytes, const FramePeek& peek);
  void rotateLocked();
  void dropOldestLocked();
  std::filesystem::path segmentPath(uint64_t seq) const;
  void recordDropLocked(const FramePeek& peek);

  const std::filesystem::path m_path;
  const uint64_t m_maxBytes;
  const std::shared_ptr<SpoolBudget> m_budget;
  const bool m_ring;
  uint64_t m_segmentBytes = 0;
  uint64_t m_retryRotateAt = 0;  // after a failed rotation: try again when the segment reaches this size
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

  // Ring mode.
  struct Segment {
    uint64_t seq = 0;
    std::filesystem::path file;
    uint64_t bytes = 0;   // logical size: file plus buffered bytes
    uint64_t prefix = 0;  // of which re-emitted header / strings / names
  };
  std::deque<Segment> m_segments;  // oldest first; back() is the open one
  uint64_t m_nextSeq = 1;
  std::vector<uint8_t> m_prefixHeader;                               // latest StreamHeader frame
  std::vector<uint8_t> m_prefixDefs;                                 // StringDef frames since it
  std::unordered_map<uint32_t, std::vector<uint8_t>> m_prefixNames;  // tid -> latest ThreadName frame
  uint64_t m_prefixNamesBytes = 0;
  std::shared_ptr<PinState> m_pins;
};

}  // namespace tachyon::inst
