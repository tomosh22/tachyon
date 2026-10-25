#include <gtest/gtest.h>

#include "core/platform.h"
#include "etw_platform.h"

using namespace tachyon::etlstat;

#if !TACHYON_PLATFORM_WINDOWS
TEST(Platform, RecordAndStatsReportRequiresWindowsOnLinux) {
  EXPECT_FALSE(etwSupported());
  CliOptions opts;
  opts.command = Command::Record;
  opts.outPath = "x.etl";
  const auto rec = recordEtl(opts);
  ASSERT_FALSE(rec.ok());
  EXPECT_EQ(rec.error().code, tachyon::ErrorCode::Unsupported);
  EXPECT_NE(rec.error().message.find("requires Windows"), std::string::npos);

  StatsAccumulator acc;
  TraceFileInfo info;
  const auto stats = consumeEtl("x.etl", acc, info, nullptr, nullptr);
  ASSERT_FALSE(stats.ok());
  EXPECT_EQ(stats.error().code, tachyon::ErrorCode::Unsupported);
  EXPECT_NE(stats.error().message.find("requires Windows"), std::string::npos);
}
#else
TEST(Platform, ConsumingMissingEtlFailsCleanly) {
  EXPECT_TRUE(etwSupported());
  StatsAccumulator acc;
  TraceFileInfo info;
  const auto r = consumeEtl("this-file-does-not-exist.etl", acc, info, nullptr, nullptr);
  EXPECT_FALSE(r.ok());
}
#endif
