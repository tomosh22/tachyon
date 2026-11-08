#include "capture/windows_etw/rundown.h"

#include "core/log.h"
#include "core/platform.h"

#if TACHYON_PLATFORM_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// tlhelp32.h must come after windows.h.
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace tachyon::capture::etw {

namespace {

std::string wideToUtf8(const wchar_t* w) {
  if (w == nullptr || *w == L'\0') return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
  out.resize(static_cast<size_t>(n - 1));
  return out;
}

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

GetThreadDescriptionFn threadDescriptionApi() {
  static const GetThreadDescriptionFn fn = [] {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    return k != nullptr ? reinterpret_cast<GetThreadDescriptionFn>(
                              reinterpret_cast<void*>(GetProcAddress(k, "GetThreadDescription")))
                        : nullptr;
  }();
  return fn;
}

std::string threadName(GetThreadDescriptionFn api, uint32_t tid) {
  if (api == nullptr) return {};
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (h == nullptr) return {};
  PWSTR text = nullptr;
  std::string name;
  if (SUCCEEDED(api(h, &text)) && text != nullptr) name = wideToUtf8(text);
  if (text != nullptr) LocalFree(text);
  CloseHandle(h);
  return name;
}

void collectModules(uint32_t pid, std::vector<RundownModule>& out) {
  HANDLE snap = INVALID_HANDLE_VALUE;
  // ERROR_BAD_LENGTH: the module list changed while it was copied; try again.
  for (int attempt = 0; attempt < 3; ++attempt) {
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
  }
  if (snap == INVALID_HANDLE_VALUE) return;  // protected or not yet initialised: skipped
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  if (Module32FirstW(snap, &me)) {
    do {
      RundownModule m;
      m.pid = pid;
      m.base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(me.modBaseAddr));
      m.size = me.modBaseSize;
      m.path = wideToUtf8(me.szExePath);
      if (m.base != 0 && !m.path.empty()) out.push_back(std::move(m));
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
}

}  // namespace

Result<RundownSnapshot> captureRundown(const std::vector<uint32_t>& detailPids) {
  RundownSnapshot out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS | TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return makeError(ErrorCode::IoError,
                     "CreateToolhelp32Snapshot failed with Windows error " + std::to_string(GetLastError()));
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      RundownProcess p;
      p.pid = pe.th32ProcessID;
      p.ppid = pe.th32ParentProcessID;
      p.name = wideToUtf8(pe.szExeFile);
      if (p.pid != 0) out.processes.push_back(std::move(p));
    } while (Process32NextW(snap, &pe));
  }
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32ThreadID == 0 || te.th32OwnerProcessID == 0) continue;
      RundownThread t;
      t.tid = te.th32ThreadID;
      t.pid = te.th32OwnerProcessID;
      out.threads.push_back(std::move(t));
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);

  const std::unordered_set<uint32_t> detail(detailPids.begin(), detailPids.end());
  const bool everyone = detailPids.empty();
  for (const RundownProcess& p : out.processes)
    if (everyone || detail.count(p.pid) != 0) collectModules(p.pid, out.modules);
  if (!everyone) {
    const GetThreadDescriptionFn api = threadDescriptionApi();
    for (RundownThread& t : out.threads)
      if (detail.count(t.pid) != 0) t.name = threadName(api, t.tid);
  }
  return out;
}

}  // namespace tachyon::capture::etw

#else  // !TACHYON_PLATFORM_WINDOWS

namespace tachyon::capture::etw {

Result<RundownSnapshot> captureRundown(const std::vector<uint32_t>&) {
  return makeError(ErrorCode::Unsupported, "the process rundown requires Windows");
}

}  // namespace tachyon::capture::etw

#endif
