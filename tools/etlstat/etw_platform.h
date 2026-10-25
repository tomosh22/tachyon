// The only platform-specific surface of etlstat: the advapi32 ETW session (record) and consumer (stats).
// On non-Windows builds every function returns ErrorCode::Unsupported ("requires Windows").
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#include "cli.h"
#include "core/result.h"
#include "etw_hex.h"
#include "etw_stats.h"
#include "report.h"

namespace tachyon::etlstat {

struct RecordSummary {
  uint32_t exePid = 0;  // 0 when --exe was not used
  bool exeStillRunningAtStop = false;
  uint32_t exeExitCode = 0;
  uint32_t logicalCores = 0;
  uint32_t profileIntervalUnits = 0;
  bool intervalSetWithSessionHandle = false;  // false: needed handle 0
  bool stackTracingEnabled = false;
  bool sessionGuidWasUnique = true;  // false: fell back to SystemTraceControlGuid
  uint32_t eventsLost = 0;
  uint32_t logBuffersLost = 0;
  uint32_t realTimeBuffersLost = 0;
  uint32_t buffersWritten = 0;
  uint32_t numberOfBuffers = 0;
  double recordedSeconds = 0;
  uint64_t fileBytes = 0;
};

// Not thread-safe (process-wide ETW session). Requires an elevated process.
Result<RecordSummary> recordEtl(const CliOptions& options);

// Not thread-safe. Reads `path` with OpenTrace/ProcessTrace and feeds `accumulator`; fills `info` from the
// logfile header. When `dump` is non-null, events selected by `selector` are written to it in hex-event form.
Result<void> consumeEtl(const std::string& path, StatsAccumulator& accumulator, TraceFileInfo& info,
                        HexDumpSelector* selector, std::ostream* dump);

// Thread-safe: pure function. True when this build can record/consume ETL files.
bool etwSupported();

}  // namespace tachyon::etlstat
