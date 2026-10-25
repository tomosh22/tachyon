// Helper executable for the launch tests (WP-13): a small process tree on demand.
//
//   tachyon-test-spawner --dump FILE            write the working directory and environment to FILE
//                        --delay S              sleep S seconds before spawning (lets a capture attach first)
//                        --spawn PATH[;GRAND]   start PATH (repeatable) as `PATH --busy N [--spawn GRAND]`
//                        --busy S               spin on the CPU for S seconds (N above is the same S)
// Children are waited for before exiting. Exit code 0.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

void dump(const std::string& file) {
  std::FILE* f = nullptr;
  if (fopen_s(&f, file.c_str(), "wb") != 0 || f == nullptr) return;
  wchar_t cwd[MAX_PATH * 2] = {};
  GetCurrentDirectoryW(static_cast<DWORD>(sizeof(cwd) / sizeof(cwd[0])), cwd);
  char narrow[MAX_PATH * 4] = {};
  WideCharToMultiByte(CP_UTF8, 0, cwd, -1, narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr);
  std::fprintf(f, "CWD=%s\n", narrow);
  wchar_t* block = GetEnvironmentStringsW();
  for (const wchar_t* p = block; block != nullptr && *p != L'\0'; p += wcslen(p) + 1) {
    char line[8192] = {};
    WideCharToMultiByte(CP_UTF8, 0, p, -1, line, static_cast<int>(sizeof(line)), nullptr, nullptr);
    std::fprintf(f, "ENV:%s\n", line);
  }
  if (block != nullptr) FreeEnvironmentStringsW(block);
  std::fclose(f);
}

void busy(double seconds) {
  const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
  volatile uint64_t x = 0;
  while (GetTickCount64() < end)
    for (int i = 0; i < 20000; ++i) x = x + static_cast<uint64_t>(i);
}

HANDLE spawn(const std::string& spec, double busySeconds) {
  std::string path = spec, grand;
  if (const size_t semi = spec.find(';'); semi != std::string::npos) {
    path = spec.substr(0, semi);
    grand = spec.substr(semi + 1);
  }
  std::string cmd = "\"" + path + "\" --busy " + std::to_string(busySeconds);
  if (!grand.empty()) cmd += " --spawn \"" + grand + "\"";
  std::wstring wcmd = widen(cmd);
  wcmd.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    std::fprintf(stderr, "spawner: CreateProcess(%s) failed: %lu\n", path.c_str(), GetLastError());
    return nullptr;
  }
  CloseHandle(pi.hThread);
  return pi.hProcess;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> spawns;
  std::string dumpFile;
  double busySeconds = 0.0, delay = 0.0;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--dump" && i + 1 < argc) {
      dumpFile = argv[++i];
    } else if (a == "--delay" && i + 1 < argc) {
      delay = std::atof(argv[++i]);
    } else if (a == "--spawn" && i + 1 < argc) {
      spawns.emplace_back(argv[++i]);
    } else if (a == "--busy" && i + 1 < argc) {
      busySeconds = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "spawner: unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  if (!dumpFile.empty()) dump(dumpFile);
  if (delay > 0.0) Sleep(static_cast<DWORD>(delay * 1000.0));
  std::vector<HANDLE> children;
  for (const std::string& s : spawns)
    if (HANDLE h = spawn(s, busySeconds); h != nullptr) children.push_back(h);
  if (busySeconds > 0.0) busy(busySeconds);
  for (HANDLE h : children) {
    WaitForSingleObject(h, 60000);
    CloseHandle(h);
  }
  return 0;
}
