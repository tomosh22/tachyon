#include "process_util.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace tachyon::agent::testing_util {

std::string readTextFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

#if TACHYON_PLATFORM_WINDOWS

namespace {
std::string quote(const std::string& s) {
  if (!s.empty() && s.find_first_of(" \t\"") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '\\';
    out += c;
  }
  return out + "\"";
}
}  // namespace

Process Process::spawn(const std::string& exe, const std::vector<std::string>& args,
                       const std::filesystem::path& outFile) {
  std::string cmd = quote(exe);
  for (const auto& a : args) cmd += " " + quote(a);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  HANDLE out = INVALID_HANDLE_VALUE;
  BOOL inherit = FALSE;
  if (!outFile.empty()) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    out = CreateFileA(outFile.string().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return Process();
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError = out;
    inherit = TRUE;
  }
  const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, inherit, CREATE_NEW_PROCESS_GROUP,
                                 nullptr, nullptr, &si, &pi);
  if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
  if (!ok) return Process();
  CloseHandle(pi.hThread);
  Process p;
  p.m_handle = pi.hProcess;
  p.m_pid = pi.dwProcessId;
  return p;
}

bool Process::valid() const {
  return m_handle != nullptr;
}

bool Process::waitFor(int timeoutMs, int* exitCode) {
  if (m_handle == nullptr) return false;
  if (WaitForSingleObject(m_handle, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) return false;
  DWORD code = 0;
  GetExitCodeProcess(m_handle, &code);
  if (exitCode != nullptr) *exitCode = static_cast<int>(code);
  return true;
}

void Process::requestTerminate() {
  if (m_handle != nullptr) GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, m_pid);
}

void Process::kill() {
  if (m_handle == nullptr) return;
  TerminateProcess(m_handle, 1);
  WaitForSingleObject(m_handle, 5000);
  reset();
}

void Process::reset() {
  if (m_handle != nullptr) CloseHandle(m_handle);
  m_handle = nullptr;
  m_pid = 0;
}

Process::Process(Process&& other) noexcept : m_handle(other.m_handle), m_pid(other.m_pid) {
  other.m_handle = nullptr;
  other.m_pid = 0;
}

Process& Process::operator=(Process&& other) noexcept {
  if (this != &other) {
    kill();
    m_handle = other.m_handle;
    m_pid = other.m_pid;
    other.m_handle = nullptr;
    other.m_pid = 0;
  }
  return *this;
}

Process::~Process() {
  kill();
}

#else

Process Process::spawn(const std::string& exe, const std::vector<std::string>& args,
                       const std::filesystem::path& outFile) {
  std::vector<std::string> argvStore;
  argvStore.push_back(exe);
  argvStore.insert(argvStore.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& s : argvStore) argv.push_back(s.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  if (!outFile.empty()) {
    posix_spawn_file_actions_addopen(&actions, 1, outFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, 1, 2);
  }
  pid_t pid = -1;
  const int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  Process p;
  if (rc == 0) p.m_pid = pid;
  return p;
}

bool Process::valid() const {
  return m_pid > 0;
}

bool Process::waitFor(int timeoutMs, int* exitCode) {
  if (m_pid <= 0) return false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    int status = 0;
    const pid_t r = waitpid(m_pid, &status, WNOHANG);
    if (r == m_pid) {
      m_reaped = true;
      if (exitCode != nullptr) *exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      return true;
    }
    if (r < 0) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void Process::requestTerminate() {
  if (m_pid > 0 && !m_reaped) ::kill(m_pid, SIGTERM);
}

void Process::kill() {
  if (m_pid > 0 && !m_reaped) {
    ::kill(m_pid, SIGKILL);
    int status = 0;
    waitpid(m_pid, &status, 0);
  }
  reset();
}

void Process::reset() {
  m_pid = -1;
  m_reaped = false;
}

Process::Process(Process&& other) noexcept : m_pid(other.m_pid), m_reaped(other.m_reaped) {
  other.m_pid = -1;
  other.m_reaped = false;
}

Process& Process::operator=(Process&& other) noexcept {
  if (this != &other) {
    kill();
    m_pid = other.m_pid;
    m_reaped = other.m_reaped;
    other.m_pid = -1;
    other.m_reaped = false;
  }
  return *this;
}

Process::~Process() {
  kill();
}

#endif

int runToCompletion(const std::string& exe, const std::vector<std::string>& args,
                    const std::filesystem::path& outFile, int timeoutMs) {
  Process p = Process::spawn(exe, args, outFile);
  if (!p.valid()) return -1;
  int code = -1;
  if (!p.waitFor(timeoutMs, &code)) return -1;
  return code;
}

}  // namespace tachyon::agent::testing_util
