// LiveEtwSource (WP-65w, docs/specs/live.md section 1): the thread and the bounded queue between the ETW
// real-time consumer and the LiveEtwDecoder. The ETW callback thread must never wait (a consumer that does
// loses real-time buffers), so it only copies the few kernel events the decoder understands into chunks, one
// pair (metadata, data) per ETW buffer; this class's thread takes the chunks roughly every `period`, orders
// the events of a batch by timestamp (the buffers of the CPUs arrive in any order) and decodes them.
//
// Same rules as the perf live source:
//   * metadata (Process / Thread / Image events) is kept up to a large cap; losing it corrupts the tables;
//   * sample / switch / ready / stack data is dropped (counted, reported with ILiveSink::onDropped) when the
//     queue is over its cap or when a chunk is older than `maxLag` when the thread gets to it: a view that is
//     seconds behind is worse than one with holes;
//   * the ETL (the final trace) is never affected: this is a second reader of the same session.
// Data chunks are held back for `holdBack` after they arrive so that the metadata that explains them (an
// image load from another CPU's buffer) has arrived too.
//
// Platform independent; the Windows consumer (rt_consumer.h) is its only producer outside tests.
#pragma once

#include "capture/windows_etw/live_etw_decoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tachyon::capture::etw {

// Thread-safe. CPU time (kernel + user) the calling thread has used so far, in seconds; 0 when unknown.
double currentThreadCpuSeconds();

struct LiveEtwSourceConfig {
  // The decoder's configuration; rootPids are supplied by arm().
  LiveEtwDecoderConfig decoder;
  uint64_t queueBytes = 128ull << 20;     // pending data bytes before they are dropped
  uint64_t metaQueueBytes = 64ull << 20;  // pending metadata bytes before the source gives up
  std::chrono::milliseconds period{50};   // how often the thread decodes what is queued
  std::chrono::milliseconds holdBack{200};
  std::chrono::milliseconds maxLag{2000};
  // The trace clock now (for the heartbeat); empty: no heartbeat.
  std::function<uint64_t()> nowTicks;
};

struct LiveEtwSourceStats {
  LiveEtwDecoderStats decoder;
  uint64_t eventsSubmitted = 0;  // events copied into chunks
  uint64_t eventsIgnored = 0;    // events the decoder does not understand (not copied)
  uint64_t chunks = 0;
  uint64_t droppedEvents = 0;  // data events dropped: queue cap or maxLag
  uint64_t metaLost = 0;       // metadata events dropped (tables may be wrong afterwards)
  uint64_t queuedBytes = 0;
  uint64_t maxQueuedBytes = 0;
  uint64_t batches = 0;           // wake-ups that decoded something
  double decodeSeconds = 0;       // wall time spent decoding
  double decodeCpuSeconds = 0;    // cpu time of the decoder thread
  double maxEventAgeSeconds = 0;  // oldest event, by arrival, when it was decoded
};

class LiveEtwSource {
 public:
  // Thread-safe.
  explicit LiveEtwSource(LiveEtwSourceConfig config);
  // Thread-safe: calls finish().
  ~LiveEtwSource();
  LiveEtwSource(const LiveEtwSource&) = delete;
  LiveEtwSource& operator=(const LiveEtwSource&) = delete;

  // Not thread-safe. Starts the thread. It waits for arm() before it decodes anything.
  void start();
  // Thread-safe, once. Fixes the target processes (empty: system mode) and lets the thread run. `rundown`
  // (optional) is called on the source thread before the first event is decoded and its result is applied
  // (processes, threads and modules that existed before the session). A launched process is only known after
  // the session started, so the consumer can be connected (events queue up) before this call.
  void arm(std::vector<uint32_t> rootPids, std::function<RundownSnapshot()> rundown = {});
  // Not thread-safe against itself (one producer thread at a time; the next producer may start after the
  // previous one has returned). Never blocks on the decoder. Copies the event if the decoder understands it.
  void submit(const etlconvert::EtwEvent& event);
  // Same thread as submit(). The end of one ETW buffer: hands over what was copied since the last call.
  void endBuffer();
  // Thread-safe, idempotent. Decodes everything still queued (no hold-back, no lag drops), releases the
  // pending stacks, sends a last heartbeat and joins the thread.
  void finish();
  // Thread-safe.
  LiveEtwSourceStats stats() const;

 private:
  struct Chunk {
    std::vector<uint8_t> bytes;
    bool data = false;  // sample / switch / ready / stack events (droppable)
    uint64_t events = 0;
    std::chrono::steady_clock::time_point queuedAt;
  };
  struct Stage {
    std::vector<uint8_t> bytes;
    uint64_t events = 0;
  };
  void run();
  void process(std::vector<Chunk>& chunks, bool stopping);
  void stageEvent(Stage& stage, const etlconvert::EtwEvent& e);
  void pushStage(Stage& stage, bool data);

  LiveEtwSourceConfig m_config;
  Stage m_metaStage, m_dataStage;             // producer thread only
  std::unique_ptr<LiveEtwDecoder> m_decoder;  // created by arm(), used by the thread only
  std::function<RundownSnapshot()> m_rundown;
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<Chunk> m_queue;
  uint64_t m_dataBytes = 0, m_metaBytes = 0;
  uint64_t m_droppedPending = 0;
  std::atomic<uint64_t> m_ignored{0};
  LiveEtwSourceStats m_stats;
  bool m_armed = false;
  bool m_stop = false;
  std::thread m_thread;
};

}  // namespace tachyon::capture::etw
