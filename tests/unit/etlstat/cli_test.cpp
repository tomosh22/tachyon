#include <gtest/gtest.h>

#include <string_view>
#include <vector>

#include "cli.h"
#include "session_config.h"

using namespace tachyon::etlstat;

namespace {
tachyon::Result<CliOptions> parse(std::vector<std::string_view> args) {
  return parseCli(std::span<const std::string_view>(args.data(), args.size()));
}
}  // namespace

TEST(Cli, NoArgumentsMeansHelp) {
  const auto r = parse({});
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->command, Command::Help);
}

TEST(Cli, ParsesRecordWithAllOptions) {
  const auto r = parse({"record", "--out", "x.etl", "--seconds", "30", "--hz", "8000", "--exe", "bin/app.exe",
                        "--exe-args", "--frames 100"});
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->command, Command::Record);
  EXPECT_EQ(r->outPath, "x.etl");
  EXPECT_EQ(r->seconds, 30u);
  EXPECT_EQ(r->hz, 8000u);
  EXPECT_EQ(r->exePath, "bin/app.exe");
  EXPECT_EQ(r->exeArgs, "--frames 100");
}

TEST(Cli, RecordDefaults) {
  const auto r = parse({"record", "--out", "a.etl"});
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->seconds, 10u);
  EXPECT_EQ(r->hz, 8000u);
  EXPECT_TRUE(r->exePath.empty());
}

TEST(Cli, RecordValidation) {
  EXPECT_FALSE(parse({"record"}).ok());
  EXPECT_FALSE(parse({"record", "--out"}).ok());
  EXPECT_FALSE(parse({"record", "--out", "a.etl", "--seconds", "0"}).ok());
  EXPECT_FALSE(parse({"record", "--out", "a.etl", "--seconds", "abc"}).ok());
  EXPECT_FALSE(parse({"record", "--out", "a.etl", "--hz", "0"}).ok());
  EXPECT_FALSE(parse({"record", "--out", "a.etl", "--exe-args", "x"}).ok());
  EXPECT_FALSE(parse({"record", "--out", "a.etl", "--json"}).ok());
}

TEST(Cli, ParsesStats) {
  const auto r = parse({"stats", "x.etl", "--json", "--dump-hex", "3", "--dump-out", "d.txt", "--pid", "42",
                        "--image", "game.exe", "--hz", "4000"});
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->command, Command::Stats);
  EXPECT_EQ(r->inputPath, "x.etl");
  EXPECT_TRUE(r->json);
  EXPECT_EQ(r->dumpHexCount, 3u);
  EXPECT_EQ(r->dumpOutPath, "d.txt");
  ASSERT_TRUE(r->pid.has_value());
  EXPECT_EQ(*r->pid, 42u);
  EXPECT_EQ(r->image, "game.exe");
  EXPECT_EQ(r->hz, 4000u);
}

TEST(Cli, StatsAndReplayNeedAnInput) {
  EXPECT_FALSE(parse({"stats"}).ok());
  EXPECT_FALSE(parse({"replay", "--json"}).ok());
  EXPECT_FALSE(parse({"stats", "a.etl", "b.etl"}).ok());
  EXPECT_FALSE(parse({"replay", "a.hex", "--dump-hex", "2"}).ok());  // dump is stats-only
  EXPECT_TRUE(parse({"replay", "a.hex", "--json"}).ok());
}

TEST(Cli, UnknownCommandsAndOptionsAreInvalidArgument) {
  const auto a = parse({"frobnicate"});
  ASSERT_FALSE(a.ok());
  EXPECT_EQ(a.error().code, tachyon::ErrorCode::InvalidArgument);
  EXPECT_FALSE(parse({"stats", "a.etl", "--wat"}).ok());
  EXPECT_FALSE(parse({"stats", "a.etl", "--pid"}).ok());
}

TEST(Cli, UsageMentionsAllCommands) {
  const std::string u = usageText();
  EXPECT_NE(u.find("record"), std::string::npos);
  EXPECT_NE(u.find("stats"), std::string::npos);
  EXPECT_NE(u.find("replay"), std::string::npos);
}

TEST(SessionConfig, BufferFormula) {
  EXPECT_EQ(computeBufferSettings(4).bufferSizeKb, 1024u);
  EXPECT_EQ(computeBufferSettings(4).minimumBuffers, 64u);
  EXPECT_EQ(computeBufferSettings(8).minimumBuffers, 64u);
  EXPECT_EQ(computeBufferSettings(16).minimumBuffers, 64u);
  EXPECT_EQ(computeBufferSettings(32).minimumBuffers, 128u);
  EXPECT_EQ(computeBufferSettings(64).minimumBuffers, 256u);
  EXPECT_EQ(computeBufferSettings(8).maximumBuffers, 256u);
  EXPECT_EQ(computeBufferSettings(64).maximumBuffers, 1024u);
  EXPECT_GE(computeBufferSettings(1).maximumBuffers, computeBufferSettings(1).minimumBuffers);
}

TEST(SessionConfig, ProfileInterval) {
  EXPECT_EQ(profileIntervalUnits(8000), 1221u);
  EXPECT_EQ(profileIntervalUnits(20000), 1221u);
  EXPECT_EQ(profileIntervalUnits(1000), 10000u);
  EXPECT_EQ(profileIntervalUnits(100), 100000u);
  EXPECT_EQ(profileIntervalUnits(0), 10000000u);
  EXPECT_NEAR(intervalUnitsToHz(1221), 8190.0, 1.0);
  EXPECT_DOUBLE_EQ(intervalUnitsToHz(10000), 1000.0);
}

TEST(SessionConfig, EnableFlagsComposition) {
  EXPECT_EQ(kEnableFlags, 0x01000817u);
}
