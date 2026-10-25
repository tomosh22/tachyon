// Windows-only: records 2 s of kernel ETW with etlstat's own session code and analyses it. Skipped when the
// process lacks SeSystemProfilePrivilege (a non-elevated shell); never asserts the 8 kHz acceptance numbers,
// which need the dedicated >= 8 core machine (see docs/etw-notes.md).
#include <gtest/gtest.h>

#include <filesystem>
#include <iostream>

#include "etw_platform.h"
#include "report.h"

using namespace tachyon::etlstat;

TEST(EtlstatWindows, RecordAndConsumeRoundTrip) {
  const std::string path = std::string(TACHYON_BINARY_DIR) + "/etlstat_roundtrip.etl";
  std::error_code ec;
  std::filesystem::remove(path, ec);

  CliOptions opts;
  opts.command = Command::Record;
  opts.outPath = path;
  opts.seconds = 2;
  opts.hz = 8000;
  auto rec = recordEtl(opts);
  if (!rec.ok() && rec.error().code == tachyon::ErrorCode::NoPrivilege) {
    GTEST_SKIP() << "not elevated: " << rec.error().message;
  }
  ASSERT_TRUE(rec.ok()) << rec.error().toString();
  std::cout << "[etlstat] interval units=" << rec->profileIntervalUnits
            << " sessionHandleForInterval=" << rec->intervalSetWithSessionHandle
            << " stackTracing=" << rec->stackTracingEnabled << " eventsLost=" << rec->eventsLost
            << " logBuffersLost=" << rec->logBuffersLost << " fileBytes=" << rec->fileBytes << "\n";
  EXPECT_TRUE(rec->stackTracingEnabled);
  EXPECT_GT(rec->fileBytes, 0u);

  StatsAccumulator acc;
  TraceFileInfo info;
  auto consumed = consumeEtl(path, acc, info, nullptr, nullptr);
  ASSERT_TRUE(consumed.ok()) << consumed.error().toString();
  acc.finish();
  const StatsSnapshot& s = acc.snapshot();
  EXPECT_TRUE(info.valid);
  EXPECT_GT(info.perfFreq, 0u);
  EXPECT_GT(s.totalEvents, 0u);
  EXPECT_GT(s.sampledProfile, 0u);
  EXPECT_GT(s.stackEvents, 0u);

  ReportOptions ro;
  const Report report = buildReport(s, info, ro);
  std::cout << formatReportText(s, info, report, ro);
  std::filesystem::remove(path, ec);
}
