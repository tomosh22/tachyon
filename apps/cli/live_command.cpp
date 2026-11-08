#include "cli/live_command.h"

#include "capture/windows_etw/launch.h"
#include "cli/cli.h"
#include "control/auth.h"
#include "control/client.h"
#include "control/protocol.h"
#include "control/tcp.h"
#include "control/transport.h"
#include "core/platform.h"
#include "model/live_model.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <ostream>
#include <sstream>
#include <unordered_map>

namespace tachyon::cli {

namespace {

using namespace control;
using Clock = std::chrono::steady_clock;

struct LiveArgs {
  std::string address, tokenFile;
  int timeoutS = 600;
  StartRequest start;
  double durationS = 0.0;
  double windowS = 30.0;
  double intervalS = 1.0;
  bool haveTarget = false;
  int targetFlags = 0;
};

bool parseDouble(const std::string& s, double maxValue, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (end != s.c_str() + s.size() || !(v > 0.0) || v > maxValue) return false;
  out = v;
  return true;
}

bool parseUint32(const std::string& s, uint32_t& out) {
  uint32_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}

std::string parseArgs(const std::vector<std::string>& args, LiveArgs& a) {
  a.start.live = true;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    auto value = [&](std::string& out) {
      if (i + 1 >= args.size()) return false;
      out = args[++i];
      return true;
    };
    std::string v;
    uint32_t n = 0;
    auto uintValue = [&]() { return value(v) && parseUint32(v, n); };
    if (arg == "--socket" || arg == "--pipe" || arg == "--agent") {
      if (!value(a.address)) return "missing value for " + arg;
    } else if (arg == "--token-file") {
      if (!value(a.tokenFile)) return "missing value for --token-file";
    } else if (arg == "--json" || arg == "--wait") {
      // handled by the caller / meaningless here
    } else if (arg == "--timeout" || arg == "--connect-timeout-ms") {
      if (!uintValue()) return "invalid value for " + arg;
      if (arg == "--timeout") a.timeoutS = static_cast<int>(n);
    } else if (arg == "--launch") {
      if (!value(a.start.exe)) return "missing value for --launch";
      a.start.mode = capture::CaptureMode::Launch;
      a.haveTarget = true;
      ++a.targetFlags;
    } else if (arg == "--attach") {
      if (!uintValue()) return "invalid value for --attach";
      a.start.pid = n;
      a.start.mode = capture::CaptureMode::Attach;
      a.haveTarget = true;
      ++a.targetFlags;
    } else if (arg == "--system") {
      a.start.mode = capture::CaptureMode::System;
      a.haveTarget = true;
      ++a.targetFlags;
    } else if (arg == "--arg") {
      if (!value(v)) return "missing value for --arg";
      a.start.args.push_back(v);
    } else if (arg == "--args") {
      if (i + 1 >= args.size()) return "missing value for --args";
      if (args[i + 1].find_first_of(" \t") != std::string::npos) {
        for (std::string& t : capture::etw::splitCommandLine(args[++i])) a.start.args.push_back(std::move(t));
      } else {
        while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) a.start.args.push_back(args[++i]);
      }
    } else if (arg == "--cwd") {
      if (!value(a.start.cwd)) return "missing value for --cwd";
    } else if (arg == "--env") {
      if (!value(v)) return "missing value for --env";
      const size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) return "--env expects KEY=VALUE";
      a.start.env[v.substr(0, eq)] = v.substr(eq + 1);
    } else if (arg == "--hz" || arg == "--sampling-hz") {
      if (!uintValue() || n == 0) return "invalid value for " + arg;
      a.start.sampling_hz = n;
    } else if (arg == "--follow-children") {
      a.start.follow_children = true;
    } else if (arg == "--child-filter") {
      if (!value(a.start.child_filter)) return "missing value for --child-filter";
    } else if (arg == "--circular-mb") {
      if (!uintValue()) return "invalid value for --circular-mb";
      a.start.circular_mb = n;
    } else if (arg == "--out" || arg == "--out-path") {
      if (!value(a.start.out_path)) return "missing value for " + arg;
    } else if (arg == "--duration") {
      if (!value(v) || !parseDouble(v, 86400.0 * 7, a.durationS)) return "--duration expects seconds: " + v;
    } else if (arg == "--window") {
      if (!value(v) || !parseDouble(v, 3600.0, a.windowS))
        return "--window expects seconds (at most 3600): " + v;
    } else if (arg == "--interval") {
      if (!value(v) || !parseDouble(v, 3600.0, a.intervalS)) return "--interval expects seconds: " + v;
    } else {
      return "unknown argument for live: " + arg;
    }
  }
  if (!a.haveTarget) return "live requires a target: --launch EXE, --attach PID or --system";
  if (a.targetFlags > 1) return "give exactly one of --launch, --attach, --system";
  if (a.start.mode == capture::CaptureMode::Launch && a.start.exe.empty())
    return "launch mode needs an executable";
  if (a.start.mode != capture::CaptureMode::Launch && a.durationS <= 0.0)
    return "live --attach / --system need --duration (nothing else ends the capture)";
  return {};
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

// The trace clock of a local Linux agent is CLOCK_MONOTONIC_RAW nanoseconds, the same as ours.
bool localClockNs(uint64_t& out) {
#if TACHYON_PLATFORM_LINUX
  timespec ts{};
  if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) return false;
  out = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
  return true;
#else
  (void)out;
  return false;
#endif
}

struct TopRow {
  std::string name;
  uint64_t samples = 0;
};

// The functions (frames, no symbols) with the most exclusive samples in the snapshot.
std::vector<TopRow> topFunctions(const model::TraceModel& m, uint64_t& total, size_t count) {
  std::unordered_map<uint64_t, uint64_t> byFrame;  // frame index -> samples
  total = 0;
  for (const model::SampleColumns& c : m.samplesByThread) {
    for (CallstackIdx cs : c.callstack) {
      ++total;
      if (!cs.valid() || cs.get() == 0 || cs.get() >= m.nodes.size()) continue;
      ++byFrame[m.nodes[cs.get()].frame.get()];
    }
  }
  std::vector<std::pair<uint64_t, uint64_t>> rows(byFrame.begin(), byFrame.end());
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  if (rows.size() > count) rows.resize(count);
  std::vector<TopRow> out;
  for (const auto& [frame, samples] : rows) {
    TopRow r;
    r.samples = samples;
    if (frame < m.frames.size()) {
      const model::Frame& f = m.frames[frame];
      char buf[32];
      if (f.module.valid() && f.module.get() < m.modules.size()) {
        const std::string& path = m.modules[f.module.get()].path;
        const size_t slash = path.find_last_of('/');
        std::snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(f.addr_or_rva));
        r.name = (slash == std::string::npos ? path : path.substr(slash + 1)) + buf;
      } else {
        std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(f.addr_or_rva));
        r.name = buf;
      }
    }
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace

int runLive(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err) {
  LiveArgs a;
  if (const std::string e = parseArgs(args, a); !e.empty()) {
    if (json)
      out << "{\"ok\":false,\"code\":\"EUSAGE\",\"error\":\"" << jsonEscape(e) << "\"}\n";
    else
      err << "tachyon-cli: " << e << "\n";
    return kExitUsage;
  }
  auto fail = [&](const char* code, const std::string& message, int exitCode) {
    if (json)
      out << "{\"ok\":false,\"code\":\"" << code << "\",\"error\":\"" << jsonEscape(message) << "\"}\n";
    else
      err << "error [" << code << "]: " << message << "\n";
    return exitCode;
  };
  if (a.address.empty()) a.address = defaultAgentAddress();
  const bool remote = isTcpAddress(a.address);
  std::string token;
  if (remote) {
    std::filesystem::path tokenFile =
        a.tokenFile.empty() ? tokenFileFromEnvironment() : std::filesystem::path(a.tokenFile);
    if (tokenFile.empty())
      return fail("EUSAGE", "a tcp:// agent needs --token-file FILE (or $TACHYON_AGENT_TOKEN_FILE)",
                  kExitUsage);
    auto loaded = loadTokenFile(tokenFile, 1);
    if (!loaded.ok()) return fail("EUSAGE", loaded.error().message, kExitUsage);
    token = std::move(*loaded);
  }
  auto connected = Client::connectWithRetry(a.address, 0, token);
  if (!connected.ok())
    return fail(connected.error().code == ErrorCode::NoPrivilege ? "ENOPRIV" : "ECONNECT",
                "cannot connect to agent at " + a.address + ": " + connected.error().message,
                kExitConnectionFailure);
  Client client = std::move(*connected);

  model::LiveModelOptions modelOptions;
  modelOptions.windowNs = static_cast<uint64_t>(a.windowS * 1e9);
  model::LiveModel liveModel(modelOptions);
  bool gap = false, ended = false;
  uint64_t gaps = 0;
  std::string applyError;
  client.setLiveHandler([&](const Event& e) {
    if (const auto* le = std::get_if<LiveEvent>(&e)) {
      const auto* p = reinterpret_cast<const std::byte*>(le->data.data());
      if (auto r = liveModel.apply({p, le->data.size()}, le->keyframe); !r.ok()) {
        applyError = r.error().message;
        gap = true;  // resubscribe: the next event is a keyframe
      }
    } else if (std::holds_alternative<LiveGapEvent>(e)) {
      gap = true;
    } else if (std::holds_alternative<LiveEndEvent>(e)) {
      ended = true;
    }
  });

  auto started = client.call(a.start, 30000);
  if (!started.ok()) return fail("ECONNECT", started.error().message, kExitConnectionFailure);
  if (!started->ok)
    return fail(std::string(protocolCode(started->code)).c_str(), started->error, kExitProtocolError);
  if (!json) out << "live capture started\n";

  auto subscribe = [&]() -> bool {
    auto r = client.call(LiveSubscribeRequest{}, 30000);
    return r.ok() && r->ok;
  };
  if (!subscribe()) {
    (void)client.call(CancelRequest{}, 10000);
    return fail("EINTERNAL", "live_subscribe failed", kExitProtocolError);
  }

  const auto began = Clock::now();
  auto nextSummary =
      began + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(a.intervalS));
  const auto deadline =
      a.durationS > 0.0
          ? began + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(a.durationS))
          : Clock::time_point::max();
  std::optional<CaptureDoneEvent> done;
  std::string captureError;
  uint64_t droppedLive = 0;

  auto printSummary = [&](double elapsed) {
    auto snap = liveModel.snapshot();
    const model::TraceModel& m = *snap;
    uint64_t total = 0;
    const std::vector<TopRow> top = topFunctions(m, total, 5);
    const double hz = static_cast<double>(m.header.clock_frequency_hz);
    const double windowS = hz > 0 ? static_cast<double>(m.header.end_ts - m.header.start_ts) / hz : 0.0;
    uint64_t newest = 0;
    for (const auto& c : m.samplesByThread)
      if (!c.ts.empty()) newest = std::max(newest, c.ts.back());
    double latency = -1.0;
    uint64_t now = 0;
    if (!remote && newest != 0 && localClockNs(now) && m.header.clock_frequency_hz == 1'000'000'000ull)
      latency = static_cast<double>(now >= newest ? now - newest : 0) / 1e9;
    droppedLive = m.captureInfo.events_lost;
    if (json) {
      out << "{\"t\":" << std::fixed << std::setprecision(2) << elapsed << ",\"window_s\":" << windowS
          << ",\"samples\":" << total << ",\"latency_s\":";
      if (latency >= 0)
        out << std::setprecision(3) << latency;
      else
        out << "null";
      out << ",\"gaps\":" << gaps << ",\"dropped\":" << droppedLive << ",\"top\":[";
      for (size_t i = 0; i < top.size(); ++i)
        out << (i ? "," : "") << "{\"name\":\"" << jsonEscape(top[i].name)
            << "\",\"samples\":" << top[i].samples << ",\"pct\":" << std::setprecision(1)
            << (total ? 100.0 * static_cast<double>(top[i].samples) / static_cast<double>(total) : 0.0)
            << "}";
      out << "]}\n";
    } else {
      out << "[" << std::fixed << std::setprecision(1) << elapsed << "s] window " << windowS << " s, "
          << total << " samples, latency ";
      if (latency >= 0)
        out << std::setprecision(2) << latency << " s";
      else
        out << "n/a";
      out << ", gaps " << gaps << ", dropped " << droppedLive << ", top:";
      if (top.empty()) out << " -";
      for (const TopRow& r : top)
        out << " " << r.name << " " << std::setprecision(1)
            << (total ? 100.0 * static_cast<double>(r.samples) / static_cast<double>(total) : 0.0) << "%";
      out << "\n";
    }
    out.flush();
  };

  // Main loop: live events are applied by the handler while we wait for the other events.
  while (true) {
    auto ev = client.nextEvent(100);
    if (!ev.ok() && ev.error().code != ErrorCode::Timeout) {
      return fail("ECONNECT", "connection to the agent lost: " + ev.error().message, kExitConnectionFailure);
    }
    if (ev.ok()) {
      if (const auto* d = std::get_if<CaptureDoneEvent>(&*ev)) {
        done = *d;
        break;
      }
      if (const auto* e = std::get_if<ErrorEvent>(&*ev)) {
        captureError = e->message;
        break;
      }
    }
    if (gap) {
      gap = false;
      ++gaps;
      if (!ended && !subscribe()) break;
    }
    const auto now = Clock::now();
    if (now >= nextSummary) {
      printSummary(std::chrono::duration<double>(now - began).count());
      nextSummary += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(a.intervalS));
    }
    if (now >= deadline) break;
    if (std::chrono::duration<double>(now - began).count() > a.timeoutS && a.durationS <= 0.0) break;
  }

  if (!done && captureError.empty()) {
    auto stop = client.call(StopRequest{}, 30000);
    if (!stop.ok()) return fail("ECONNECT", stop.error().message, kExitConnectionFailure);
    if (!stop->ok && stop->code != ErrorCode::NotFound && stop->code != ErrorCode::Busy)
      return fail(std::string(protocolCode(stop->code)).c_str(), stop->error, kExitProtocolError);
    auto event = client.waitForEvent(
        [](const Event& e) {
          return std::holds_alternative<CaptureDoneEvent>(e) || std::holds_alternative<ErrorEvent>(e);
        },
        a.timeoutS * 1000);
    if (!event.ok())
      return fail("ECONNECT", "waiting for capture_done: " + event.error().message, kExitConnectionFailure);
    if (const auto* d = std::get_if<CaptureDoneEvent>(&*event)) done = *d;
    if (const auto* e = std::get_if<ErrorEvent>(&*event)) captureError = e->message;
  }
  printSummary(std::chrono::duration<double>(Clock::now() - began).count());
  if (!captureError.empty()) return fail("EINTERNAL", captureError, kExitProtocolError);
  if (done) {
    if (json)
      out << "{\"event\":\"capture_done\",\"path\":\"" << jsonEscape(done->summary.path)
          << "\",\"events_lost\":" << done->summary.events_lost << ",\"gaps\":" << gaps << "}\n";
    else
      out << "capture done: " << done->summary.path << " (" << done->summary.events_lost << " events lost, "
          << gaps << " live gaps)\n";
    for (const std::string& w : done->summary.warnings) err << "warning: " << w << "\n";
    if (done->summary.events_lost > 0) return kExitEventsLost;
  }
  return kExitOk;
}

}  // namespace tachyon::cli
