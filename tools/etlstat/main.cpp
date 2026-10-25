// etlstat: ETW spike tool. `record` captures a kernel session to an ETL file; `stats` analyses one;
// `replay` analyses a hex-event text file (any platform).
#include <fmt/format.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "etw_hex.h"
#include "etw_platform.h"
#include "etw_stats.h"
#include "report.h"
#include "session_config.h"

namespace {

using namespace tachyon;
using namespace tachyon::etlstat;

int fail(const Error& error) {
  std::fprintf(stderr, "etlstat: %s\n", error.message.c_str());
  return error.code == ErrorCode::InvalidArgument ? 2 : 1;
}

void printReport(const StatsSnapshot& snap, const TraceFileInfo& info, const CliOptions& cli) {
  ReportOptions ro;
  ro.expectedHz = cli.hz;
  ro.pid = cli.pid;
  ro.image = cli.image;
  const Report report = buildReport(snap, info, ro);
  const std::string text =
      cli.json ? formatReportJson(snap, info, report, ro) : formatReportText(snap, info, report, ro);
  std::fputs(text.c_str(), stdout);
  if (cli.json) std::fputc('\n', stdout);
}

int runRecord(const CliOptions& cli) {
  auto result = recordEtl(cli);
  if (!result.ok()) return fail(result.error());
  const RecordSummary& s = *result;
  const double hz = intervalUnitsToHz(s.profileIntervalUnits);
  fmt::print("recorded {:.2f} s to {} ({} bytes)\n", s.recordedSeconds, cli.outPath, s.fileBytes);
  fmt::print("logical cores: {}  interval: {} x100ns ({:.1f} Hz per core)\n", s.logicalCores,
             s.profileIntervalUnits, hz);
  fmt::print("interval set with session handle: {}  stack tracing enabled: {}  unique session GUID: {}\n",
             s.intervalSetWithSessionHandle ? "yes" : "no (handle 0 worked)",
             s.stackTracingEnabled ? "yes" : "no",
             s.sessionGuidWasUnique ? "yes" : "no (fell back to SystemTraceControlGuid)");
  fmt::print("buffers: written={} total={}\n", s.buffersWritten, s.numberOfBuffers);
  fmt::print("EventsLost={} LogBuffersLost={} RealTimeBuffersLost={}\n", s.eventsLost, s.logBuffersLost,
             s.realTimeBuffersLost);
  if (s.exePid != 0) {
    fmt::print("exe pid {} ({}; exit code {})\n", s.exePid,
               s.exeStillRunningAtStop ? "terminated at end of recording" : "exited on its own",
               s.exeExitCode);
    fmt::print("analyse with: etlstat stats {} --pid {}\n", cli.outPath, s.exePid);
  }
  return 0;
}

int runStats(const CliOptions& cli) {
  StatsAccumulator accumulator;
  TraceFileInfo info;
  std::ofstream dumpFile;
  HexDumpSelector selector(cli.dumpHexCount);
  const bool dumping = cli.dumpHexCount > 0;
  if (dumping) {
    const std::string dumpPath = cli.dumpOutPath.empty() ? cli.inputPath + ".hexevents" : cli.dumpOutPath;
    dumpFile.open(dumpPath, std::ios::binary | std::ios::trunc);
    if (!dumpFile) return fail(makeError(ErrorCode::IoError, "cannot write " + dumpPath));
    dumpFile << "# etlstat hex dump of " << cli.inputPath << "; first " << cli.dumpHexCount
             << " events per (provider, opcode, version)\n";
  }
  auto result = consumeEtl(cli.inputPath, accumulator, info, dumping ? &selector : nullptr,
                           dumping ? &dumpFile : nullptr);
  if (!result.ok()) return fail(result.error());
  accumulator.finish();
  printReport(accumulator.snapshot(), info, cli);
  return 0;
}

int runReplay(const CliOptions& cli) {
  auto events = loadHexEventFile(cli.inputPath);
  if (!events.ok()) return fail(events.error());
  StatsAccumulator accumulator;
  for (const HexEvent& e : *events) accumulator.add(e.view());
  accumulator.finish();
  printReport(accumulator.snapshot(), TraceFileInfo{}, cli);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
  auto parsed = parseCli(args);
  if (!parsed.ok()) {
    std::fprintf(stderr, "etlstat: %s\n\n%s", parsed.error().message.c_str(), usageText().c_str());
    return 2;
  }
  switch (parsed->command) {
    case Command::Help: std::fputs(usageText().c_str(), stdout); return 0;
    case Command::Record: return runRecord(*parsed);
    case Command::Stats: return runStats(*parsed);
    case Command::Replay: return runReplay(*parsed);
  }
  return 2;
}
