// The drain thread of a perf capture (WP-70): moves bytes from the kernel's ring buffers to the spool.
//
// One thread polls every ring (poll() on the event fds with a short timeout, so a slow trickle of records
// still reaches the disk promptly), copies whatever is pending, splits it into SAMPLE records and everything
// else, and appends each part to its spool file tagged with the ring id. SAMPLEs go to the data segments
// (rotating and size-limited in circular mode), all other records to meta.bin, which never shrinks. The
// thread also feeds a DescendantTracker with FORK records (so the backend can tell when a followed process
// tree has gone) and tallies LOST / THROTTLE records for live diagnostics. It does no conversion work.
#pragma once

#include "capture/linux_perf/perf_records.h"
#include "capture/linux_perf/perf_system.h"
#include "capture/linux_perf/spool.h"
#include "core/result.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace tachyon::capture::perf {

// The pids of the processes started by the target (FORK records). Thread-safe.
class DescendantTracker {
 public:
  // Thread-safe.
  void setRoots(const std::vector<uint32_t>& roots, bool follow);
  // Thread-safe. Called with every FORK record: a new process (pid == tid) whose parent is a tracked pid.
  void onFork(uint32_t pid, uint32_t ppid, uint32_t tid);
  // Thread-safe. Called with a process leader's EXIT record.
  void onExit(uint32_t pid, uint32_t tid);
  // Thread-safe. The tracked pids (roots and, when following, descendants).
  std::vector<uint32_t> pids() const;

 private:
  mutable std::mutex m_mutex;
  std::unordered_set<uint32_t> m_pids;
  bool m_follow = false;
};

struct DrainStats {
  uint64_t bytesDrained = 0;
  uint64_t samples = 0;
  uint64_t metaRecords = 0;
  uint64_t lostRecords = 0;  // LOST records seen
  uint64_t lostEvents = 0;   // sum of their counts
  uint64_t throttleRecords = 0;
  uint64_t malformedChunks = 0;  // a ring produced bytes that are not a record sequence
  uint64_t ringOverruns = 0;
  uint64_t segmentsWritten = 0;
  uint64_t segmentsDropped = 0;  // circular mode: old segments deleted
  bool ioFailed = false;
  std::string ioError;
};

struct DrainConfig {
  RecordLayout layout;
  std::filesystem::path spoolDir;
  uint32_t circularMb = 0;  // 0: one growing data segment; otherwise rotate and keep about this many MiB
  std::chrono::milliseconds pollInterval{10};
};

struct SealedSegment {
  std::filesystem::path path;
  uint64_t firstTs = 0, lastTs = 0;  // sample timestamps (0 when unknown)
  uint64_t bytes = 0;
};

class Drain {
 public:
  // `rings` are not owned and must outlive the drain. Ring i is tagged with id i in the spool.
  Drain(DrainConfig config, std::vector<IRing*> rings, DescendantTracker* tracker = nullptr);
  ~Drain();
  Drain(const Drain&) = delete;
  Drain& operator=(const Drain&) = delete;

  // Not thread-safe. Creates meta.bin and the first data segment; call once before start().
  Result<void> open();
  // Thread-safe after open(). Appends already-formed records (the /proc rundown) to meta.bin.
  Result<void> appendMeta(uint32_t ringId, std::span<const uint8_t> records);
  // Not thread-safe. Starts the thread.
  void start();
  // Not thread-safe. Tells the thread to take a last look at the rings, then joins it and closes the files.
  // Call after the events were disabled. Idempotent.
  Result<void> finish();

  // Thread-safe. Circular mode: closes the current data segment, opens the next one and returns every sealed
  // segment (oldest first). The caller converts them and then calls release().
  Result<std::vector<SealedSegment>> sealForFlush();
  // Thread-safe. Deletes the given sealed segments.
  void release(const std::vector<SealedSegment>& segments);
  // Thread-safe. The data segments that exist (sealed and current), oldest first.
  std::vector<SealedSegment> segments() const;

  // Thread-safe. A snapshot.
  DrainStats stats() const;

 private:
  struct Segment {
    SealedSegment info;
    uint32_t index = 0;
  };
  void run();
  void drainAll();
  void process(uint32_t ringId, const std::vector<uint8_t>& bytes);
  void appendSplit(uint32_t ringId, const std::vector<uint8_t>& meta, const std::vector<uint8_t>& data);
  Result<void> openSegmentLocked();
  void sealCurrentLocked();
  void enforceBudgetLocked();
  void fail(const Error& e);

  DrainConfig m_config;
  std::vector<IRing*> m_rings;
  DescendantTracker* m_tracker;

  mutable std::mutex m_mutex;  // writers, segments, stats
  std::unique_ptr<SpoolWriter> m_meta;
  std::unique_ptr<SpoolWriter> m_data;
  std::vector<Segment> m_segments;  // last one is the current (open) segment
  uint32_t m_nextSegment = 0;
  DrainStats m_stats;

  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::mutex m_wakeMutex;
  std::condition_variable m_wake;
  bool m_opened = false;
  bool m_finished = false;
  std::vector<uint8_t> m_scratch, m_metaBuf, m_dataBuf;
};

}  // namespace tachyon::capture::perf
