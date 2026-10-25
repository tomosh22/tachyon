#include "etw_platform.h"

#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// Order matters: wmistr.h before evntrace.h before evntcons.h.
#include <evntcons.h>
#include <evntrace.h>
#include <wmistr.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "session_config.h"

namespace tachyon::etlstat {

// The platform-independent copies of the flag values must match the SDK.
static_assert(kFlagProcess == EVENT_TRACE_FLAG_PROCESS);
static_assert(kFlagThread == EVENT_TRACE_FLAG_THREAD);
static_assert(kFlagImageLoad == EVENT_TRACE_FLAG_IMAGE_LOAD);
static_assert(kFlagCSwitch == EVENT_TRACE_FLAG_CSWITCH);
static_assert(kFlagDispatcher == EVENT_TRACE_FLAG_DISPATCHER);
static_assert(kFlagProfile == EVENT_TRACE_FLAG_PROFILE);
static_assert(sizeof(Guid) == sizeof(GUID));

namespace {

GUID toWinGuid(const Guid& g) {
  GUID out;
  std::memcpy(&out, &g, sizeof(out));
  return out;
}

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

Error winError(ErrorCode code, const std::string& what, unsigned long status) {
  return makeError(code, what + " failed with Windows error " + std::to_string(status));
}

// SeSystemProfilePrivilege is required to set the sampled-profile interval and to collect PROFILE events.
Result<void> enableProfilePrivilege() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
    return winError(ErrorCode::NoPrivilege, "OpenProcessToken", GetLastError());
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueW(nullptr, L"SeSystemProfilePrivilege", &tp.Privileges[0].Luid)) {
    const DWORD e = GetLastError();
    CloseHandle(token);
    return winError(ErrorCode::NoPrivilege, "LookupPrivilegeValue(SeSystemProfilePrivilege)", e);
  }
  AdjustTokenPrivileges(token, FALSE, &tp, static_cast<DWORD>(sizeof(tp)), nullptr, nullptr);
  const DWORD e = GetLastError();
  CloseHandle(token);
  if (e == ERROR_NOT_ALL_ASSIGNED) {
    return makeError(ErrorCode::NoPrivilege,
                     "SeSystemProfilePrivilege is not held by this token; run from an elevated prompt "
                     "(Administrators hold it by default)");
  }
  if (e != ERROR_SUCCESS) return winError(ErrorCode::NoPrivilege, "AdjustTokenPrivileges", e);
  return ok();
}

// Owns the EVENT_TRACE_PROPERTIES block plus the trailing logger-name and log-file-name strings.
class PropertiesBlock {
 public:
  PropertiesBlock(const std::wstring& loggerName, const std::wstring& fileName, const GUID& guid,
                  const BufferSettings& buffers) {
    const size_t nameBytes = (loggerName.size() + 1) * sizeof(wchar_t);
    const size_t fileBytes = (fileName.size() + 1) * sizeof(wchar_t);
    const size_t total = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes + fileBytes;
    m_storage.assign((total + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
    EVENT_TRACE_PROPERTIES* p = props();
    p->Wnode.BufferSize = static_cast<ULONG>(total);
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // QPC timestamps
    p->Wnode.Guid = guid;
    p->BufferSize = buffers.bufferSizeKb;
    p->MinimumBuffers = buffers.minimumBuffers;
    p->MaximumBuffers = buffers.maximumBuffers;
    p->FlushTimer = 1;
    p->LogFileMode = EVENT_TRACE_FILE_MODE_SEQUENTIAL | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    p->EnableFlags = kEnableFlags;
    p->LoggerNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES));
    p->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + nameBytes);
    std::memcpy(reinterpret_cast<char*>(p) + p->LoggerNameOffset, loggerName.c_str(), nameBytes);
    std::memcpy(reinterpret_cast<char*>(p) + p->LogFileNameOffset, fileName.c_str(), fileBytes);
  }

  EVENT_TRACE_PROPERTIES* props() { return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(m_storage.data()); }

 private:
  std::vector<uint64_t> m_storage;
};

}  // namespace

bool etwSupported() {
  return true;
}

Result<RecordSummary> recordEtl(const CliOptions& options) {
  RecordSummary summary;
  TACHYON_TRY(enableProfilePrivilege());

  summary.logicalCores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  const BufferSettings buffers = computeBufferSettings(summary.logicalCores);
  summary.profileIntervalUnits = profileIntervalUnits(options.hz);

  // A unique name plus EVENT_TRACE_SYSTEM_LOGGER_MODE (Windows 8+) gives a private system logger instead of
  // the single global "NT Kernel Logger", so we do not collide with WPA/xperf/PerfView or a second etlstat
  // (up to 8 system loggers can exist at once).
  const std::wstring name = L"Tachyon etlstat " + std::to_wstring(GetCurrentProcessId());
  const std::wstring file = utf8ToWide(options.outPath);

  // The session GUID is derived from the pid and time so it is unique per run.
  const auto stamp = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  GUID guid = {0x7ac5a1e0u, 0x1e71u, 0x4a7cu, {0x9b, 0x1d, 0x54, 0x61, 0x63, 0x68, 0x79, 0x6f}};
  guid.Data1 ^= GetCurrentProcessId();
  guid.Data2 = static_cast<unsigned short>(guid.Data2 ^ (stamp & 0xFFFF));

  PropertiesBlock block(name, file, guid, buffers);
  TRACEHANDLE session = 0;
  ULONG status = StartTraceW(&session, name.c_str(), block.props());
  if (status == ERROR_INVALID_PARAMETER) {
    // Some Windows builds insist on the kernel provider GUID for kernel flags; retry with it.
    summary.sessionGuidWasUnique = false;
    // SystemTraceControlGuid {9e814aad-3204-11d2-9a82-006008a86939}, spelled out to avoid needing INITGUID.
    const GUID kernelGuid = {0x9e814aadu, 0x3204u, 0x11d2u, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
    block = PropertiesBlock(name, file, kernelGuid, buffers);
    status = StartTraceW(&session, name.c_str(), block.props());
  }
  if (status == ERROR_ACCESS_DENIED)
    return winError(ErrorCode::NoPrivilege, "StartTrace (run elevated)", status);
  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "StartTrace", status);

  auto stopSession = [&]() {
    // ControlTrace(STOP) rewrites the properties with final counters (EventsLost, LogBuffersLost, ...).
    const ULONG stopStatus = ControlTraceW(session, name.c_str(), block.props(), EVENT_TRACE_CONTROL_STOP);
    if (stopStatus == ERROR_SUCCESS) {
      const EVENT_TRACE_PROPERTIES* p = block.props();
      summary.eventsLost = p->EventsLost;
      summary.logBuffersLost = p->LogBuffersLost;
      summary.realTimeBuffersLost = p->RealTimeBuffersLost;
      summary.buffersWritten = p->BuffersWritten;
      summary.numberOfBuffers = p->NumberOfBuffers;
    }
    return stopStatus;
  };

  // Sampling interval. The handle to pass is not documented clearly: try the session handle first and fall
  // back to 0 (system-wide); record which one worked so docs/etw-notes.md can state it.
  TRACE_PROFILE_INTERVAL interval{};
  interval.Source = 0;  // ProfileTime
  interval.Interval = summary.profileIntervalUnits;
  status = TraceSetInformation(session, TraceSampledProfileIntervalInfo, &interval,
                               static_cast<ULONG>(sizeof(interval)));
  summary.intervalSetWithSessionHandle = status == ERROR_SUCCESS;
  if (status != ERROR_SUCCESS) {
    status = TraceSetInformation(0, TraceSampledProfileIntervalInfo, &interval,
                                 static_cast<ULONG>(sizeof(interval)));
  }
  if (status != ERROR_SUCCESS) {
    stopSession();
    return winError(ErrorCode::NoPrivilege, "TraceSetInformation(TraceSampledProfileIntervalInfo)", status);
  }

  // Stack walking for SampledProfile (PerfInfo/46), CSwitch (Thread/36) and ReadyThread (Thread/50).
  CLASSIC_EVENT_ID ids[3] = {};
  ids[0].EventGuid = toWinGuid(guids::kPerfInfo);
  ids[0].Type = opcodes::kPerfInfoSampledProfile;
  ids[1].EventGuid = toWinGuid(guids::kThread);
  ids[1].Type = opcodes::kThreadCSwitch;
  ids[2].EventGuid = toWinGuid(guids::kThread);
  ids[2].Type = opcodes::kThreadReadyThread;
  status = TraceSetInformation(session, TraceStackTracingInfo, ids, static_cast<ULONG>(sizeof(ids)));
  summary.stackTracingEnabled = status == ERROR_SUCCESS;
  if (status != ERROR_SUCCESS) {
    stopSession();
    return winError(ErrorCode::IoError, "TraceSetInformation(TraceStackTracingInfo)", status);
  }

  const auto start = std::chrono::steady_clock::now();
  PROCESS_INFORMATION pi{};
  bool haveProcess = false;
  if (!options.exePath.empty()) {
    std::wstring cmd = L"\"" + utf8ToWide(options.exePath) + L"\"";
    if (!options.exeArgs.empty()) cmd += L" " + utf8ToWide(options.exeArgs);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &pi)) {
      const DWORD e = GetLastError();
      stopSession();
      return winError(ErrorCode::NotFound, "CreateProcess(" + options.exePath + ")", e);
    }
    haveProcess = true;
    summary.exePid = pi.dwProcessId;
  }

  const DWORD waitMs = options.seconds * 1000u;
  if (haveProcess) {
    const DWORD w = WaitForSingleObject(pi.hProcess, waitMs);
    if (w == WAIT_TIMEOUT) {
      summary.exeStillRunningAtStop = true;
      TerminateProcess(pi.hProcess, 0);
      WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    summary.exeExitCode = code;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    Sleep(waitMs);
  }
  summary.recordedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  status = stopSession();
  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "ControlTrace(STOP)", status);

  WIN32_FILE_ATTRIBUTE_DATA attrs{};
  if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attrs)) {
    summary.fileBytes = (static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
  }
  return summary;
}

namespace {

struct ConsumerContext {
  StatsAccumulator* accumulator = nullptr;
  HexDumpSelector* selector = nullptr;
  std::ostream* dump = nullptr;
};

void WINAPI onEventRecord(PEVENT_RECORD record) {
  auto* ctx = static_cast<ConsumerContext*>(record->UserContext);
  EtwEvent e;
  std::memcpy(&e.provider, &record->EventHeader.ProviderId, sizeof(GUID));
  e.opcode = record->EventHeader.EventDescriptor.Opcode;
  e.version = record->EventHeader.EventDescriptor.Version;
  e.timestamp = static_cast<uint64_t>(record->EventHeader.TimeStamp.QuadPart);
  e.threadId = record->EventHeader.ThreadId;
  e.processId = record->EventHeader.ProcessId;
  e.cpu = record->BufferContext.ProcessorIndex;
  e.payload =
      std::span<const std::byte>(static_cast<const std::byte*>(record->UserData), record->UserDataLength);
  ctx->accumulator->add(e);
  if (ctx->selector != nullptr && ctx->dump != nullptr && ctx->selector->select(e)) {
    *ctx->dump << formatHexEventLine(e) << '\n';
  }
}

}  // namespace

Result<void> consumeEtl(const std::string& path, StatsAccumulator& accumulator, TraceFileInfo& info,
                        HexDumpSelector* selector, std::ostream* dump) {
  const std::wstring wpath = utf8ToWide(path);
  ConsumerContext ctx{&accumulator, selector, dump};

  EVENT_TRACE_LOGFILEW logfile{};
  logfile.LogFileName = const_cast<LPWSTR>(wpath.c_str());
  logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
  logfile.EventRecordCallback = onEventRecord;
  logfile.Context = &ctx;

  TRACEHANDLE handle = OpenTraceW(&logfile);
  if (handle == INVALID_PROCESSTRACE_HANDLE) {
    return winError(ErrorCode::IoError, "OpenTrace(" + path + ")", GetLastError());
  }
  const ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);

  // The header counters are final once ProcessTrace has consumed the file.
  info.valid = true;
  info.perfFreq = static_cast<uint64_t>(logfile.LogfileHeader.PerfFreq.QuadPart);
  if (info.perfFreq == 0) info.perfFreq = 10'000'000;
  info.eventsLost = logfile.LogfileHeader.EventsLost;
  info.buffersLost = logfile.LogfileHeader.BuffersLost;
  info.buffersWritten = logfile.LogfileHeader.BuffersWritten;
  info.numberOfProcessors = logfile.LogfileHeader.NumberOfProcessors;
  info.pointerSize = logfile.LogfileHeader.PointerSize;
  CloseTrace(handle);

  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "ProcessTrace", status);
  return ok();
}

}  // namespace tachyon::etlstat

#else  // !TACHYON_PLATFORM_WINDOWS

namespace tachyon::etlstat {

bool etwSupported() {
  return false;
}

Result<RecordSummary> recordEtl(const CliOptions&) {
  return makeError(ErrorCode::Unsupported, "etlstat record requires Windows (ETW kernel logger)");
}

Result<void> consumeEtl(const std::string&, StatsAccumulator&, TraceFileInfo&, HexDumpSelector*,
                        std::ostream*) {
  return makeError(
      ErrorCode::Unsupported,
      "etlstat stats requires Windows (ETW); use 'etlstat replay <file.hexevents>' on this platform");
}

}  // namespace tachyon::etlstat

#endif
