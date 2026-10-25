#include "cli.h"

#include <charconv>

namespace tachyon::etlstat {

namespace {
Error badArg(std::string message) {
  return makeError(ErrorCode::InvalidArgument, std::move(message));
}

bool parseU32(std::string_view text, uint32_t& out) {
  uint32_t v = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || ptr != text.data() + text.size()) return false;
  out = v;
  return true;
}
}  // namespace

std::string usageText() {
  return "usage:\n"
         "  etlstat record --out x.etl [--seconds N] [--hz 8000] [--exe path [--exe-args \"...\"]]\n"
         "      Windows only, elevated. Records a kernel ETW session (PROCESS|THREAD|IMAGE_LOAD|PROFILE|\n"
         "      CSWITCH|DISPATCHER, QPC clock, stack walks for SampledProfile/CSwitch/ReadyThread).\n"
         "  etlstat stats x.etl [--hz N] [--json] [--dump-hex N [--dump-out file]] [--pid N] [--image name]\n"
         "      Windows only. Counts events per (provider, opcode, version), matches StackWalk events to\n"
         "      their parents, reports samples/s per CPU, lost events, depth and SetName presence.\n"
         "  etlstat replay x.hexevents [--hz N] [--json] [--pid N] [--image name]\n"
         "      Same report from a hex-event text file (any platform).\n";
}

Result<CliOptions> parseCli(std::span<const std::string_view> args) {
  CliOptions o;
  if (args.empty()) return o;
  const std::string_view cmd = args[0];
  if (cmd == "help" || cmd == "--help" || cmd == "-h") return o;
  if (cmd == "record") {
    o.command = Command::Record;
  } else if (cmd == "stats") {
    o.command = Command::Stats;
  } else if (cmd == "replay") {
    o.command = Command::Replay;
  } else {
    return badArg("unknown command: " + std::string(cmd));
  }

  for (size_t i = 1; i < args.size(); ++i) {
    const std::string_view a = args[i];
    auto value = [&](std::string_view name) -> Result<std::string_view> {
      if (i + 1 >= args.size()) return badArg("missing value for " + std::string(name));
      return args[++i];
    };
    auto numeric = [&](std::string_view name, uint32_t& out) -> Result<void> {
      auto v = value(name);
      if (!v.ok()) return v.error();
      if (!parseU32(*v, out))
        return badArg("invalid number for " + std::string(name) + ": " + std::string(*v));
      return ok();
    };
    if (a == "--out" && o.command == Command::Record) {
      auto v = value(a);
      if (!v.ok()) return v.error();
      o.outPath = std::string(*v);
    } else if (a == "--seconds" && o.command == Command::Record) {
      TACHYON_TRY(numeric(a, o.seconds));
    } else if (a == "--exe" && o.command == Command::Record) {
      auto v = value(a);
      if (!v.ok()) return v.error();
      o.exePath = std::string(*v);
    } else if (a == "--exe-args" && o.command == Command::Record) {
      auto v = value(a);
      if (!v.ok()) return v.error();
      o.exeArgs = std::string(*v);
    } else if (a == "--hz") {
      TACHYON_TRY(numeric(a, o.hz));
    } else if (a == "--json" && o.command != Command::Record) {
      o.json = true;
    } else if (a == "--dump-hex" && o.command == Command::Stats) {
      TACHYON_TRY(numeric(a, o.dumpHexCount));
    } else if (a == "--dump-out" && o.command == Command::Stats) {
      auto v = value(a);
      if (!v.ok()) return v.error();
      o.dumpOutPath = std::string(*v);
    } else if (a == "--pid" && o.command != Command::Record) {
      uint32_t pid = 0;
      TACHYON_TRY(numeric(a, pid));
      o.pid = pid;
    } else if (a == "--image" && o.command != Command::Record) {
      auto v = value(a);
      if (!v.ok()) return v.error();
      o.image = std::string(*v);
    } else if (!a.empty() && a[0] != '-' && o.command != Command::Record && o.inputPath.empty()) {
      o.inputPath = std::string(a);
    } else {
      return badArg("unknown or misplaced argument: " + std::string(a));
    }
  }

  if (o.command == Command::Record) {
    if (o.outPath.empty()) return badArg("record requires --out <file.etl>");
    if (o.seconds == 0) return badArg("--seconds must be > 0");
    if (o.hz == 0) return badArg("--hz must be > 0");
    if (!o.exeArgs.empty() && o.exePath.empty()) return badArg("--exe-args requires --exe");
  } else if (o.inputPath.empty()) {
    return badArg(std::string(o.command == Command::Stats ? "stats" : "replay") + " requires an input file");
  }
  return o;
}

}  // namespace tachyon::etlstat
