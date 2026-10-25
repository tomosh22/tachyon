#include "capture/windows_etw/time_window.h"

#include "core/platform.h"

#include <algorithm>
#include <cstring>

#if TACHYON_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// Order matters: wmistr.h before evntrace.h before evntcons.h.
#include <evntcons.h>
#include <evntrace.h>
#include <wmistr.h>

#include <string>
#endif

namespace tachyon::capture::etw {

using etlconvert::ConvertOptions;
using etlconvert::ConvertStats;
using etlconvert::EtlFileInfo;
using etlconvert::EtwEvent;
using etlconvert::EventSink;
using etlconvert::PipelinePass;

uint64_t windowStartTimestamp(uint64_t maxTimestamp, uint64_t windowTicks) {
  if (windowTicks == 0 || maxTimestamp <= windowTicks) return 0;
  return maxTimestamp - windowTicks;
}

uint64_t windowEventTime(const EtwEvent& e) {
  if (e.provider == etlconvert::guids::kStackWalk && e.opcode == etlconvert::opcodes::kStackWalkStack &&
      e.payload.size() >= sizeof(uint64_t)) {
    uint64_t parent = 0;
    std::memcpy(&parent, e.payload.data(), sizeof(parent));
    if (parent != 0) return parent;
  }
  return e.timestamp;
}

void TimeWindowSink::onEvent(const EtwEvent& event) {
  if (windowEventTime(event) < m_min) {
    ++m_dropped;
    return;
  }
  ++m_forwarded;
  m_inner.onEvent(event);
}

namespace {

// Forwards everything and remembers the newest header timestamp.
class MaxTimestampSink final : public EventSink {
 public:
  explicit MaxTimestampSink(EventSink& inner) : m_inner(inner) {}
  void onEvent(const EtwEvent& event) override {
    m_max = std::max(m_max, event.timestamp);
    m_inner.onEvent(event);
  }
  uint64_t maxTimestamp() const { return m_max; }

 private:
  EventSink& m_inner;
  uint64_t m_max = 0;
};

}  // namespace

Result<ConvertStats> convertWindowed(const std::filesystem::path& out, ConvertOptions options,
                                     const EtlFileInfo& info, uint64_t windowTicks, const ReplayFn& replay) {
  TACHYON_TRY_ASSIGN(std::unique_ptr<etlconvert::ConversionPipeline> pipeline,
                     etlconvert::ConversionPipeline::create(out, std::move(options), info));
  pipeline->beginPass(PipelinePass::Metadata);
  MaxTimestampSink tracker(*pipeline);
  TACHYON_TRY(replay(tracker));

  const uint64_t cutoff = windowStartTimestamp(tracker.maxTimestamp(), windowTicks);
  pipeline->beginPass(PipelinePass::Main);
  TimeWindowSink window(*pipeline, cutoff);
  TACHYON_TRY(replay(window));
  pipeline->setFileInfo(info);
  return pipeline->finish();
}

#if TACHYON_PLATFORM_WINDOWS

namespace {

std::string errorText(const char* what, ULONG status) {
  return std::string(what) + " failed with Windows error " + std::to_string(status);
}

void WINAPI onEventRecord(PEVENT_RECORD record) {
  auto* sink = static_cast<EventSink*>(record->UserContext);
  EtwEvent e;
  std::memcpy(&e.provider, &record->EventHeader.ProviderId, sizeof(GUID));
  e.opcode = record->EventHeader.EventDescriptor.Opcode;
  e.version = record->EventHeader.EventDescriptor.Version;
  e.timestamp = static_cast<uint64_t>(record->EventHeader.TimeStamp.QuadPart);
  e.threadId = record->EventHeader.ThreadId;
  e.processId = record->EventHeader.ProcessId;
  e.cpu = record->BufferContext.ProcessorIndex;
  e.payload = etlconvert::Bytes(static_cast<const std::byte*>(record->UserData), record->UserDataLength);
  sink->onEvent(e);
}

// Opens the ETL; with `sink == nullptr` only the logfile header is read.
Result<void> runPass(const std::wstring& path, EventSink* sink, EtlFileInfo& info) {
  EVENT_TRACE_LOGFILEW logfile{};
  logfile.LogFileName = const_cast<LPWSTR>(path.c_str());
  logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
  logfile.EventRecordCallback = onEventRecord;
  logfile.Context = sink;

  TRACEHANDLE handle = OpenTraceW(&logfile);
  if (handle == INVALID_PROCESSTRACE_HANDLE) {
    const ULONG e = GetLastError();
    return makeError(
        e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ErrorCode::NotFound : ErrorCode::IoError,
        errorText("OpenTrace", e));
  }
  const TRACE_LOGFILE_HEADER& h = logfile.LogfileHeader;
  const uint64_t perfFreq = static_cast<uint64_t>(h.PerfFreq.QuadPart);
  info.clockFrequencyHz = h.ReservedFlags == 2 ? 10'000'000 : (perfFreq != 0 ? perfFreq : 10'000'000);
  info.numberOfProcessors = h.NumberOfProcessors;
  info.pointerSize = h.PointerSize;
  ULONG status = ERROR_SUCCESS;
  if (sink != nullptr) status = ProcessTrace(&handle, 1, nullptr, nullptr);
  info.eventsLost = h.EventsLost;
  info.buffersLost = h.BuffersLost;
  info.buffersWritten = h.BuffersWritten;
  CloseTrace(handle);
  if (status != ERROR_SUCCESS) return makeError(ErrorCode::IoError, errorText("ProcessTrace", status));
  return ok();
}

}  // namespace

Result<ConvertStats> convertEtlLastSeconds(const std::filesystem::path& etl, const std::filesystem::path& out,
                                           ConvertOptions options, uint32_t seconds) {
  if (seconds == 0) return etlconvert::EtlConverter::convert(etl, out, options);
  const std::wstring wpath = etl.wstring();
  EtlFileInfo info;
  TACHYON_TRY(runPass(wpath, nullptr, info));
  const uint64_t windowTicks = info.clockFrequencyHz * seconds;
  ReplayFn replay = [&](EventSink& sink) { return runPass(wpath, &sink, info); };
  return convertWindowed(out, std::move(options), info, windowTicks, replay);
}

#else

Result<ConvertStats> convertEtlLastSeconds(const std::filesystem::path&, const std::filesystem::path&,
                                           ConvertOptions, uint32_t) {
  return makeError(ErrorCode::Unsupported, "converting an ETL requires Windows");
}

#endif

}  // namespace tachyon::capture::etw
