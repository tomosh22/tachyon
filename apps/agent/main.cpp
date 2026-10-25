// tachyon-agent: capture control daemon. See agent.h for the state machine.
#include "agent/agent.h"
#include "agent/privileges.h"
#include "agent/stub_backend.h"
#include "capture/mock/mock_backend.h"
#include "control/server.h"
#include "control/transport.h"
#include "core/log.h"
#include "core/platform.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if TACHYON_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using namespace tachyon;

// Exit codes.
constexpr int kExitOk = 0;
constexpr int kExitFatal = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNoPrivilege = 3;

std::atomic<bool> g_stopRequested{false};

#if TACHYON_PLATFORM_WINDOWS
BOOL WINAPI consoleHandler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT: g_stopRequested.store(true); return TRUE;
    default: return FALSE;
  }
}
void installSignalHandlers() {
  SetConsoleCtrlHandler(consoleHandler, TRUE);
}
#else
extern "C" void signalHandler(int) {
  g_stopRequested.store(true);
}
void installSignalHandlers() {
  struct sigaction sa;
  sa.sa_handler = signalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);
}
#endif

void printUsage() {
  std::fprintf(
      stderr,
      "usage: tachyon-agent [--backend mock|etw|perf] [--socket PATH | --pipe NAME]\n"
      "                     [--log-level trace|debug|info|warn|error]\n"
      "                     [--mock-fixture FILE] [--mock-duration-ms N] [--mock-steps N]\n"
      "The address defaults to $TACHYON_AGENT_PIPE or the platform default. The mock backend copies\n"
      "$TACHYON_MOCK_FIXTURE (or --mock-fixture) to the requested output path.\n");
}

struct Options {
  std::string backend = "mock";
  std::string address;
  std::string logLevel;
  std::string mockFixture;
  long mockDurationMs = 0;
  long mockSteps = 4;
  bool help = false;
};

// Returns false on a usage error.
bool parseArgs(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    std::string tmp;
    if (a == "--help" || a == "-h") {
      o.help = true;
    } else if (a == "--backend") {
      if (!value(o.backend)) return false;
    } else if (a == "--socket" || a == "--pipe") {
      if (!value(o.address)) return false;
    } else if (a == "--log-level") {
      if (!value(o.logLevel)) return false;
    } else if (a == "--mock-fixture") {
      if (!value(o.mockFixture)) return false;
    } else if (a == "--mock-duration-ms") {
      if (!value(tmp)) return false;
      o.mockDurationMs = std::strtol(tmp.c_str(), nullptr, 10);
    } else if (a == "--mock-steps") {
      if (!value(tmp)) return false;
      o.mockSteps = std::strtol(tmp.c_str(), nullptr, 10);
    } else {
      std::fprintf(stderr, "tachyon-agent: unknown argument '%s'\n", a.c_str());
      return false;
    }
  }
  return true;
}

bool parseLogLevel(const std::string& s, core::LogLevel& out) {
  for (core::LogLevel l : {core::LogLevel::Trace, core::LogLevel::Debug, core::LogLevel::Info,
                           core::LogLevel::Warn, core::LogLevel::Error, core::LogLevel::Off}) {
    if (core::logLevelName(l) == s) {
      out = l;
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parseArgs(argc, argv, opt)) {
    printUsage();
    return kExitUsage;
  }
  if (opt.help) {
    printUsage();
    return kExitOk;
  }
  if (!opt.logLevel.empty()) {
    core::LogLevel level;
    if (!parseLogLevel(opt.logLevel, level)) {
      std::fprintf(stderr, "tachyon-agent: bad --log-level '%s'\n", opt.logLevel.c_str());
      return kExitUsage;
    }
    core::setLogLevel(level);
  }

  const bool elevated = agent::hasCapturePrivileges();
  std::unique_ptr<capture::ICaptureBackend> backend;
  if (opt.backend == "mock") {
    capture::MockBackendConfig cfg;
    cfg.fixture = opt.mockFixture;
    cfg.captureDuration = std::chrono::milliseconds(opt.mockDurationMs);
    cfg.progressSteps = opt.mockSteps > 0 ? static_cast<uint32_t>(opt.mockSteps) : 1U;
    backend = std::make_unique<capture::MockBackend>(cfg);
    if (!elevated)
      TACHYON_LOG_WARN("agent: not elevated (CAP_PERFMON/root or admin); fine for the mock backend");
  } else if (opt.backend == "etw" || opt.backend == "perf") {
    const bool available =
        (opt.backend == "etw") ? TACHYON_PLATFORM_WINDOWS != 0 : TACHYON_PLATFORM_LINUX != 0;
    if (!available) {
      std::fprintf(stderr, "tachyon-agent: backend '%s' not available in this build\n", opt.backend.c_str());
      return kExitFatal;
    }
    if (!elevated) {
      std::fprintf(stderr, "tachyon-agent: ENOPRIV: the '%s' backend needs %s\n", opt.backend.c_str(),
                   TACHYON_PLATFORM_WINDOWS ? "an elevated (administrator) process" : "root or CAP_PERFMON");
      return kExitNoPrivilege;
    }
    backend = std::make_unique<agent::StubBackend>(opt.backend);
  } else {
    std::fprintf(stderr, "tachyon-agent: unknown backend '%s' (expected mock|etw|perf)\n",
                 opt.backend.c_str());
    return kExitUsage;
  }

  const std::string address = !opt.address.empty() ? opt.address : control::defaultAgentAddress();
  auto listener = control::listenOn(address);
  if (!listener.ok()) {
    TACHYON_LOG_ERROR("agent: cannot listen on {}: {}", address, listener.error().toString());
    return kExitFatal;
  }

  agent::AgentConfig cfg;
  cfg.backendName = opt.backend;
  cfg.agentAddress = address;
  cfg.elevated = elevated;
  agent::Agent agentCore(std::move(backend), cfg);
  control::Server server(std::move(*listener), agentCore);
  agentCore.setEventSink([&server](const control::Event& e) { server.broadcast(e); });

  installSignalHandlers();
  server.start();
  TACHYON_LOG_INFO("tachyon-agent listening on {} (backend {})", address, opt.backend);

  while (!g_stopRequested.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));

  TACHYON_LOG_INFO("agent: shutting down");
  server.stop();
  agentCore.shutdown();
  return kExitOk;
}
