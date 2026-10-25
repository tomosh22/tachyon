// Windows implementation of TargetProcess (CreateProcessW suspended, job object, exit watching) and of the
// ETL process-event scan.
#include "capture/windows_etw/launch.h"
#include "capture/windows_etw/process_scan.h"
#include "core/log.h"
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// Order matters: wmistr.h before evntrace.h before evntcons.h.
#include <evntcons.h>
#include <evntrace.h>
#include <wmistr.h>

#include <cstring>
#include <filesystem>

namespace tachyon::capture::etw {

namespace {

std::wstring utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string wideToUtf8(const wchar_t* w, size_t len) {
  if (len == 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), out.data(), n, nullptr, nullptr);
  return out;
}

Error winError(ErrorCode code, const std::string& what, DWORD status) {
  return makeError(code, what + " failed with Windows error " + std::to_string(status));
}

ErrorCode launchErrorCode(DWORD status) {
  switch (status) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_DIRECTORY: return ErrorCode::NotFound;
    case ERROR_ACCESS_DENIED: return ErrorCode::NoPrivilege;
    default: return ErrorCode::IoError;
  }
}

// The agent's environment as name/value pairs (the hidden "=C:" variables keep their leading '=').
std::vector<EnvVar> currentEnvironment() {
  std::vector<EnvVar> vars;
  wchar_t* block = GetEnvironmentStringsW();
  if (block == nullptr) return vars;
  for (const wchar_t* p = block; *p != L'\0';) {
    const size_t len = wcslen(p);
    const wchar_t* eq = (*p == L'=') ? wcschr(p + 1, L'=') : wcschr(p, L'=');
    if (eq != nullptr) {
      vars.emplace_back(wideToUtf8(p, static_cast<size_t>(eq - p)),
                        wideToUtf8(eq + 1, len - static_cast<size_t>(eq - p) - 1));
    }
    p += len + 1;
  }
  FreeEnvironmentStringsW(block);
  return vars;
}

std::wstring environmentBlock(const std::vector<EnvVar>& vars) {
  std::wstring block;
  for (const EnvVar& v : vars) {
    block += utf8ToWide(v.first);
    block.push_back(L'=');
    block += utf8ToWide(v.second);
    block.push_back(L'\0');
  }
  if (block.empty()) block.push_back(L'\0');
  block.push_back(L'\0');
  return block;
}

}  // namespace

struct TargetProcess::Impl {
  HANDLE process = nullptr;
  HANDLE thread = nullptr;  // main thread while suspended
  HANDLE job = nullptr;
  uint32_t pid = 0;

  ~Impl() {
    if (thread != nullptr) CloseHandle(thread);
    if (process != nullptr) CloseHandle(process);
    if (job != nullptr) CloseHandle(job);
  }
};

TargetProcess::TargetProcess(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
TargetProcess::~TargetProcess() = default;
TargetProcess::TargetProcess(TargetProcess&&) noexcept = default;
TargetProcess& TargetProcess::operator=(TargetProcess&&) noexcept = default;

Result<TargetProcess> TargetProcess::launchSuspended(const LaunchSpec& spec) {
  if (spec.exe.empty()) return makeError(ErrorCode::InvalidArgument, "launch needs an executable");
  std::wstring cmd = utf8ToWide(buildCommandLine(spec.exe, spec.args));
  cmd.push_back(L'\0');  // CreateProcessW may modify the buffer
  std::wstring env = environmentBlock(mergeEnvironment(currentEnvironment(), spec.env));
  const std::wstring cwd = utf8ToWide(spec.cwd);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  // An agent without a console (started detached, as a service or from a test harness) would make every
  // console target pop up its own window: give such targets a hidden console instead.
  DWORD flags = CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT;
  if (GetConsoleWindow() == nullptr) flags |= CREATE_NO_WINDOW;
  // Only the command line is given: CreateProcess then searches the usual places (application directory,
  // current directory, system directories, PATH) for a bare name and appends ".exe".
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags, env.data(),
                      cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    const DWORD e = GetLastError();
    return winError(launchErrorCode(e), "CreateProcess(" + spec.exe + ")", e);
  }
  auto impl = std::make_unique<Impl>();
  impl->process = pi.hProcess;
  impl->thread = pi.hThread;
  impl->pid = pi.dwProcessId;

  // A job object lets us see when the whole tree is gone. Failure is not fatal (the process may already
  // belong to a job that forbids nesting): then only the root is watched.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job != nullptr && AssignProcessToJobObject(job, impl->process)) {
    impl->job = job;
  } else {
    TACHYON_LOG_WARN("etw: cannot place pid {} in a job object (error {}); only the root is watched",
                     impl->pid, GetLastError());
    if (job != nullptr) CloseHandle(job);
  }
  return TargetProcess(std::move(impl));
}

Result<TargetProcess> TargetProcess::open(uint32_t pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (h == nullptr) {
    const DWORD e = GetLastError();
    // ERROR_INVALID_PARAMETER is what OpenProcess reports for a pid that no longer exists.
    return winError(e == ERROR_INVALID_PARAMETER ? ErrorCode::NotFound : launchErrorCode(e),
                    "OpenProcess(" + std::to_string(pid) + ")", e);
  }
  auto impl = std::make_unique<Impl>();
  impl->process = h;
  impl->pid = pid;
  return TargetProcess(std::move(impl));
}

uint32_t TargetProcess::pid() const {
  return m_impl ? m_impl->pid : 0;
}

Result<void> TargetProcess::resume() {
  if (!m_impl || m_impl->thread == nullptr)
    return makeError(ErrorCode::InvalidArgument, "the process is not suspended");
  const DWORD r = ResumeThread(m_impl->thread);
  if (r == static_cast<DWORD>(-1)) return winError(ErrorCode::IoError, "ResumeThread", GetLastError());
  CloseHandle(m_impl->thread);
  m_impl->thread = nullptr;
  return ok();
}

bool TargetProcess::exited() const {
  if (!m_impl || m_impl->process == nullptr) return true;
  return WaitForSingleObject(m_impl->process, 0) != WAIT_TIMEOUT;
}

bool TargetProcess::allExited() const {
  if (!exited()) return false;
  if (!m_impl || m_impl->job == nullptr) return true;
  JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info{};
  if (!QueryInformationJobObject(m_impl->job, JobObjectBasicAccountingInformation, &info, sizeof(info),
                                 nullptr))
    return true;  // cannot tell: fall back to the root
  return info.ActiveProcesses == 0;
}

bool TargetProcess::tracksDescendants() const {
  return m_impl && m_impl->job != nullptr;
}

void TargetProcess::terminate() {
  if (m_impl && m_impl->process != nullptr) TerminateProcess(m_impl->process, 1);
}

// ---------------------------------------------------------------------------------------------------------
// ETL process-event scan

namespace {

void WINAPI onProcessRecord(PEVENT_RECORD record) {
  static_assert(sizeof(etlconvert::Guid) == sizeof(GUID));
  if (std::memcmp(&record->EventHeader.ProviderId, &etlconvert::guids::kProcess, sizeof(GUID)) != 0) return;
  auto* tracker = static_cast<ProcessTreeTracker*>(record->UserContext);
  etlconvert::EtwEvent e;
  e.provider = etlconvert::guids::kProcess;
  e.opcode = record->EventHeader.EventDescriptor.Opcode;
  e.version = record->EventHeader.EventDescriptor.Version;
  e.timestamp = static_cast<uint64_t>(record->EventHeader.TimeStamp.QuadPart);
  e.threadId = record->EventHeader.ThreadId;
  e.processId = record->EventHeader.ProcessId;
  e.payload = etlconvert::Bytes(static_cast<const std::byte*>(record->UserData), record->UserDataLength);
  tracker->onEvent(e);
}

}  // namespace

Result<void> scanEtlProcessEvents(const std::filesystem::path& etl, ProcessTreeTracker& tracker) {
  std::wstring path = etl.wstring();
  EVENT_TRACE_LOGFILEW logfile{};
  logfile.LogFileName = path.data();
  logfile.ProcessTraceMode = PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
  logfile.EventRecordCallback = onProcessRecord;
  logfile.Context = &tracker;
  TRACEHANDLE handle = OpenTraceW(&logfile);
  if (handle == INVALID_PROCESSTRACE_HANDLE) {
    const DWORD e = GetLastError();
    return winError(
        e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ErrorCode::NotFound : ErrorCode::IoError,
        "OpenTrace(" + etl.string() + ")", e);
  }
  const ULONG status = ProcessTrace(&handle, 1, nullptr, nullptr);
  CloseTrace(handle);
  if (status != ERROR_SUCCESS) return winError(ErrorCode::IoError, "ProcessTrace", status);
  return ok();
}

}  // namespace tachyon::capture::etw

#else  // !TACHYON_PLATFORM_WINDOWS

namespace tachyon::capture::etw {

Result<void> scanEtlProcessEvents(const std::filesystem::path&, ProcessTreeTracker&) {
  return makeError(ErrorCode::Unsupported, "scanning an ETL needs Windows");
}

}  // namespace tachyon::capture::etw

#endif
