// The only advapi32 user of the library: OpenTrace / ProcessTrace over an ETL file feeding a
// ConversionPipeline.
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#include "etlconvert/etl_driver.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// Order matters: wmistr.h before evntrace.h before evntcons.h.
#include <evntcons.h>
#include <evntrace.h>
#include <wmistr.h>

#include <cstring>
#include <string>

namespace tachyon::etlconvert::detail {

static_assert(sizeof(Guid) == sizeof(GUID));

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
  e.payload = Bytes(static_cast<const std::byte*>(record->UserData), record->UserDataLength);
  sink->onEvent(e);
}

// Opens the ETL for one pass. With `sink == nullptr` only the logfile header is read.
Result<void> runPass(const std::wstring& path, EventSink* sink, EtlFileInfo& info,
                     std::vector<std::string>& notes) {
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
  // Timestamps are raw ticks of the session clock (ReservedFlags: 1 QPC, 2 system time, 3 CPU cycles).
  const TRACE_LOGFILE_HEADER& h = logfile.LogfileHeader;
  const uint64_t perfFreq = static_cast<uint64_t>(h.PerfFreq.QuadPart);
  const uint32_t clockType = h.ReservedFlags;
  if (clockType == 2) {
    info.clockFrequencyHz = 10'000'000;  // 100 ns system time ticks
    if (sink == nullptr)
      notes.push_back(
          "ETL uses the system-time clock (100 ns); timestamps will not line up with QPC instrumentation");
  } else {
    info.clockFrequencyHz = perfFreq != 0 ? perfFreq : 10'000'000;
    if (clockType == 3 && sink == nullptr)
      notes.push_back(
          "ETL uses the CPU cycle counter clock; clock_frequency_hz is the QPC frequency and may be wrong");
  }
  info.numberOfProcessors = h.NumberOfProcessors;
  info.pointerSize = h.PointerSize;

  ULONG status = ERROR_SUCCESS;
  if (sink != nullptr) status = ProcessTrace(&handle, 1, nullptr, nullptr);
  // The header counters are final once ProcessTrace has consumed the file.
  info.eventsLost = h.EventsLost;
  info.buffersLost = h.BuffersLost;
  info.buffersWritten = h.BuffersWritten;
  CloseTrace(handle);
  if (status != ERROR_SUCCESS) return makeError(ErrorCode::IoError, errorText("ProcessTrace", status));
  return ok();
}

}  // namespace

Result<ConvertStats> convertEtlFile(const std::filesystem::path& etl, const std::filesystem::path& out,
                                    const ConvertOptions& options) {
  const std::wstring wpath = etl.wstring();
  EtlFileInfo info;
  std::vector<std::string> notes;
  TACHYON_TRY(runPass(wpath, nullptr, info, notes));

  TACHYON_TRY_ASSIGN(std::unique_ptr<ConversionPipeline> pipeline,
                     ConversionPipeline::create(out, options, info));
  if (options.twoPass) {
    pipeline->beginPass(PipelinePass::Metadata);
    TACHYON_TRY(runPass(wpath, pipeline.get(), info, notes));
  }
  pipeline->beginPass(PipelinePass::Main);
  TACHYON_TRY(runPass(wpath, pipeline.get(), info, notes));
  pipeline->setFileInfo(info);
  TACHYON_TRY_ASSIGN(ConvertStats stats, pipeline->finish());
  for (std::string& n : notes) stats.warnings.push_back(std::move(n));
  return stats;
}

}  // namespace tachyon::etlconvert::detail

#endif  // TACHYON_PLATFORM_WINDOWS
