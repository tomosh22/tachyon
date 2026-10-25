#include "cli/cli.h"

#include "control/client.h"
#include "control/protocol.h"
#include "control/transport.h"

#include <charconv>
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
  bool wait = false;
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

// Parses args into `p`. Returns an error message on usage errors, empty string on success.
std::string parse(const std::vector<std::string>& args, Parsed& p) {
  StartRequest start;
  FlushRequest flush;
  bool haveMode = false, haveName = false, haveSeconds = false;

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
    } else if (p.command == "start" && a == "--mode") {
      if (!needValue(v)) return "missing value for --mode";
      auto m = capture::parseCaptureMode(v);
      if (!m) return "--mode must be launch, attach or system";
      start.mode = *m;
      haveMode = true;
    } else if (p.command == "start" && a == "--exe") {
      if (!needValue(start.exe)) return "missing value for --exe";
    } else if (p.command == "start" && a == "--cwd") {
      if (!needValue(start.cwd)) return "missing value for --cwd";
    } else if (p.command == "start" && a == "--arg") {
      if (!needValue(v)) return "missing value for --arg";
      start.args.push_back(v);
    } else if (p.command == "start" && a == "--args") {
      // Consumes every following token up to the next "--option".
      while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) start.args.push_back(args[++i]);
    } else if (p.command == "start" && a == "--env") {
      if (!needValue(v)) return "missing value for --env";
      const size_t eq = v.find('=');
      if (eq == std::string::npos || eq == 0) return "--env expects KEY=VALUE";
      start.env[v.substr(0, eq)] = v.substr(eq + 1);
    } else if (p.command == "start" && a == "--pid") {
      if (!needUint(start.pid, err)) return err;
    } else if (p.command == "start" && (a == "--hz" || a == "--sampling-hz")) {
      if (!needUint(start.sampling_hz, err)) return err;
    } else if (p.command == "start" && a == "--follow-children") {
      start.follow_children = true;
    } else if (p.command == "start" && a == "--child-filter") {
      if (!needValue(start.child_filter)) return "missing value for --child-filter";
    } else if (p.command == "start" && a == "--circular-mb") {
      if (!needUint(start.circular_mb, err)) return err;
    } else if (p.command == "start" && (a == "--out" || a == "--out-path")) {
      if (!needValue(start.out_path)) return "missing value for " + a;
    } else if (p.command == "start" && a == "--defer-start") {
      start.start_immediately = false;
    } else if (p.command == "start" && a == "--no-profile") {
      start.stackwalk.profile = false;
    } else if (p.command == "start" && a == "--no-cswitch") {
      start.stackwalk.cswitch = false;
    } else if (p.command == "start" && a == "--no-readythread") {
      start.stackwalk.readythread = false;
    } else if (p.command == "start" && a == "--no-kernel-stacks") {
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
  } else if (p.command == "start") {
    if (!haveMode) return "start requires --mode";
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
  return {};
}

void printUsage(std::ostream& err) {
  err << "usage: tachyon-cli [--socket PATH | --pipe NAME] [--json] [--timeout SECONDS] <command> [options]\n"
         "commands:\n"
         "  status\n"
         "  start --mode launch|attach|system [--exe E] [--args A...] [--arg A] [--cwd D] [--env K=V]\n"
         "        [--pid N] [--hz N] [--follow-children] [--child-filter G] [--circular-mb N] [--out PATH]\n"
         "        [--defer-start] [--no-profile] [--no-cswitch] [--no-readythread] [--no-kernel-stacks] "
         "[--wait]\n"
         "  stop [--wait]\n"
         "  flush --name N --seconds S\n"
         "  cancel\n";
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

}  // namespace

int run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
  Parsed p;
  if (const std::string e = parse(args, p); !e.empty()) {
    err << "tachyon-cli: " << e << "\n";
    printUsage(err);
    return kExitUsage;
  }
  if (p.address.empty()) p.address = defaultAgentAddress();

  auto client = Client::connectWithRetry(p.address, p.connectTimeoutMs);
  if (!client.ok()) {
    err << "tachyon-cli: cannot connect to agent at " << p.address << ": " << client.error().message << "\n";
    return kExitConnectionFailure;
  }

  auto resp = client->call(p.request);
  if (!resp.ok()) {
    err << "tachyon-cli: " << resp.error().message << "\n";
    return kExitConnectionFailure;
  }
  printResponse(*resp, p.json, out, err);
  if (!resp->ok) return kExitProtocolError;
  if (!p.wait) return kExitOk;

  auto event = client->waitForEvent(
      [](const Event& e) {
        return std::holds_alternative<CaptureDoneEvent>(e) || std::holds_alternative<ErrorEvent>(e);
      },
      p.timeoutS * 1000);
  if (!event.ok()) {
    err << "tachyon-cli: waiting for capture_done: " << event.error().message << "\n";
    return kExitConnectionFailure;
  }
  if (const auto* ee = std::get_if<ErrorEvent>(&*event)) {
    if (p.json) {
      out << encodeEvent(*event) << "\n";
    } else {
      err << "error: " << ee->message << "\n";
    }
    return kExitProtocolError;
  }
  printEvent(*event, p.json, out);
  return kExitOk;
}

}  // namespace tachyon::cli
