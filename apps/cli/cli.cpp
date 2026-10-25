#include "cli/cli.h"

#include "capture/windows_etw/launch.h"
#include "control/client.h"
#include "control/protocol.h"
#include "control/transport.h"
#include "inst/merge.h"
#include "inst/sidecar_paths.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <ostream>
#include <sstream>

namespace tachyon::cli {

namespace {

using namespace control;

struct Parsed {
  std::string address;
  bool json = false;
  int timeoutS = 600;
  int connectTimeoutMs = 0;
  bool wait = false;       // start / stop: wait for capture_done
  double durationS = 0.0;  // capture: stop after this many seconds (0: until the target exits)
  bool noStart = false;    // start: park the launch
  std::string command;
  RequestBody request;
};

bool parseUint(const std::string& s, uint32_t& out) {
  uint32_t v = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || ptr != s.data() + s.size()) return false;
  out = v;
  return true;
}

bool parseSeconds(const std::string& s, double& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end != s.c_str() + s.size() || !(v > 0.0) || v > 86400.0 * 7) return false;
  out = v;
  return true;
}

bool isStartLike(const std::string& command) {
  return command == "start" || command == "capture";
}

bool hasWhitespace(const std::string& s) {
  return s.find_first_of(" \t\n") != std::string::npos;
}

// Parses args into `p`. Returns an error message on usage errors, empty string on success.
std::string parse(const std::vector<std::string>& args, Parsed& p) {
  StartRequest start;
  FlushRequest flush;
  bool haveMode = false, haveName = false, haveSeconds = false;
  int targetFlags = 0;  // --launch / --attach / --system / --mode: exactly one

  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    auto needValue = [&](std::string& out) {
      if (i + 1 >= args.size()) return false;
      out = args[++i];
      return true;
    };
    auto needUint = [&](uint32_t& out, std::string& err) {
      std::string v;
      if (!needValue(v)) {
        err = "missing value for " + a;
        return false;
      }
      if (!parseUint(v, out)) {
        err = "invalid number for " + a + ": " + v;
        return false;
      }
      return true;
    };
    std::string err, v;
    uint32_t n = 0;
    const bool startLike = isStartLike(p.command);

    if (a == "--socket" || a == "--pipe") {
      if (!needValue(p.address)) return "missing value for " + a;
    } else if (a == "--json") {
      p.json = true;
    } else if (a == "--timeout") {
      if (!needUint(n, err)) return err;
      p.timeoutS = static_cast<int>(n);
    } else if (a == "--connect-timeout-ms") {
      if (!needUint(n, err)) return err;
      p.connectTimeoutMs = static_cast<int>(n);
    } else if (a == "--wait") {
      p.wait = true;
    } else if (a.rfind("--", 0) != 0 && p.command.empty()) {
      p.command = a;
    } else if (p.command == "capture" && a == "--duration") {
      if (!needValue(v)) return "missing value for --duration";
      if (!parseSeconds(v, p.durationS)) return "--duration expects a positive number of seconds: " + v;
    } else if (startLike && a == "--launch") {
      if (!needValue(start.exe)) return "missing value for --launch";
      start.mode = capture::CaptureMode::Launch;
      haveMode = true;
      ++targetFlags;
    } else if (startLike && a == "--attach") {
      if (!needUint(start.pid, err)) return err;
      start.mode = capture::CaptureMode::Attach;
      haveMode = true;
      ++targetFlags;
    } else if (startLike && a == "--system") {
      start.mode = capture::CaptureMode::System;
      haveMode = true;
      ++targetFlags;
    } else if (startLike && a == "--mode") {
      if (!needValue(v)) return "missing value for --mode";
      auto m = capture::parseCaptureMode(v);
      if (!m) return "--mode must be launch, attach or system";
      start.mode = *m;
      haveMode = true;
      ++targetFlags;
    } else if (startLike && a == "--exe") {
      if (!needValue(start.exe)) return "missing value for --exe";
    } else if (startLike && a == "--cwd") {
      if (!needValue(start.cwd)) return "missing value for --cwd";
    } else if (startLike && a == "--arg") {
      if (!needValue(v)) return "missing value for --arg";
      start.args.push_back(v);
    } else if (startLike && a == "--args") {
      if (i + 1 >= args.size()) return "missing value for --args";
      if (hasWhitespace(args[i + 1])) {
        // One command-line string: "--scenario spin --duration 3".
        for (std::string& t : capture::etw::splitCommandLine(args[++i])) start.args.push_back(std::move(t));
      } else {
        // Legacy form: every following word up to the next "--option".
        while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) start.args.push_back(args[++i]);
      }
    } else if (startLike && a == "--env") {
      if (!needValue(v)) return "missing value for --env";
      const size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) return "--env expects KEY=VALUE";
      start.env[v.substr(0, eq)] = v.substr(eq + 1);
    } else if (startLike && a == "--pid") {
      if (!needUint(start.pid, err)) return err;
    } else if (startLike && (a == "--hz" || a == "--sampling-hz")) {
      if (!needUint(start.sampling_hz, err)) return err;
    } else if (startLike && a == "--follow-children") {
      start.follow_children = true;
    } else if (startLike && a == "--child-filter") {
      if (!needValue(start.child_filter)) return "missing value for --child-filter";
    } else if (startLike && a == "--circular-mb") {
      if (!needUint(start.circular_mb, err)) return err;
    } else if (startLike && (a == "--out" || a == "--out-path")) {
      if (!needValue(start.out_path)) return "missing value for " + a;
    } else if (startLike && (a == "--no-start" || a == "--defer-start")) {
      start.start_immediately = false;
      p.noStart = true;
    } else if (startLike && a == "--no-profile") {
      start.stackwalk.profile = false;
    } else if (startLike && a == "--no-cswitch") {
      start.stackwalk.cswitch = false;
    } else if (startLike && a == "--no-readythread") {
      start.stackwalk.readythread = false;
    } else if (startLike && a == "--no-kernel-stacks") {
      start.kernel_stacks = false;
    } else if (p.command == "flush" && a == "--name") {
      if (!needValue(flush.name)) return "missing value for --name";
      haveName = true;
    } else if (p.command == "flush" && a == "--seconds") {
      if (!needUint(flush.seconds, err)) return err;
      haveSeconds = true;
    } else {
      return "unknown argument: " + a;
    }
  }

  if (p.command == "status") {
    p.request = StatusRequest{};
  } else if (p.command == "stop") {
    p.request = StopRequest{};
  } else if (p.command == "cancel") {
    p.request = CancelRequest{};
  } else if (p.command == "resume") {
    // The trigger of a parked launch (see agent.h): a start whose only meaning is "now". The protocol insists
    // on an exe for mode launch; the agent ignores everything else in this request.
    StartRequest r;
    r.mode = capture::CaptureMode::Launch;
    r.exe = "-";
    r.start_immediately = true;
    p.request = std::move(r);
  } else if (isStartLike(p.command)) {
    if (!haveMode) return p.command + " requires a target: --launch EXE, --attach PID or --system";
    if (targetFlags > 1) return "give exactly one of --launch, --attach, --system, --mode";
    if (start.mode == capture::CaptureMode::Launch && start.exe.empty())
      return "launch mode needs an executable (--launch EXE or --exe E)";
    if (start.mode != capture::CaptureMode::Launch && p.noStart) return "--no-start applies to --launch only";
    if (!start.follow_children && !start.child_filter.empty())
      return "--child-filter needs --follow-children";
    if (p.command == "capture") {
      if (p.noStart) return "capture starts the recording itself; --no-start belongs to the start command";
      if (start.mode == capture::CaptureMode::System && p.durationS <= 0.0)
        return "capture --system needs --duration (nothing else ends a system capture)";
    }
    p.request = std::move(start);
  } else if (p.command == "flush") {
    if (!haveName || !haveSeconds) return "flush requires --name and --seconds";
    p.request = std::move(flush);
  } else if (p.command.empty()) {
    return "missing command";
  } else {
    return "unknown command: " + p.command;
  }
  if (p.wait && p.command != "start" && p.command != "stop") return "--wait applies to start and stop only";
  if (p.wait && p.noStart) return "--wait cannot be combined with --no-start";
  if (p.durationS > 0.0 && p.command != "capture") return "--duration applies to capture only";
  return {};
}

void printUsage(std::ostream& err) {
  err << "usage: tachyon-cli [--socket PATH | --pipe NAME] [--json] [--timeout SECONDS] <command> [options]\n"
         "commands:\n"
         "  status\n"
         "  start   <target> [start options] [--no-start] [--wait]\n"
         "  stop    [--wait]\n"
         "  resume                       begin recording a launch parked with 'start --no-start'\n"
         "  flush   --name N --seconds S\n"
         "  cancel\n"
         "  merge   TRACE.tachyon INST.tachyon-inst [--out PATH] [--clip] [--no-siblings]\n"
         "                               adds an instrumentation sidecar (and its numbered siblings) to a "
         "trace;\n"
         "                               needs no agent\n"
         "  capture <target> [start options] [--duration S]\n"
         "target: --launch EXE | --attach PID | --system | --mode launch|attach|system [--exe E] [--pid N]\n"
         "start options: [--args \"A B\" | --args A B | --arg A]... [--cwd D] [--env K=V]... [--hz N]\n"
         "  [--follow-children] [--child-filter GLOB] [--circular-mb N] [--out PATH] [--no-profile]\n"
         "  [--no-cswitch] [--no-readythread] [--no-kernel-stacks]\n"
         "exit codes: 0 ok, 1 agent/capture error, 2 connection failure or timeout, 3 usage,\n"
         "            4 capture written but events were lost\n";
}

void printResponse(const Response& r, bool json, std::ostream& out, std::ostream& err) {
  if (json) {
    out << encodeResponse(r) << "\n";
    return;
  }
  if (!r.ok) {
    err << "error [" << protocolCode(r.code) << "]: " << r.error << "\n";
    return;
  }
  if (const auto* s = std::get_if<StatusBody>(&r.body)) {
    out << "state: " << capture::captureStateName(s->state) << "\nprogress: " << std::fixed
        << std::setprecision(2) << s->progress << "\nbackend: " << s->backend
        << "\nelevated: " << (s->elevated ? "yes" : "no") << "\n";
  } else if (const auto* c = std::get_if<CaptureSummary>(&r.body)) {
    out << "flushed: " << c->path << " (events_lost " << c->events_lost << ")\n";
  } else {
    out << "ok\n";
  }
}

void printEvent(const Event& e, bool json, std::ostream& out) {
  if (json) {
    out << encodeEvent(e) << "\n";
    return;
  }
  if (const auto* d = std::get_if<CaptureDoneEvent>(&e)) {
    out << "capture_done: path=" << d->summary.path << " events_lost=" << d->summary.events_lost
        << " duration_s=" << std::fixed << std::setprecision(3) << d->summary.duration_s << "\n";
  }
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  for (const char ch : in) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

// Reports a failure that did not come from the agent: a human line on stderr and, with --json, one
// `{"ok":false,"error":{"code":...,"message":...}}` line on stdout, so scripts always get valid JSON.
// Codes: EUSAGE (exit 3), ECONNECT (exit 2, cannot reach or lost the agent), ETIMEOUT (exit 2, a wait ran
// out).
void reportFailure(bool json, std::ostream& out, std::ostream& err, const char* code,
                   const std::string& message) {
  err << "tachyon-cli: " << message << "\n";
  if (json)
    out << "{\"ok\":false,\"error\":{\"code\":\"" << code << "\",\"message\":\"" << jsonEscape(message)
        << "\"}}\n";
}

const char* codeFor(const Error& e) {
  return e.code == ErrorCode::Timeout ? "ETIMEOUT" : "ECONNECT";
}

bool isFinal(const Event& e) {
  return std::holds_alternative<CaptureDoneEvent>(e) || std::holds_alternative<ErrorEvent>(e);
}

// Prints the final event and maps it to an exit code.
int finish(const Event& event, bool json, std::ostream& out, std::ostream& err) {
  if (const auto* ee = std::get_if<ErrorEvent>(&event)) {
    if (json) {
      out << encodeEvent(event) << "\n";
    } else {
      err << "error: " << ee->message << "\n";
    }
    return kExitProtocolError;
  }
  printEvent(event, json, out);
  if (const auto* d = std::get_if<CaptureDoneEvent>(&event); d != nullptr && d->summary.events_lost > 0) {
    err << "tachyon-cli: warning: the session lost " << d->summary.events_lost
        << " events; the trace is incomplete\n";
    return kExitEventsLost;
  }
  return kExitOk;
}

// `capture`: the start request was accepted; wait for the end, stop when --duration runs out.
int runCapture(Client& client, const Parsed& p, std::ostream& out, std::ostream& err) {
  const int timeoutMs = p.timeoutS * 1000;
  const bool timed = p.durationS > 0.0;
  const int firstWaitMs = timed ? static_cast<int>(p.durationS * 1000.0) : timeoutMs;

  // The agent finishing by itself (the launched or attached target exited) arrives as capture_done.
  auto event = client.waitForEvent(isFinal, firstWaitMs);
  bool timedOut = false;
  if (!event.ok()) {
    if (event.error().code != ErrorCode::Timeout) {
      reportFailure(p.json, out, err, codeFor(event.error()),
                    "waiting for the capture: " + event.error().message);
      return kExitConnectionFailure;
    }
    timedOut = !timed;  // no --duration: --timeout ran out before the target exited
    auto stop = client.call(StopRequest{});
    if (!stop.ok()) {
      reportFailure(p.json, out, err, codeFor(stop.error()), stop.error().message);
      return kExitConnectionFailure;
    }
    // NotFound / Busy: the capture just ended on its own; its capture_done is already on the way.
    if (!stop->ok && stop->code != ErrorCode::NotFound && stop->code != ErrorCode::Busy) {
      printResponse(*stop, p.json, out, err);
      return kExitProtocolError;
    }
    event = client.waitForEvent(isFinal, timeoutMs);
    if (!event.ok()) {
      reportFailure(p.json, out, err, codeFor(event.error()),
                    "waiting for capture_done: " + event.error().message);
      return kExitConnectionFailure;
    }
  }
  const int code = finish(*event, p.json, out, err);
  if (timedOut && code == kExitOk) {
    reportFailure(p.json, out, err, "ETIMEOUT",
                  "the target did not exit within --timeout " + std::to_string(p.timeoutS) +
                      " s; the capture was stopped and the trace written");
    return kExitConnectionFailure;
  }
  return code;
}

// `merge TRACE INST [--out PATH] [--clip] [--no-siblings]`: offline, needs no agent.
int runMerge(const std::vector<std::string>& args, bool json, std::ostream& out, std::ostream& err) {
  namespace fs = std::filesystem;
  std::vector<std::string> positional;
  std::string outPath;
  bool clip = false, siblings = true;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--out") {
      if (i + 1 >= args.size()) {
        reportFailure(json, out, err, "EUSAGE", "missing value for --out");
        return kExitUsage;
      }
      outPath = args[++i];
    } else if (a == "--clip") {
      clip = true;
    } else if (a == "--no-siblings") {
      siblings = false;
    } else if (a == "--json") {
      // already handled
    } else if (a.rfind("--", 0) == 0) {
      reportFailure(json, out, err, "EUSAGE", "unknown argument: " + a);
      return kExitUsage;
    } else {
      positional.push_back(a);
    }
  }
  if (positional.size() != 2) {
    reportFailure(json, out, err, "EUSAGE", "merge expects TRACE.tachyon and INST.tachyon-inst");
    return kExitUsage;
  }
  const fs::path tracePath = positional[0];
  const fs::path instPath = positional[1];
  std::error_code ec;
  if (!fs::is_regular_file(tracePath, ec)) {
    reportFailure(json, out, err, "ENOTFOUND", "trace not found: " + tracePath.string());
    return kExitProtocolError;
  }
  if (!fs::is_regular_file(instPath, ec)) {
    reportFailure(json, out, err, "ENOTFOUND", "instrumentation file not found: " + instPath.string());
    return kExitProtocolError;
  }
  std::vector<inst::MergeInput> inputs;
  for (const fs::path& file :
       siblings ? inst::expandSidecarSiblings(instPath) : std::vector<fs::path>{instPath})
    inputs.push_back({file, ~uint64_t{0}});
  inst::MergeOptions options;
  options.outPath = outPath;
  options.clipToTraceRange = clip;
  auto merged = inst::mergeInstrumentation(tracePath, inputs, options);
  if (!merged.ok()) {
    reportFailure(json, out, err, "EINTERNAL", "merge failed: " + merged.error().toString());
    return kExitProtocolError;
  }
  const inst::MergeReport& r = *merged;
  if (json) {
    out << "{\"ok\":true,\"output\":\"" << jsonEscape(r.outputPath.string())
        << "\",\"written\":" << (r.wroteOutput ? "true" : "false") << ",\"inputs\":[";
    for (size_t i = 0; i < inputs.size(); ++i)
      out << (i ? "," : "") << "\"" << jsonEscape(inputs[i].file.string()) << "\"";
    out << "],\"streams\":" << r.streams.size() << ",\"scope_events\":" << r.scopeEvents
        << ",\"fiber_events\":" << r.fiberEvents << ",\"frame_marks\":" << r.frameMarks
        << ",\"markers\":" << r.markers << ",\"plot_samples\":" << r.plots
        << ",\"thread_names\":" << r.threadNames << ",\"dropped_records\":" << r.decode.droppedRecords
        << ",\"clipped_records\":" << r.clippedRecords
        << ",\"unmatched_scope_ends\":" << r.decode.unmatchedScopeEnds
        << ",\"unclosed_scopes\":" << r.decode.unclosedScopes << ",\"warnings\":[";
    for (size_t i = 0; i < r.warnings.size(); ++i)
      out << (i ? "," : "") << "\"" << jsonEscape(r.warnings[i]) << "\"";
    out << "]}\n";
    return kExitOk;
  }
  if (r.wroteOutput) {
    out << "merged " << inputs.size() << " instrumentation file(s) into " << r.outputPath.string() << ": "
        << r.scopeEvents << " scope events, " << r.fiberEvents << " fiber events, " << r.frameMarks
        << " frame marks, " << r.markers << " markers, " << r.plots << " plot samples, " << r.threadNames
        << " thread names\n";
  } else {
    out << "nothing merged; " << r.outputPath.string() << " is unchanged\n";
  }
  for (const std::string& w : r.warnings) err << "tachyon-cli: warning: " << w << "\n";
  return kExitOk;
}

// Index of the command word in args (skipping the global options and their values), or npos.
size_t commandIndex(const std::vector<std::string>& args) {
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--socket" || a == "--pipe" || a == "--timeout" || a == "--connect-timeout-ms") {
      ++i;
    } else if (a.rfind("--", 0) != 0) {
      return i;
    }
  }
  return std::string::npos;
}

}  // namespace

int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  if (const size_t at = commandIndex(args); at != std::string::npos && args[at] == "merge") {
    const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
    return runMerge(std::vector<std::string>(args.begin() + static_cast<std::ptrdiff_t>(at) + 1, args.end()),
                    json, out, err);
  }
  Parsed p;
  if (const std::string e = parse(args, p); !e.empty()) {
    // parse() may have stopped before it saw --json, so look for it directly.
    const bool json = std::find(args.begin(), args.end(), "--json") != args.end();
    reportFailure(json, out, err, "EUSAGE", e);
    printUsage(err);
    return kExitUsage;
  }
  if (p.address.empty()) p.address = defaultAgentAddress();

  auto client = Client::connectWithRetry(p.address, p.connectTimeoutMs);
  if (!client.ok()) {
    reportFailure(p.json, out, err, "ECONNECT",
                  "cannot connect to agent at " + p.address + ": " + client.error().message);
    return kExitConnectionFailure;
  }

  auto resp = client->call(p.request);
  if (!resp.ok()) {
    reportFailure(p.json, out, err, codeFor(resp.error()), resp.error().message);
    return kExitConnectionFailure;
  }
  const bool capturing = p.command == "capture";
  if (capturing && !p.json && resp->ok) {
    out << "capture started\n";
  } else {
    printResponse(*resp, p.json, out, err);
  }
  if (!resp->ok) return kExitProtocolError;
  if (capturing) return runCapture(*client, p, out, err);
  if (!p.wait) return kExitOk;

  auto event = client->waitForEvent(isFinal, p.timeoutS * 1000);
  if (!event.ok()) {
    reportFailure(p.json, out, err, codeFor(event.error()),
                  "waiting for capture_done: " + event.error().message);
    return kExitConnectionFailure;
  }
  return finish(*event, p.json, out, err);
}

}  // namespace tachyon::cli
