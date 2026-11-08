// Forwards the instrumentation of one process to a live sink as it arrives (WP-65a, docs/specs/live.md).
//
// The agent's ingest spools the raw wire frames of an SDK client untouched; for a live capture it also hands
// every complete frame to a LiveForwarder, which resolves the string keys (StringDef frames) and calls the
// capture::ILiveSink: scope begins / ends, frame marks, markers, plots, thread names and dropped counts.
// Fiber events are not part of the live protocol and are ignored. Timestamps are taken as they are (the SDK
// stamps the trace clock; a stream in another clock domain is not converted live).
#pragma once

#include "capture/capture_backend.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tachyon::inst {

class LiveForwarder {
 public:
  // Thread-safe.
  LiveForwarder(uint32_t pid, std::shared_ptr<capture::ILiveSink> sink);

  // Thread-safe (the connections of one process share the string table, so frames are decoded under a lock;
  // the sink is called with it held). `frame` is a complete frame: FrameHeader followed by its payload.
  void onFrame(const uint8_t* frame, size_t bytes);

  // Thread-safe. Frames that did not decode (too short, unknown string key) so far.
  uint64_t malformedFrames() const;

 private:
  uint32_t m_pid;
  std::shared_ptr<capture::ILiveSink> m_sink;
  mutable std::mutex m_mutex;
  std::unordered_map<uint64_t, std::string> m_strings;
  size_t m_stringBytes = 0;
  uint64_t m_malformed = 0;
};

}  // namespace tachyon::inst
