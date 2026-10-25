// Platform-independent parts of the ETW session controller: sizing, flags, naming, orphan matching, config.
#include "capture/windows_etw/session_config.h"

#include "etlconvert/hex_events.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <set>

namespace tachyon::capture::etw {
namespace {

// ---- buffer sizing ----------------------------------------------------------------------------------------

TEST(BufferSizing, DefaultsMatchTheWp01Recipe) {
  const BufferSettings s = computeBufferSettings(8, 8000);
  EXPECT_EQ(s.bufferSizeKb, 1024u);
  EXPECT_EQ(s.minimumBuffers, 64u);   // max(64, 4 x 8 cores)
  EXPECT_EQ(s.maximumBuffers, 256u);  // 4x at 8 kHz
  EXPECT_EQ(s.flushTimerSeconds, 1u);
}

TEST(BufferSizing, MinimumScalesWithCoresAboveSixteen) {
  EXPECT_EQ(computeBufferSettings(1, 8000).minimumBuffers, 64u);
  EXPECT_EQ(computeBufferSettings(16, 8000).minimumBuffers, 64u);
  EXPECT_EQ(computeBufferSettings(17, 8000).minimumBuffers, 68u);
  EXPECT_EQ(computeBufferSettings(32, 8000).minimumBuffers, 128u);
  EXPECT_EQ(computeBufferSettings(32, 8000).maximumBuffers, 512u);  // 128 x 4 = the cap
}

TEST(BufferSizing, MaximumScalesWithSamplingRateAndIsCapped) {
  EXPECT_EQ(computeBufferSettings(8, 1000).maximumBuffers, 64u * 3);  // 2 + ceil(1000 / 4000)
  EXPECT_EQ(computeBufferSettings(8, 4000).maximumBuffers, 64u * 3);
  EXPECT_EQ(computeBufferSettings(8, 4001).maximumBuffers, 64u * 4);
  EXPECT_EQ(computeBufferSettings(8, 8000).maximumBuffers, 64u * 4);
  EXPECT_EQ(computeBufferSettings(8, 16000).maximumBuffers, 64u * 6);
  EXPECT_EQ(computeBufferSettings(64, 16000).maximumBuffers, kMaxBufferCountCap);
  // 0 means the default rate.
  EXPECT_EQ(computeBufferSettings(8, 0).maximumBuffers, computeBufferSettings(8, 8000).maximumBuffers);
}

TEST(BufferSizing, OverridesReplaceFormulaValuesAndAreClamped) {
  BufferOverrides o;
  o.bufferSizeKb = 2048;
  o.minimumBuffers = 100;
  o.maximumBuffers = 150;
  o.flushTimerSeconds = 5;
  BufferSettings s = computeBufferSettings(8, 8000, o);
  EXPECT_EQ(s.bufferSizeKb, 2048u);
  EXPECT_EQ(s.minimumBuffers, 100u);
  EXPECT_EQ(s.maximumBuffers, 150u);
  EXPECT_EQ(s.flushTimerSeconds, 5u);

  // Maximum below minimum is raised to the minimum; absurd sizes are clamped.
  BufferOverrides bad;
  bad.minimumBuffers = 200;
  bad.maximumBuffers = 10;
  bad.bufferSizeKb = 1u << 30;
  bad.flushTimerSeconds = 0;
  s = computeBufferSettings(8, 8000, bad);
  EXPECT_EQ(s.minimumBuffers, 200u);
  EXPECT_EQ(s.maximumBuffers, 200u);
  EXPECT_EQ(s.bufferSizeKb, 16384u);
  EXPECT_EQ(s.flushTimerSeconds, 1u);

  BufferOverrides tiny;
  tiny.minimumBuffers = 0;
  tiny.bufferSizeKb = 1;
  s = computeBufferSettings(8, 8000, tiny);
  EXPECT_EQ(s.minimumBuffers, 2u);
  EXPECT_EQ(s.bufferSizeKb, 64u);
}

// ---- interval and flags -----------------------------------------------------------------------------------

TEST(SamplingInterval, MapsHzToHundredNanosecondUnits) {
  EXPECT_EQ(profileIntervalUnits(8000), kMinProfileIntervalUnits);
  EXPECT_EQ(profileIntervalUnits(20000), kMinProfileIntervalUnits);
  EXPECT_EQ(profileIntervalUnits(0), kMinProfileIntervalUnits);  // default rate
  EXPECT_EQ(profileIntervalUnits(1000), 10000u);
  EXPECT_EQ(profileIntervalUnits(100), 100000u);
  EXPECT_EQ(profileIntervalUnits(7000), 1428u);
  EXPECT_NEAR(intervalUnitsToHz(1221), 8190.0, 1.0);
  EXPECT_DOUBLE_EQ(intervalUnitsToHz(0), 0.0);
}

TEST(EnableFlags, AlwaysProcessThreadImageProfileCSwitchDispatcher) {
  CaptureOptions o;
  EXPECT_EQ(enableFlagsFor(o), 0x01000817u);
  o.stackProfile = o.stackCSwitch = o.stackReady = false;
  EXPECT_EQ(enableFlagsFor(o), 0x01000817u);  // stack options never remove the events themselves
  EXPECT_EQ(kFlagProcess | kFlagThread | kFlagImageLoad | kFlagProfile | kFlagCSwitch | kFlagDispatcher,
            0x01000817u);
}

TEST(StackEvents, FollowTheStackOptions) {
  CaptureOptions o;
  auto ids = stackEventsFor(o);
  ASSERT_EQ(ids.size(), 3u);
  EXPECT_EQ(ids[0].provider, etlconvert::guids::kPerfInfo);
  EXPECT_EQ(ids[0].opcode, 46);
  EXPECT_EQ(ids[1].provider, etlconvert::guids::kThread);
  EXPECT_EQ(ids[1].opcode, 36);
  EXPECT_EQ(ids[2].provider, etlconvert::guids::kThread);
  EXPECT_EQ(ids[2].opcode, 50);

  o.stackCSwitch = false;
  ids = stackEventsFor(o);
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[1].opcode, 50);

  o.stackProfile = false;
  o.stackReady = false;
  EXPECT_TRUE(stackEventsFor(o).empty());
}

// ---- naming and orphan matching ---------------------------------------------------------------------------

TEST(SessionNaming, NameIsPidAndCounter) {
  EXPECT_EQ(makeSessionName(1234, 0), "Tachyon 1234 0");
  EXPECT_EQ(makeSessionName(4294967295u, 17), "Tachyon 4294967295 17");
}

TEST(SessionNaming, GuidsDifferPerPidCounterAndStamp) {
  std::set<std::string> seen;
  for (uint32_t pid : {100u, 101u})
    for (uint32_t counter : {0u, 1u, 70000u})
      for (uint64_t stamp : {0ull, 12345ull, 0xABCDEF0123ull})
        seen.insert(etlconvert::guidToString(makeSessionGuid(pid, counter, stamp)));
  EXPECT_EQ(seen.size(), 18u);
}

TEST(SessionNaming, ParsesOurNamesAndTheEtlstatTool) {
  auto a = parseSessionName("Tachyon 1234 7");
  EXPECT_TRUE(a.tachyon);
  EXPECT_EQ(a.pid, 1234u);
  EXPECT_EQ(a.counter, 7u);

  auto b = parseSessionName("Tachyon etlstat 99");
  EXPECT_TRUE(b.tachyon);
  EXPECT_EQ(b.pid, 99u);
  EXPECT_FALSE(b.counter.has_value());

  auto c = parseSessionName("Tachyon 42");
  EXPECT_EQ(c.pid, 42u);

  auto d = parseSessionName("Tachyon something");
  EXPECT_TRUE(d.tachyon);
  EXPECT_FALSE(d.pid.has_value());

  // pid 0, overflow and junk are not pids.
  EXPECT_FALSE(parseSessionName("Tachyon 0 1").pid.has_value());
  EXPECT_FALSE(parseSessionName("Tachyon 99999999999 1").pid.has_value());
  EXPECT_FALSE(parseSessionName("Tachyon 12x 1").pid.has_value());
}

TEST(SessionNaming, OnlyThePrefixFollowedBySpaceIsOurs) {
  EXPECT_FALSE(parseSessionName("NT Kernel Logger").tachyon);
  EXPECT_FALSE(parseSessionName("Tachyon").tachyon);  // no pid part at all
  EXPECT_FALSE(parseSessionName("TachyonX 12 1").tachyon);
  EXPECT_FALSE(parseSessionName("tachyon 12 1").tachyon);  // case-sensitive
  EXPECT_FALSE(parseSessionName("My Tachyon 12 1").tachyon);
  EXPECT_FALSE(parseSessionName("").tachyon);
}

TEST(OrphanMatching, ClassifiesByOwnerLiveness) {
  const auto alive = [](uint32_t pid) { return pid == 500 || pid == 600; };
  EXPECT_EQ(classifySession("Tachyon 400 0", alive), OrphanVerdict::Orphan);
  EXPECT_EQ(classifySession("Tachyon 500 3", alive), OrphanVerdict::Alive);  // live sibling agent
  EXPECT_EQ(classifySession("Tachyon etlstat 600", alive), OrphanVerdict::Alive);
  EXPECT_EQ(classifySession("Tachyon etlstat 601", alive), OrphanVerdict::Orphan);
  EXPECT_EQ(classifySession("Tachyon mystery", alive), OrphanVerdict::Unparseable);
  EXPECT_EQ(classifySession("NT Kernel Logger", alive), OrphanVerdict::NotTachyon);
  EXPECT_EQ(classifySession("EventLog-System", alive), OrphanVerdict::NotTachyon);
  EXPECT_EQ(classifySession("TachyonCorp 400 1", alive), OrphanVerdict::NotTachyon);
}

TEST(OrphanMatching, NeverAsksAboutNonTachyonSessions) {
  int calls = 0;
  const auto probe = [&](uint32_t) {
    ++calls;
    return false;
  };
  EXPECT_EQ(classifySession("Circular Kernel Context Logger", probe), OrphanVerdict::NotTachyon);
  EXPECT_EQ(classifySession("Tachyon nothing", probe), OrphanVerdict::Unparseable);
  EXPECT_EQ(calls, 0);
}

// ---- session config ---------------------------------------------------------------------------------------

CaptureOptions systemOptions() {
  CaptureOptions o;
  o.mode = CaptureMode::System;
  o.outPath = "capture.tachyon";
  return o;
}

TEST(SessionConfigTest, SequentialByDefault) {
  auto cfg = makeSessionConfig(systemOptions(), "Tachyon 1 0", makeSessionGuid(1, 0, 0), "a.etl", 8);
  ASSERT_TRUE(cfg.ok()) << cfg.error().toString();
  EXPECT_FALSE(cfg->circular);
  EXPECT_EQ(cfg->maxFileMb, 0u);
  EXPECT_EQ(cfg->name, "Tachyon 1 0");
  EXPECT_EQ(cfg->etlPath, "a.etl");
  EXPECT_EQ(cfg->enableFlags, 0x01000817u);
  EXPECT_EQ(cfg->profileIntervalUnits, kMinProfileIntervalUnits);
  EXPECT_EQ(cfg->stackEvents.size(), 3u);
  EXPECT_EQ(cfg->buffers.maximumBuffers, 256u);
}

TEST(SessionConfigTest, CircularMbSelectsCircularModeWithThatCap) {
  CaptureOptions o = systemOptions();
  o.circularMb = 256;
  auto cfg = makeSessionConfig(o, "n", makeSessionGuid(1, 0, 0), "a.etl", 8);
  ASSERT_TRUE(cfg.ok());
  EXPECT_TRUE(cfg->circular);
  EXPECT_EQ(cfg->maxFileMb, 256u);
}

TEST(SessionConfigTest, RejectsBadCircularSizesAndEmptyNames) {
  CaptureOptions o = systemOptions();
  o.circularMb = kMinCircularMb - 1;
  EXPECT_FALSE(makeSessionConfig(o, "n", makeSessionGuid(1, 0, 0), "a.etl", 8).ok());
  o.circularMb = kMaxCircularMb + 1;
  EXPECT_FALSE(makeSessionConfig(o, "n", makeSessionGuid(1, 0, 0), "a.etl", 8).ok());
  o.circularMb = kMinCircularMb;
  EXPECT_TRUE(makeSessionConfig(o, "n", makeSessionGuid(1, 0, 0), "a.etl", 8).ok());
  EXPECT_FALSE(makeSessionConfig(o, "", makeSessionGuid(1, 0, 0), "a.etl", 8).ok());
  EXPECT_FALSE(makeSessionConfig(o, "n", makeSessionGuid(1, 0, 0), "", 8).ok());
}

TEST(SessionConfigTest, SamplingRateAndOverridesReachTheConfig) {
  CaptureOptions o = systemOptions();
  o.samplingHz = 1000;
  o.stackReady = false;
  BufferOverrides ov;
  ov.maximumBuffers = 999;
  auto cfg = makeSessionConfig(o, "n", makeSessionGuid(1, 0, 0), "a.etl", 8, ov);
  ASSERT_TRUE(cfg.ok());
  EXPECT_EQ(cfg->profileIntervalUnits, 10000u);
  EXPECT_EQ(cfg->stackEvents.size(), 2u);
  EXPECT_EQ(cfg->buffers.maximumBuffers, 999u);
}

TEST(ValidateOptions, ModesAndRequiredFields) {
  CaptureOptions o = systemOptions();
  EXPECT_TRUE(validateCaptureOptions(o).ok());

  o.mode = CaptureMode::Attach;
  o.pid = 0;
  auto r = validateCaptureOptions(o);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  o.pid = 42;
  EXPECT_TRUE(validateCaptureOptions(o).ok());

  o.mode = CaptureMode::Launch;
  r = validateCaptureOptions(o);
  ASSERT_FALSE(r.ok()) << "launch needs an exe";
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  o.exe = "game.exe";
  EXPECT_TRUE(validateCaptureOptions(o).ok());

  o = systemOptions();
  o.outPath.clear();
  EXPECT_EQ(validateCaptureOptions(o).error().code, ErrorCode::InvalidArgument);
}

TEST(Paths, SegmentNamesAndFlushNames) {
  EXPECT_EQ(etlSegmentPath("dir/capture.tachyon", 0), std::filesystem::path("dir/capture.tachyon.etl"));
  EXPECT_EQ(etlSegmentPath("dir/capture.tachyon", 1), std::filesystem::path("dir/capture.tachyon.1.etl"));
  EXPECT_EQ(etlSegmentPath("c.tachyon", 12), std::filesystem::path("c.tachyon.12.etl"));
  EXPECT_TRUE(isValidFlushName("hitch-0042"));
  EXPECT_FALSE(isValidFlushName(""));
  EXPECT_FALSE(isValidFlushName("."));
  EXPECT_FALSE(isValidFlushName(".."));
  EXPECT_FALSE(isValidFlushName("a/b"));
  EXPECT_FALSE(isValidFlushName("a\\b"));
}

TEST(CaptureOptionsJsonTest, IsValidJsonWithTheEffectiveSettings) {
  CaptureOptions o = systemOptions();
  o.mode = CaptureMode::Attach;
  o.pid = 77;
  o.circularMb = 64;
  o.stackReady = false;
  auto cfg = makeSessionConfig(o, "Tachyon 5 2", makeSessionGuid(5, 2, 0), "a.etl", 8);
  ASSERT_TRUE(cfg.ok());
  const auto j = nlohmann::json::parse(captureOptionsJson(o, *cfg));
  EXPECT_EQ(j["backend"], "etw");
  EXPECT_EQ(j["mode"], "attach");
  EXPECT_EQ(j["pid"], 77);
  EXPECT_EQ(j["circular_mb"], 64);
  EXPECT_EQ(j["stackwalk"]["readythread"], false);
  EXPECT_EQ(j["session"], "Tachyon 5 2");
  EXPECT_NEAR(j["sampling_hz_effective"].get<double>(), 8190.0, 1.0);
  EXPECT_EQ(j["buffers"]["max"], 256);
}

}  // namespace
}  // namespace tachyon::capture::etw
