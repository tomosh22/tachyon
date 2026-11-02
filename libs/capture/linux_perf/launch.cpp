#include "capture/linux_perf/launch.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

extern char** environ;

namespace tachyon::capture::perf {

std::vector<std::string> mergeEnvironment(const std::vector<std::string>& parent,
                                          const std::map<std::string, std::string>& overrides) {
  std::vector<std::string> out = parent;
  for (const auto& [name, value] : overrides) {
    const std::string prefix = name + "=";
    bool replaced = false;
    for (std::string& entry : out) {
      if (entry.compare(0, prefix.size(), prefix) == 0) {
        entry = prefix + value;
        replaced = true;
        break;
      }
    }
    if (!replaced) out.push_back(prefix + value);
  }
  return out;
}

Result<std::string> resolveExecutable(const std::string& exe, const std::string& pathVar,
                                      const std::string& cwd) {
  if (exe.empty()) return makeError(ErrorCode::InvalidArgument, "no executable given");
  auto usable = [&](const std::string& candidate) {
    std::string full = candidate;
    if (!candidate.empty() && candidate[0] != '/' && !cwd.empty()) full = cwd + "/" + candidate;
    struct stat st {};
    return ::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(full.c_str(), X_OK) == 0;
  };
  if (exe.find('/') != std::string::npos) {
    if (!usable(exe))
      return makeError(ErrorCode::NotFound, "executable '" + exe + "' not found or not executable");
    return exe;
  }
  size_t start = 0;
  const std::string& path = pathVar;
  while (start <= path.size()) {
    size_t end = path.find(':', start);
    if (end == std::string::npos) end = path.size();
    std::string dir = path.substr(start, end - start);
    if (dir.empty()) dir = ".";
    const std::string candidate = dir + "/" + exe;
    if (usable(candidate)) return candidate;
    start = end + 1;
  }
  return makeError(ErrorCode::NotFound, "executable '" + exe + "' not found in PATH");
}

LaunchedProcess::~LaunchedProcess() {
  if (m_state == State::Parked)
    terminate();  // a released process is never killed: the target outlives the capture
  closePipes();
}

LaunchedProcess::LaunchedProcess(LaunchedProcess&& o) noexcept
    : m_pid(o.m_pid),
      m_state(o.m_state),
      m_releaseFd(o.m_releaseFd),
      m_errFd(o.m_errFd),
      m_reaped(o.m_reaped),
      m_status(o.m_status) {
  o.m_state = State::Dead;
  o.m_releaseFd = -1;
  o.m_errFd = -1;
  o.m_pid = 0;
}

LaunchedProcess& LaunchedProcess::operator=(LaunchedProcess&& o) noexcept {
  if (this != &o) {
    if (m_state == State::Parked) terminate();
    closePipes();
    m_pid = o.m_pid;
    m_state = o.m_state;
    m_releaseFd = o.m_releaseFd;
    m_errFd = o.m_errFd;
    m_reaped = o.m_reaped;
    m_status = o.m_status;
    o.m_state = State::Dead;
    o.m_releaseFd = -1;
    o.m_errFd = -1;
    o.m_pid = 0;
  }
  return *this;
}

void LaunchedProcess::closePipes() {
  if (m_releaseFd >= 0) ::close(m_releaseFd);
  if (m_errFd >= 0) ::close(m_errFd);
  m_releaseFd = m_errFd = -1;
}

Result<LaunchedProcess> LaunchedProcess::spawnParked(const LaunchSpec& spec) {
  // Everything that allocates happens before fork.
  std::vector<std::string> parentEnv;
  for (char** e = environ; e != nullptr && *e != nullptr; ++e) parentEnv.emplace_back(*e);
  std::vector<std::string> env = mergeEnvironment(parentEnv, spec.env);
  std::string pathVar = "/usr/local/bin:/usr/bin:/bin";
  for (const std::string& entry : env)
    if (entry.compare(0, 5, "PATH=") == 0) pathVar = entry.substr(5);
  auto resolved = resolveExecutable(spec.exe, pathVar, spec.cwd);
  if (!resolved.ok()) return resolved.error();
  if (!spec.cwd.empty()) {
    struct stat st {};
    if (::stat(spec.cwd.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
      return makeError(ErrorCode::NotFound, "working directory '" + spec.cwd + "' does not exist");
  }
  const std::string path = *resolved;
  std::vector<std::string> argvStrings;
  argvStrings.push_back(spec.exe);
  argvStrings.insert(argvStrings.end(), spec.args.begin(), spec.args.end());
  std::vector<char*> argv;
  for (std::string& a : argvStrings) argv.push_back(a.data());
  argv.push_back(nullptr);
  std::vector<char*> envp;
  for (std::string& e : env) envp.push_back(e.data());
  envp.push_back(nullptr);

  int releasePipe[2], errPipe[2];
  if (::pipe2(releasePipe, O_CLOEXEC) != 0)
    return makeError(ErrorCode::IoError, std::string("pipe failed: ") + std::strerror(errno));
  if (::pipe2(errPipe, O_CLOEXEC) != 0) {
    const int e = errno;
    ::close(releasePipe[0]);
    ::close(releasePipe[1]);
    return makeError(ErrorCode::IoError, std::string("pipe failed: ") + std::strerror(e));
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    const int e = errno;
    ::close(releasePipe[0]);
    ::close(releasePipe[1]);
    ::close(errPipe[0]);
    ::close(errPipe[1]);
    return makeError(ErrorCode::IoError, std::string("fork failed: ") + std::strerror(e));
  }
  if (pid == 0) {
    // Child: async-signal-safe calls only.
    ::close(releasePipe[1]);  // so that the parent's death shows up as EOF
    ::close(errPipe[0]);
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    ::sigaction(SIGPIPE, &dfl, nullptr);  // the agent ignores SIGPIPE; the target should not inherit that
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    char go = 0;
    ssize_t n;
    do {
      n = ::read(releasePipe[0], &go, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1 || go != 1) ::_exit(127);  // never released: do not run the target
    int err = 0;
    if (!spec.cwd.empty() && ::chdir(spec.cwd.c_str()) != 0) {
      err = errno;
    } else {
      ::execve(path.c_str(), argv.data(), envp.data());
      err = errno;
    }
    ssize_t w;
    do {
      w = ::write(errPipe[1], &err, sizeof(err));
    } while (w < 0 && errno == EINTR);
    ::_exit(127);
  }

  ::close(releasePipe[0]);
  ::close(errPipe[1]);
  LaunchedProcess p;
  p.m_pid = static_cast<uint32_t>(pid);
  p.m_state = State::Parked;
  p.m_releaseFd = releasePipe[1];
  p.m_errFd = errPipe[0];
  return p;
}

Result<void> LaunchedProcess::release() {
  if (m_state != State::Parked) return makeError(ErrorCode::Busy, "the process was already released");
  const char go = 1;
  ssize_t w;
  do {
    w = ::write(m_releaseFd, &go, 1);
  } while (w < 0 && errno == EINTR);
  const int writeErr = errno;
  ::close(m_releaseFd);
  m_releaseFd = -1;
  if (w != 1) {
    terminate();
    return makeError(ErrorCode::IoError,
                     std::string("cannot release the launched process: ") + std::strerror(writeErr));
  }
  int childErr = 0;
  ssize_t r;
  do {
    r = ::read(m_errFd, &childErr, sizeof(childErr));
  } while (r < 0 && errno == EINTR);
  ::close(m_errFd);
  m_errFd = -1;
  if (r == 0) {  // EOF: execve closed the pipe, the program is running
    m_state = State::Running;
    return ok();
  }
  // exec (or chdir) failed: reap the child
  int st = 0;
  ::waitpid(static_cast<pid_t>(m_pid), &st, 0);
  m_reaped = true;
  m_status = st;
  m_state = State::Dead;
  if (r == static_cast<ssize_t>(sizeof(childErr)))
    return makeError(childErr == ENOENT || childErr == ENOTDIR ? ErrorCode::NotFound : ErrorCode::IoError,
                     std::string("cannot start the target: ") + std::strerror(childErr));
  return makeError(ErrorCode::IoError, "the launched process exited before it could exec");
}

bool LaunchedProcess::exited() const {
  if (m_pid == 0) return true;
  if (m_reaped) return true;
  int st = 0;
  const pid_t r = ::waitpid(static_cast<pid_t>(m_pid), &st, WNOHANG);
  if (r == static_cast<pid_t>(m_pid)) {
    m_reaped = true;
    m_status = st;
    return true;
  }
  if (r < 0 && errno == ECHILD) {  // somebody else reaped it
    m_reaped = true;
    return true;
  }
  return false;
}

void LaunchedProcess::terminate() {
  if (m_pid != 0 && !m_reaped) {
    ::kill(static_cast<pid_t>(m_pid), SIGKILL);
    int st = 0;
    pid_t r;
    do {
      r = ::waitpid(static_cast<pid_t>(m_pid), &st, 0);
    } while (r < 0 && errno == EINTR);
    m_reaped = true;
    m_status = st;
  }
  m_state = State::Dead;
  closePipes();
}

}  // namespace tachyon::capture::perf
