#include "capture/windows_etw/etw_session.h"

#include "core/log.h"
#include "core/platform.h"

#include <cstring>
#include <system_error>
#include <utility>

#if TACHYON_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// Order matters: wmistr.h before evntrace.h before evntcons.h.
#include <evntcons.h>
#include <evntrace.h>
#include <wmistr.h>

#include <array>
#include <string>

namespace tachyon::capture::etw {

// The platform-independent copies of the flag values must match the SDK.
static_assert(kFlagProcess == EVENT_TRACE_FLAG_PROCESS);
static_assert(kFlagThread == EVENT_TRACE_FLAG_THREAD);
static_assert(kFlagImageLoad == EVENT_TRACE_FLAG_IMAGE_LOAD);
static_assert(kFlagCSwitch == EVENT_TRACE_FLAG_CSWITCH);
static_assert(kFlagDispatcher == EVENT_TRACE_FLAG_DISPATCHER);
static_assert(kFlagProfile == EVENT_TRACE_FLAG_PROFILE);
static_assert(sizeof(etlconvert::Guid) == sizeof(GUID));

namespace {

GUID toWinGuid(const etlconvert::Guid& g) {
  GUID out;
  std::memcpy(&out, &g, sizeof(out));
  return out;
}

std::string wideToUtf8(const wchar_t* w) {
  if (w == nullptr || *w == L'\0') return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<size_t>(n - 1));
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

// Owns the EVENT_TRACE_PROPERTIES block plus the trailing logger-name and log-file-name strings (ETW
// requires that space; without it StartTrace fails with ERROR_BAD_LENGTH / ERROR_INVALID_PARAMETER).
std::vector<uint64_t> buildProperties(const SessionConfig& cfg, const GUID& guid) {
  const std::wstring loggerName = utf8ToWide(cfg.name);
  const std::wstring fileName = cfg.etlPath.wstring();
  const size_t nameBytes = (loggerName.size() + 1) * sizeof(wchar_t);
  const size_t fileBytes = (fileName.size() + 1) * sizeof(wchar_t);
  const size_t total = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes + fileBytes;
  std::vector<uint64_t> storage((total + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
  auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
  p->Wnode.BufferSize = static_cast<ULONG>(total);
  p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  p->Wnode.ClientContext = 1;  // QPC timestamps
  p->Wnode.Guid = guid;
  p->BufferSize = cfg.buffers.bufferSizeKb;
  p->MinimumBuffers = cfg.buffers.minimumBuffers;
  p->MaximumBuffers = cfg.buffers.maximumBuffers;
  p->FlushTimer = cfg.buffers.flushTimerSeconds;
  p->MaximumFileSize = cfg.maxFileMb;  // MB; for circular mode this is the cap
  p->LogFileMode = (cfg.circular ? EVENT_TRACE_FILE_MODE_CIRCULAR : EVENT_TRACE_FILE_MODE_SEQUENTIAL) |
                   EVENT_TRACE_SYSTEM_LOGGER_MODE;
  p->EnableFlags = cfg.enableFlags;
  p->LoggerNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES));
  p->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + nameBytes);
  char* base = reinterpret_cast<char*>(storage.data());
  std::memcpy(base + p->LoggerNameOffset, loggerName.c_str(), nameBytes);
  std::memcpy(base + p->LogFileNameOffset, fileName.c_str(), fileBytes);
  return storage;
}

EVENT_TRACE_PROPERTIES* propsOf(std::vector<uint64_t>& storage) {
  return reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
}

SessionCounters countersOf(const EVENT_TRACE_PROPERTIES* p) {
  SessionCounters c;
  c.eventsLost = p->EventsLost;
  c.logBuffersLost = p->LogBuffersLost;
  c.realTimeBuffersLost = p->RealTimeBuffersLost;
  c.buffersWritten = p->BuffersWritten;
  c.numberOfBuffers = p->NumberOfBuffers;
  c.freeBuffers = p->FreeBuffers;
  c.bufferSizeKb = p->BufferSize;
  return c;
}

// Fresh copy of the properties for one ControlTrace call (the buffer is rewritten with the counters).
ULONG controlByHandle(uint64_t handle, const std::wstring& name, std::vector<uint64_t>& props, ULONG code) {
  return ControlTraceW(handle, name.c_str(), propsOf(props), code);
}

struct ListedSession {
  std::wstring wname;
  std::string name;
  std::vector<uint64_t> props;
};

Result<std::vector<ListedSession>> queryAll() {
  constexpr size_t kMaxSessions = 64;  // ETW supports at most 64 loggers
  constexpr size_t kNameChars = 1024;  // maximum logger / file name length
  const size_t bytes = sizeof(EVENT_TRACE_PROPERTIES) + 2 * kNameChars * sizeof(wchar_t);
  std::vector<std::vector<uint64_t>> storage(kMaxSessions);
  std::array<PEVENT_TRACE_PROPERTIES, kMaxSessions> ptrs{};
  for (size_t i = 0; i < kMaxSessions; ++i) {
    storage[i].assign((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
    EVENT_TRACE_PROPERTIES* p = propsOf(storage[i]);
    p->Wnode.BufferSize = static_cast<ULONG>(bytes);
    p->LoggerNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES));
    p->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES) + kNameChars * sizeof(wchar_t));
    ptrs[i] = p;
  }
  ULONG count = 0;
  const ULONG status = QueryAllTracesW(ptrs.data(), static_cast<ULONG>(kMaxSessions), &count);
  if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA)
    return winError(status == ERROR_ACCESS_DENIED ? ErrorCode::NoPrivilege : ErrorCode::IoError,
                    "QueryAllTraces", status);
  std::vector<ListedSession> out;
  for (size_t i = 0; i < count && i < kMaxSessions; ++i) {
    const EVENT_TRACE_PROPERTIES* p = ptrs[i];
    if (p->LoggerNameOffset == 0) continue;
    const auto* wname =
        reinterpret_cast<const wchar_t*>(reinterpret_cast<const char*>(p) + p->LoggerNameOffset);
    ListedSession s;
    s.wname = wname;
    s.name = wideToUtf8(wname);
    s.props = std::move(storage[i]);
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace

Result<void> EtwSession::enableProfilePrivilege() {
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

EtwSession::EtwSession(SessionConfig config, uint64_t handle, std::vector<uint64_t> properties)
    : m_config(std::move(config)), m_handle(handle), m_properties(std::move(properties)), m_running(true) {}

EtwSession::~EtwSession() {
  if (m_running) {
    auto r = stop();
    if (!r.ok())
      TACHYON_LOG_WARN("etw: stopping session '{}' in destructor: {}", m_config.name, r.error().toString());
  }
}

Result<std::unique_ptr<EtwSession>> EtwSession::start(const SessionConfig& config) {
  TACHYON_TRY(enableProfilePrivilege());

  std::error_code ec;
  if (config.etlPath.has_parent_path()) std::filesystem::create_directories(config.etlPath.parent_path(), ec);
  std::filesystem::remove(config.etlPath, ec);  // a stale file of an earlier run must not be appended to

  const std::wstring wname = utf8ToWide(config.name);
  GUID guid = toWinGuid(config.guid);
  std::vector<uint64_t> props = buildProperties(config, guid);
  TRACEHANDLE handle = 0;
  bool kernelGuidFallback = false;
  ULONG status = StartTraceW(&handle, wname.c_str(), propsOf(props));
  if (status == ERROR_INVALID_PARAMETER) {
    // Some Windows builds insist on the kernel provider GUID for kernel flags; retry with it.
    // SystemTraceControlGuid {9e814aad-3204-11d2-9a82-006008a86939}, spelled out to avoid needing INITGUID.
    kernelGuidFallback = true;
    guid = GUID{0x9e814aadu, 0x3204u, 0x11d2u, {0x9a, 0x82, 0x00, 0x60, 0x08, 0xa8, 0x69, 0x39}};
    props = buildProperties(config, guid);
    status = StartTraceW(&handle, wname.c_str(), propsOf(props));
  }
  if (status == ERROR_ACCESS_DENIED)
    return winError(ErrorCode::NoPrivilege, "StartTrace (run elevated)", status);
  if (status == ERROR_ALREADY_EXISTS)
    return makeError(ErrorCode::Busy, "ETW session '" + config.name + "' already exists");
  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "StartTrace", status);

  // From here on the destructor stops the session on every error return.
  std::unique_ptr<EtwSession> session(new EtwSession(config, handle, std::move(props)));
  session->m_startInfo.kernelGuidFallback = kernelGuidFallback;

  // Sampling interval. The handle must be 0 (system-wide setting): CI showed that the session handle is
  // rejected. The result is recorded in startInfo and logged.
  TRACE_PROFILE_INTERVAL interval{};
  interval.Source = 0;  // ProfileTime
  interval.Interval = config.profileIntervalUnits;
  status = TraceSetInformation(0, TraceSampledProfileIntervalInfo, &interval,
                               static_cast<ULONG>(sizeof(interval)));
  session->m_startInfo.intervalSetWithSessionHandle = false;
  TACHYON_LOG_INFO("etw: TraceSetInformation(SampledProfileInterval, handle 0, {} x 100 ns) -> {}",
                   config.profileIntervalUnits, status);
  if (status != ERROR_SUCCESS)
    return winError(ErrorCode::NoPrivilege, "TraceSetInformation(TraceSampledProfileIntervalInfo)", status);

  // Stack walking for the selected events (SampledProfile / CSwitch / ReadyThread).
  if (!config.stackEvents.empty()) {
    std::vector<CLASSIC_EVENT_ID> ids(config.stackEvents.size());
    for (size_t i = 0; i < ids.size(); ++i) {
      ids[i] = CLASSIC_EVENT_ID{};
      ids[i].EventGuid = toWinGuid(config.stackEvents[i].provider);
      ids[i].Type = config.stackEvents[i].opcode;
    }
    status = TraceSetInformation(handle, TraceStackTracingInfo, ids.data(),
                                 static_cast<ULONG>(ids.size() * sizeof(CLASSIC_EVENT_ID)));
    if (status != ERROR_SUCCESS)
      return winError(ErrorCode::IoError, "TraceSetInformation(TraceStackTracingInfo)", status);
    session->m_startInfo.stackTracingEnabled = true;
  }
  return session;
}

Result<void> EtwSession::flushBuffers() {
  if (!m_running) return makeError(ErrorCode::InvalidArgument, "ETW session is not running");
  const ULONG status =
      controlByHandle(m_handle, utf8ToWide(m_config.name), m_properties, EVENT_TRACE_CONTROL_FLUSH);
  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "ControlTrace(FLUSH)", status);
  return ok();
}

Result<SessionCounters> EtwSession::query() {
  if (!m_running) return makeError(ErrorCode::InvalidArgument, "ETW session is not running");
  const ULONG status =
      controlByHandle(m_handle, utf8ToWide(m_config.name), m_properties, EVENT_TRACE_CONTROL_QUERY);
  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "ControlTrace(QUERY)", status);
  return countersOf(propsOf(m_properties));
}

Result<SessionCounters> EtwSession::stop() {
  if (!m_running) return makeError(ErrorCode::InvalidArgument, "ETW session is not running");
  // ControlTrace(STOP) rewrites the properties with the final counters (EventsLost, LogBuffersLost, ...).
  const ULONG status =
      controlByHandle(m_handle, utf8ToWide(m_config.name), m_properties, EVENT_TRACE_CONTROL_STOP);
  if (status == ERROR_WMI_INSTANCE_NOT_FOUND) {
    m_running = false;
    return winError(ErrorCode::NotFound, "ControlTrace(STOP) (session already gone)", status);
  }
  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "ControlTrace(STOP)", status);
  m_running = false;
  return countersOf(propsOf(m_properties));
}

Result<std::vector<std::string>> EtwSession::listSessions() {
  TACHYON_TRY_ASSIGN(std::vector<ListedSession> all, queryAll());
  std::vector<std::string> names;
  for (const ListedSession& s : all) names.push_back(s.name);
  return names;
}

Result<OrphanReport> EtwSession::cleanupOrphans() {
  TACHYON_TRY_ASSIGN(std::vector<ListedSession> all, queryAll());
  OrphanReport report;
  report.inspected = all.size();
  for (ListedSession& s : all) {
    switch (classifySession(s.name, [](uint32_t pid) { return processAlive(pid); })) {
      case OrphanVerdict::NotTachyon:
      case OrphanVerdict::Unparseable: break;
      case OrphanVerdict::Alive: report.skippedAlive.push_back(s.name); break;
      case OrphanVerdict::Orphan: {
        const ULONG status = controlByHandle(0, s.wname, s.props, EVENT_TRACE_CONTROL_STOP);
        if (status == ERROR_SUCCESS) {
          TACHYON_LOG_INFO("etw: stopped orphaned session '{}'", s.name);
          report.stopped.push_back(s.name);
        } else {
          report.failed.push_back(s.name + ": Windows error " + std::to_string(status));
        }
        break;
      }
    }
  }
  return report;
}

bool EtwSession::processAlive(uint32_t pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;  // denied: it exists
  DWORD code = 0;
  const BOOL got = GetExitCodeProcess(h, &code);
  CloseHandle(h);
  return got == 0 || code == STILL_ACTIVE;
}

uint32_t EtwSession::logicalCoreCount() {
  const DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  return n == 0 ? 1u : static_cast<uint32_t>(n);
}

uint32_t EtwSession::currentPid() {
  return static_cast<uint32_t>(GetCurrentProcessId());
}

}  // namespace tachyon::capture::etw

#else  // !TACHYON_PLATFORM_WINDOWS

#include <unistd.h>

namespace tachyon::capture::etw {

namespace {
Error unsupported() {
  return makeError(ErrorCode::Unsupported, "ETW sessions require Windows");
}
}  // namespace

EtwSession::EtwSession(SessionConfig config, uint64_t handle, std::vector<uint64_t> properties)
    : m_config(std::move(config)), m_handle(handle), m_properties(std::move(properties)) {}

EtwSession::~EtwSession() = default;

Result<void> EtwSession::enableProfilePrivilege() {
  return unsupported();
}
Result<std::unique_ptr<EtwSession>> EtwSession::start(const SessionConfig&) {
  return unsupported();
}
Result<void> EtwSession::flushBuffers() {
  return unsupported();
}
Result<SessionCounters> EtwSession::query() {
  return unsupported();
}
Result<SessionCounters> EtwSession::stop() {
  return unsupported();
}
Result<std::vector<std::string>> EtwSession::listSessions() {
  return std::vector<std::string>{};
}
Result<OrphanReport> EtwSession::cleanupOrphans() {
  return OrphanReport{};
}
bool EtwSession::processAlive(uint32_t) {
  return true;
}
uint32_t EtwSession::logicalCoreCount() {
  return 1;
}
uint32_t EtwSession::currentPid() {
  return static_cast<uint32_t>(getpid());
}

}  // namespace tachyon::capture::etw

#endif
