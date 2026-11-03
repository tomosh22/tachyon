// tachyon-agent: capture control daemon. See agent.h for the state machine.
#include "agent/agent.h"
#include "agent/privileges.h"
#include "agent/stub_backend.h"
#include "capture/mock/mock_backend.h"
#include "capture/windows_etw/windows_etw_backend.h"
#if TACHYON_HAVE_PERF_BACKEND
#include "capture/linux_perf/linux_perf_backend.h"
#endif
#include "control/auth.h"
#include "control/server.h"
#include "control/tcp.h"
#include "control/transport.h"
#include "core/log.h"
#include "core/platform.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if !TACHYON_PLATFORM_WINDOWS
#include <unistd.h>
#endif

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
      "usage: tachyon-agent [--backend mock|etw|perf] [--socket PATH | --pipe NAME] [--socket-owner UID]\n"
      "                     [--log-level trace|debug|info|warn|error]\n"
      "                     [--listen HOST:PORT --token-file FILE [--allow-remote-launch] "
      "[--remote-out-dir DIR]]\n"
      "                     [--mock-fixture FILE] [--mock-duration-ms N] [--mock-steps N] "
      "[--etw-delete-raw] [--perf-delete-raw] [--perf-ring-pages N]\n"
      "                     [--inst-spool-mb N] [--inst-spool-dir DIR] [--keep-inst-spool]\n"
      "                     [--flush-seconds N] [--flush-coalesce-ms N] [--ring-min-mb N]\n"
      "The address defaults to $TACHYON_AGENT_PIPE or the platform default. An agent running as root on\n"
      "behalf of another user (started through pkexec or sudo: $PKEXEC_UID / $SUDO_UID, or --socket-owner "
      "UID)\n"
      "hands its control socket and the SDK sockets to that user (modes stay 0600).\n"
      "Remote capture: --listen HOST:PORT (IPv4, [IPv6] or a host name; name the host explicitly, 0.0.0.0\n"
      "listens on every interface) also accepts TCP clients, which must send "
      "{\"cmd\":\"auth\",\"token\":...}\n"
      "first. --token-file is required: a file whose first line is the token (at least 32 characters, not\n"
      "readable by group or others). The channel is NOT encrypted; use a trusted network or an SSH tunnel.\n"
      "Remote clients cannot launch programs unless --allow-remote-launch is given, cannot choose where\n"
      "traces are written (--remote-out-dir, default the working directory), and fetch only traces this "
      "agent\n"
      "produced. The mock backend copies\n"
      "$TACHYON_MOCK_FIXTURE (or --mock-fixture) to the requested output path. The etw backend keeps the\n"
      "raw <out>.etl next to the trace unless --etw-delete-raw is given. The perf backend (Linux) keeps its "
      "raw\n"
      "spool directory <out>.perfspool unless --perf-delete-raw is given; --perf-ring-pages sets the ring "
      "size per\n"
      "cpu (a power of two, default 256 = 1 MiB). Instrumentation from SDK clients "
      "is\n"
      "spooled per process (default cap 1024 MiB, --inst-spool-mb) in <out>.inst/ (or under "
      "--inst-spool-dir)\n"
      "and merged into the trace when the capture ends; --keep-inst-spool keeps the spool files.\n"
      "Flight recorder: a capture started with circular_mb > 0 keeps the newest instrumentation in a ring "
      "of\n"
      "max(circular_mb, --ring-min-mb (default 64)) MiB per process. Tachyon_RequestFlush() from an\n"
      "instrumented process flushes the last --flush-seconds (default 10) into\n"
      "<out dir>/<process>-<reason>-<YYYYMMDD-HHMMSS>.tachyon; triggers closer than --flush-coalesce-ms\n"
      "(default 2000) to the previous one are coalesced.\n");
}

struct Options {
  std::string backend = "mock";
  std::string address;
  std::string logLevel;
  std::string mockFixture;
  long mockDurationMs = 0;
  long mockSteps = 4;
  bool etwDeleteRaw = false;
  bool perfDeleteRaw = false;
  long perfRingPages = 256;
  long instSpoolMb = 1024;
  std::string instSpoolDir;
  bool keepInstSpool = false;
  long flushSeconds = 10;
  long flushCoalesceMs = 2000;
  long ringMinMb = 64;
  std::string socketOwner;
  std::string listen;
  std::string tokenFile;
  std::string remoteOutDir;
  bool allowRemoteLaunch = false;
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
    } else if (a == "--socket-owner") {
      if (!value(o.socketOwner)) return false;
    } else if (a == "--listen") {
      if (!value(o.listen)) return false;
    } else if (a == "--token-file") {
      if (!value(o.tokenFile)) return false;
    } else if (a == "--remote-out-dir") {
      if (!value(o.remoteOutDir)) return false;
    } else if (a == "--allow-remote-launch") {
      o.allowRemoteLaunch = true;
    } else if (a == "--log-level") {
      if (!value(o.logLevel)) return false;
    } else if (a == "--etw-delete-raw") {
      o.etwDeleteRaw = true;
    } else if (a == "--perf-delete-raw") {
      o.perfDeleteRaw = true;
    } else if (a == "--perf-ring-pages") {
      if (!value(tmp)) return false;
      o.perfRingPages = std::strtol(tmp.c_str(), nullptr, 10);
      if (o.perfRingPages < 2 || o.perfRingPages > (1l << 20) ||
          (o.perfRingPages & (o.perfRingPages - 1)) != 0)
        return false;  // the kernel wants a power-of-two number of data pages
    } else if (a == "--inst-spool-mb") {
      if (!value(tmp)) return false;
      o.instSpoolMb = std::strtol(tmp.c_str(), nullptr, 10);
      if (o.instSpoolMb <= 0) return false;
    } else if (a == "--inst-spool-dir") {
      if (!value(o.instSpoolDir)) return false;
    } else if (a == "--keep-inst-spool") {
      o.keepInstSpool = true;
    } else if (a == "--flush-seconds") {
      if (!value(tmp)) return false;
      o.flushSeconds = std::strtol(tmp.c_str(), nullptr, 10);
      if (o.flushSeconds <= 0) return false;
    } else if (a == "--flush-coalesce-ms") {
      if (!value(tmp)) return false;
      o.flushCoalesceMs = std::strtol(tmp.c_str(), nullptr, 10);
      if (o.flushCoalesceMs < 0) return false;
    } else if (a == "--ring-min-mb") {
      if (!value(tmp)) return false;
      o.ringMinMb = std::strtol(tmp.c_str(), nullptr, 10);
      if (o.ringMinMb <= 0) return false;
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

  // WP-61: a TCP listener needs a good token file; check everything before anything is created.
  std::unique_ptr<control::IListener> tcpListener;
  control::AuthPolicy authPolicy;
  if (!opt.listen.empty()) {
    if (opt.tokenFile.empty()) {
      std::fprintf(stderr, "tachyon-agent: --listen needs --token-file (a file holding the shared token)\n");
      return kExitUsage;
    }
    auto endpoint = control::parseTcpEndpoint(opt.listen);
    if (!endpoint.ok()) {
      std::fprintf(stderr, "tachyon-agent: bad --listen '%s': %s\n", opt.listen.c_str(),
                   endpoint.error().message.c_str());
      return kExitUsage;
    }
    auto token = control::loadTokenFile(opt.tokenFile, control::kMinAgentTokenChars);
    if (!token.ok()) {
      std::fprintf(stderr, "tachyon-agent: refusing to listen on TCP: %s\n", token.error().message.c_str());
      return kExitFatal;
    }
    authPolicy.token = std::move(*token);
    auto bound = control::listenTcp(*endpoint);
    if (!bound.ok()) {
      std::fprintf(stderr, "tachyon-agent: cannot listen on %s: %s\n", opt.listen.c_str(),
                   bound.error().toString().c_str());
      return kExitFatal;
    }
    tcpListener = std::move(*bound);
    if (!control::isLoopbackHost(endpoint->host))
      TACHYON_LOG_WARN(
          "agent: listening on a non-loopback address; the channel is not encrypted and the token is the "
          "only "
          "protection (use a trusted network or an SSH tunnel)");
  } else if (!opt.tokenFile.empty() || opt.allowRemoteLaunch) {
    std::fprintf(stderr, "tachyon-agent: --token-file / --allow-remote-launch only apply with --listen\n");
    return kExitUsage;
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
                   TACHYON_PLATFORM_WINDOWS
                       ? "an elevated (administrator) process"
                       : "root, CAP_PERFMON, CAP_SYS_ADMIN or kernel.perf_event_paranoid <= 0 "
                         "(run 'tachyon-cli doctor')");
      return kExitNoPrivilege;
    }
    if (opt.backend == "etw") {
      // Stops Tachyon sessions left behind by a crashed agent before the first capture (see EtwSession).
      auto orphans = capture::etw::EtwSession::cleanupOrphans();
      if (!orphans.ok()) {
        TACHYON_LOG_WARN("agent: ETW orphan cleanup failed: {}", orphans.error().toString());
      } else {
        for (const std::string& n : orphans->stopped)
          TACHYON_LOG_WARN("agent: stopped orphaned ETW session '{}'", n);
      }
      capture::WindowsEtwBackendConfig etwCfg;
      etwCfg.keepRawEtl = !opt.etwDeleteRaw;
      backend = std::make_unique<capture::WindowsEtwBackend>(etwCfg);
    } else {
#if TACHYON_HAVE_PERF_BACKEND
      capture::LinuxPerfBackendConfig perfCfg;
      perfCfg.keepRawSpool = !opt.perfDeleteRaw;
      perfCfg.ringPages = static_cast<uint32_t>(opt.perfRingPages);
      backend = std::make_unique<capture::LinuxPerfBackend>(perfCfg);
#else
      backend = std::make_unique<agent::StubBackend>(opt.backend);
#endif
    }
  } else {
    std::fprintf(stderr, "tachyon-agent: unknown backend '%s' (expected mock|etw|perf)\n",
                 opt.backend.c_str());
    return kExitUsage;
  }

  const std::string address = !opt.address.empty() ? opt.address : control::defaultAgentAddress();

  // WP-41b: an agent started with elevated rights for another user (pkexec, sudo, or --socket-owner) hands
  // its sockets to that user; the modes stay owner-only.
  control::SocketOwner socketOwner;
  {
    uint32_t ownerUid = 0;
    bool have = false;
    if (!opt.socketOwner.empty()) {
      if (!agent::parseSocketOwnerUid(opt.socketOwner, ownerUid)) {
        std::fprintf(stderr, "tachyon-agent: bad --socket-owner '%s' (expected a non-zero numeric uid)\n",
                     opt.socketOwner.c_str());
        return kExitUsage;
      }
      have = true;
    } else {
#if !TACHYON_PLATFORM_WINDOWS
      have = agent::socketOwnerFromEnvironment(geteuid() == 0, std::getenv("PKEXEC_UID"),
                                               std::getenv("SUDO_UID"), ownerUid);
#endif
    }
    if (have) {
      socketOwner = agent::socketOwnerForUid(ownerUid);
      TACHYON_LOG_INFO("agent: sockets will belong to uid {}", ownerUid);
    }
  }
  auto listener = control::listenOn(address, socketOwner);
  if (!listener.ok()) {
    TACHYON_LOG_ERROR("agent: cannot listen on {}: {}", address, listener.error().toString());
    return kExitFatal;
  }

  agent::AgentConfig cfg;
  cfg.backendName = opt.backend;
  cfg.agentAddress = address;
  cfg.elevated = elevated;
  cfg.socketOwner = socketOwner;
  cfg.allowRemoteLaunch = opt.allowRemoteLaunch;
  {
    std::error_code ec;
    cfg.remoteOutDir = opt.remoteOutDir.empty() ? std::filesystem::current_path(ec)
                                                : std::filesystem::absolute(opt.remoteOutDir, ec);
  }
  cfg.maxInstSpoolBytes = static_cast<uint64_t>(opt.instSpoolMb) << 20;
  cfg.instSpoolRoot = opt.instSpoolDir;
  cfg.keepInstSpool = opt.keepInstSpool;
  cfg.flushSeconds = static_cast<uint32_t>(opt.flushSeconds);
  cfg.flushCoalesceMs = static_cast<int>(opt.flushCoalesceMs);
  cfg.minRingBytes = static_cast<uint64_t>(opt.ringMinMb) << 20;
  agent::Agent agentCore(std::move(backend), cfg);
  control::Server server(std::move(*listener), agentCore);
  std::string tcpAddress;
  if (tcpListener) {
    tcpAddress = tcpListener->address();
    server.addRemoteListener(std::move(tcpListener), std::move(authPolicy));
  }
  agentCore.setEventSink([&server](const control::Event& e) { server.broadcast(e); });

  installSignalHandlers();
  server.start();
  TACHYON_LOG_INFO("tachyon-agent listening on {} (backend {})", address, opt.backend);
  if (!tcpAddress.empty())
    TACHYON_LOG_INFO(
        "tachyon-agent listening on TCP {} (unencrypted, token authentication; remote launch {}; remote "
        "traces go to {})",
        tcpAddress, opt.allowRemoteLaunch ? "ALLOWED" : "disabled", cfg.remoteOutDir.string());

  while (!g_stopRequested.load()) std::this_thread::sleep_for(std::chrono::milliseconds(50));

  TACHYON_LOG_INFO("agent: shutting down");
  server.stop();
  agentCore.shutdown();
  return kExitOk;
}
