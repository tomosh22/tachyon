// The perf backend's live source (WP-65a, docs/specs/live.md section 1): decodes perf records as the drain
// thread reads them and hands backend-neutral events to a capture::ILiveSink.
//
// Two layers:
//   LiveDecoder  synchronous and single-threaded: a batch of raw perf records goes in (decode()), sink calls
//   come
//                out. It keeps what the sink's identities need: the process / thread tables (FORK, COMM,
//                EXIT, the /proc rundown), one address space per process (MMAP2 / exec) with the executable
//                images found in it (module identity as in perf_converter.h: GNU build-id, ELF base), the
//                target set (launch / attach root, children when followed), and the scheduler tracepoint
//                decoders. It is the live twin of the converter: same rules for what is kept, but it reports
//                raw addresses and lets the sink resolve them (a module is reported when its process first
//                matters, and again when it is unloaded). Records of several rings are merged by timestamp
//                within one decode() call.
//   LiveSource   the thread and the bounded queue: Drain::setTap() calls submit() with every chunk it moved
//   to
//                the spool; the decoder thread takes what is queued roughly every 50 ms, decodes it and sends
//                a heartbeat. submit() never blocks: when the queue is over its byte cap the sample / switch
//                part of a chunk is dropped and counted (reported with ILiveSink::onDropped); the metadata
//                records (FORK / COMM / MMAP / EXIT) are kept up to a much larger cap because losing them
//                corrupts the tables. The spool and so the final trace are never affected.
#pragma once

#include "capture/capture_backend.h"
#include "capture/linux_perf/perf_records.h"
#include "capture/linux_perf/sched_format.h"
#include "capture/linux_perf/spool.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tachyon::capture::perf {

struct LiveDecoderConfig {
  std::shared_ptr<ILiveSink> sink;
  RecordLayout layout;
  SchedInfo sched;  // how the scheduler events were recorded (session.json)
  // Target processes. Empty: system mode, every non-idle sample is kept.
  std::vector<uint32_t> rootPids;
  bool followChildren = false;
  std::string childFilter;
  uint64_t startTs = 0;  // session start (trace clock); preexisting processes and modules are stamped with it
  std::string rootCommandLine;  // command line of the single root (launch); empty: read /proc
  std::filesystem::path procRoot = "/proc";
  // Tests: maps the file name recorded by MMAP2 to the file to read for the module identity.
  std::function<std::filesystem::path(uint32_t pid, std::string_view recordedPath)> fileMapper;
};

struct LiveDecoderStats {
  uint64_t records = 0;
  uint64_t samples = 0;   // samples handed to the sink
  uint64_t switches = 0;  // context switches handed to the sink
  uint64_t readies = 0;   // ready events handed to the sink
  uint64_t droppedNotTarget = 0;
  uint64_t modules = 0;      // load events
  uint64_t unsupported = 0;  // records of a type the decoder skips
  uint64_t malformed = 0;
  uint64_t maxTs = 0;  // newest record timestamp
};

// Not thread-safe: one thread drives it.
class LiveDecoder {
 public:
  explicit LiveDecoder(LiveDecoderConfig config);
  ~LiveDecoder();
  LiveDecoder(const LiveDecoder&) = delete;
  LiveDecoder& operator=(const LiveDecoder&) = delete;

  // Not thread-safe. Decodes whole perf records back to back (as read from one or several rings), ordered by
  // timestamp, and calls the sink. `rundown`: the records are the /proc rundown (stamped with the session
  // start): processes and threads that already existed.
  void decode(std::span<const uint8_t> records, bool rundown = false);

  // Not thread-safe.
  const LiveDecoderStats& stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

struct LiveSourceConfig {
  LiveDecoderConfig decoder;
  uint64_t queueBytes = 64ull << 20;       // pending sample / switch bytes before they are dropped
  uint64_t metaQueueBytes = 256ull << 20;  // pending metadata bytes before the source gives up
  std::chrono::milliseconds period{50};    // how often the thread decodes what is queued
  // A sample / switch chunk that has waited this long when the thread gets to it is dropped (counted,
  // reported with onDropped) instead of decoded: a live view that is seconds behind is worse than one with
  // holes, and dropping the stale data lets the decoder catch up.
  std::chrono::milliseconds maxLag{2000};
  // The trace clock (for the heartbeat); empty: CLOCK_MONOTONIC_RAW.
  std::function<uint64_t()> nowNs;
};

struct LiveSourceStats {
  LiveDecoderStats decoder;
  uint64_t chunksSubmitted = 0;
  uint64_t droppedRecords = 0;  // sample / switch records dropped because the queue was full
  uint64_t metaLost = 0;        // metadata chunks dropped (tables may be wrong afterwards)
  uint64_t queuedBytes = 0;
  uint64_t maxQueuedBytes = 0;
  uint64_t batches = 0;      // decoder wake-ups that had something to do
  double decodeSeconds = 0;  // time spent in decode()
};

class LiveSource {
 public:
  // Thread-safe.
  explicit LiveSource(LiveSourceConfig config);
  // Thread-safe: calls finish().
  ~LiveSource();
  LiveSource(const LiveSource&) = delete;
  LiveSource& operator=(const LiveSource&) = delete;

  // Not thread-safe. Starts the decoder thread.
  void start();
  // Thread-safe, never blocks on the decoder (it copies the bytes). `meta`: non-SAMPLE records, droppable
  // only at the metadata cap; `data`: SAMPLE / SWITCH records, droppable at the queue cap. `rundown`: the
  // records are the /proc rundown.
  void submit(std::span<const uint8_t> meta, std::span<const uint8_t> data, bool rundown = false);
  // Thread-safe, idempotent. Decodes what is still queued, sends a last heartbeat and joins the thread.
  void finish();
  // Thread-safe.
  LiveSourceStats stats() const;

 private:
  struct Chunk {
    std::vector<uint8_t> bytes;
    bool rundown = false;
    bool data = false;  // sample / switch records (droppable)
    std::chrono::steady_clock::time_point queuedAt;
  };
  void run();

  LiveSourceConfig m_config;
  std::unique_ptr<LiveDecoder> m_decoder;  // used by the thread only
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<Chunk> m_queue;
  uint64_t m_queuedBytes = 0;
  uint64_t m_droppedPending = 0;  // dropped records not yet reported to the sink
  LiveSourceStats m_stats;
  bool m_stop = false;
  std::thread m_thread;
};

}  // namespace tachyon::capture::perf
