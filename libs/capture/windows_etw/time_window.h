// "Keep only the last N seconds" conversion for flush (WP-11).
//
// libs/etlconvert has no time-window option (proposal in the WP-11 report: ConvertOptions::minTimestamp /
// windowTicks). Until it does, the flush path drives ConversionPipeline itself: the metadata pass sees the
// whole file (process, thread and image tables stay complete and the newest timestamp is found), the main
// pass only receives events newer than `newest - window`. The replay of the file is abstracted so Linux tests
// drive the same code from .hexevents fixtures; the Windows replay is ProcessTrace over the stopped ETL
// segment.
#pragma once

#include "core/result.h"
#include "etlconvert/etl_converter.h"

#include <cstdint>
#include <filesystem>
#include <functional>

namespace tachyon::capture::etw {

// Thread-safe: pure function. Start of the kept window: maxTimestamp - windowTicks, saturating at 0.
// windowTicks == 0 keeps everything.
uint64_t windowStartTimestamp(uint64_t maxTimestamp, uint64_t windowTicks);

// Thread-safe: pure function. The time a window filter judges an event by: the header timestamp, except for
// StackWalk Stack events (opcode 32), which are judged by the EventTimeStamp of the parent they belong to
// (first 8 payload bytes) so a stack is kept exactly when its sample / switch is.
uint64_t windowEventTime(const etlconvert::EtwEvent& event);

// Forwards events whose windowEventTime() >= minTimestamp. Not thread-safe.
class TimeWindowSink final : public etlconvert::EventSink {
 public:
  TimeWindowSink(etlconvert::EventSink& inner, uint64_t minTimestamp) : m_inner(inner), m_min(minTimestamp) {}
  void onEvent(const etlconvert::EtwEvent& event) override;
  uint64_t forwarded() const { return m_forwarded; }
  uint64_t dropped() const { return m_dropped; }

 private:
  etlconvert::EventSink& m_inner;
  uint64_t m_min;
  uint64_t m_forwarded = 0;
  uint64_t m_dropped = 0;
};

// Replays every event of the source once, in order, into the sink. Called twice by convertWindowed().
using ReplayFn = std::function<Result<void>(etlconvert::EventSink&)>;

// Not thread-safe per call. Two passes over `replay`: Metadata (all events) then Main (events inside the
// window). `info.clockFrequencyHz` converts nothing here; `windowTicks` is already in clock ticks.
Result<etlconvert::ConvertStats> convertWindowed(const std::filesystem::path& out,
                                                 etlconvert::ConvertOptions options,
                                                 const etlconvert::EtlFileInfo& info, uint64_t windowTicks,
                                                 const ReplayFn& replay);

// Not thread-safe per call. Windows: converts the last `seconds` of the ETL at `etl` (all of it when seconds
// is 0). Elsewhere: Unsupported.
Result<etlconvert::ConvertStats> convertEtlLastSeconds(const std::filesystem::path& etl,
                                                       const std::filesystem::path& out,
                                                       etlconvert::ConvertOptions options, uint32_t seconds);

}  // namespace tachyon::capture::etw
