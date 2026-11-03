// tachyon-testbed: runs known-answer scenarios and prints the expected/measured split as JSON on stdout.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "testbed/calibration.h"
#include "testbed/hooks.h"
#include "testbed/report.h"
#include "testbed/scenarios.h"

namespace {

using namespace tachyon::testbed;

void printUsage() {
  std::fputs(
      "usage: tachyon-testbed [--scenario <name[,name...]|all>] [--duration <seconds>] [--hitch-interval-ms "
      "<ms>]\n"
      "                       [--instrument] [--flush-on-hitch] [--pretty] [--list] [--help]\n"
      "  Scenarios run one after another; with several, --duration is split across them (frames gets 4x "
      "weight).\n"
      "  The expected/measured split is printed as JSON on stdout; diagnostics go to stderr.\n"
      "  --instrument emits Tachyon SDK events (scopes, thread names, frame marks, markers, plots, lock "
      "waits,\n"
      "  fiber switches) to the agent named by $TACHYON_AGENT_PIPE, or to a sidecar .tachyon-inst file.\n"
      "  --flush-on-hitch (needs --instrument) makes the frames scenario call "
      "Tachyon_RequestFlush(\"hitch\")\n"
      "  right after each planted hitch frame ended (flight recorder trigger; the agent needs "
      "--circular-mb).\n",
      stderr);
}

std::vector<std::string> splitCommas(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(',', start);
    const size_t end = comma == std::string::npos ? text.size() : comma;
    if (end > start) out.push_back(text.substr(start, end - start));
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenarioArg = "all";
  double duration = 5.0;
  double hitchIntervalMs = 0.0;  // 0 = default (2000 ms; scaled to fit when several scenarios run)
  bool pretty = false;
  bool instrument = false;
  bool flushOnHitch = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto needValue = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s needs a value\n", flag);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--scenario") {
      const char* value = needValue("--scenario");
      if (value == nullptr) return 2;
      scenarioArg = value;
    } else if (arg == "--duration") {
      const char* value = needValue("--duration");
      if (value == nullptr) return 2;
      duration = std::atof(value);
    } else if (arg == "--hitch-interval-ms") {
      const char* value = needValue("--hitch-interval-ms");
      if (value == nullptr) return 2;
      hitchIntervalMs = std::atof(value);
    } else if (arg == "--pretty") {
      pretty = true;
    } else if (arg == "--instrument") {
      instrument = true;
    } else if (arg == "--flush-on-hitch") {
      flushOnHitch = true;
    } else if (arg == "--list") {
      for (const auto& name : scenarioNames()) std::printf("%s\n", name.c_str());
      return 0;
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", arg.c_str());
      printUsage();
      return 2;
    }
  }
  if (!(duration > 0.0)) {
    std::fputs("--duration must be a positive number of seconds\n", stderr);
    return 2;
  }

  std::vector<std::string> names = scenarioArg == "all" ? scenarioNames() : splitCommas(scenarioArg);
  if (names.empty()) {
    std::fputs("no scenario selected\n", stderr);
    return 2;
  }

  if (flushOnHitch && !instrument) {
    std::fputs("--flush-on-hitch needs --instrument\n", stderr);
    return 2;
  }
  if (instrument) {
    setInstrumentation(true);
    setFlushOnHitch(flushOnHitch);
    Tachyon_SetCurrentThreadName(
        "tachyon-testbed");  // first SDK call: starts the runtime and the agent connection
    std::fputs("tachyon-testbed: instrumentation on\n", stderr);
  }

  const Calibration cal = calibrate();
  std::fprintf(stderr, "tachyon-testbed: tsc %.3f GHz, kernel %.1f Miter/s\n",
               static_cast<double>(cal.tscHz) / 1e9, cal.itersPerSecond() / 1e6);
  auto run = runScenarios(names, duration, hitchIntervalMs, cal);
  if (!run.ok()) {
    std::fprintf(stderr, "tachyon-testbed: %s\n", run.error().toString().c_str());
    return 1;
  }
  const auto json = toJson(run.value());
  std::vector<std::string> warnings;
  if (auto valid = validateReportJson(json, &warnings); !valid.ok()) {
    std::fprintf(stderr, "tachyon-testbed: internal error, report fails its own schema: %s\n",
                 valid.error().toString().c_str());
    return 1;
  }
  for (const std::string& warning : warnings)
    std::fprintf(stderr, "tachyon-testbed: warning: %s\n", warning.c_str());
  const std::string text = json.dump(pretty ? 2 : -1);
  std::fprintf(stdout, "%s\n", text.c_str());
  return 0;
}
