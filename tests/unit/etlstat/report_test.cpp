#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "etw_hex.h"
#include "report.h"
#include "test_util.h"

using namespace tachyon::etlstat;
using namespace tachyon::etlstat::testing;

namespace {

StatsAccumulator loadFixture() {
  StatsAccumulator acc;
  auto events = loadHexEventFile(std::string(TACHYON_FIXTURE_DIR) + "/etw/synthetic_basic.hexevents");
  EXPECT_TRUE(events.ok());
  if (events.ok()) {
    for (const HexEvent& e : *events) acc.add(e.view());
  }
  acc.finish();
  return acc;
}

// Builds a trace of `seconds` seconds at `hz` per CPU on `cpus` CPUs; every CPU runs a non-idle thread.
StatsSnapshot synthesize(uint32_t cpus, uint32_t hz, uint32_t seconds, double stackFraction) {
  StatsAccumulator acc;
  const uint64_t freq = 10'000'000;
  const uint64_t step = freq / hz;
  uint64_t n = 0;
  for (uint64_t t = 0; t < static_cast<uint64_t>(seconds) * freq; t += step) {
    for (uint16_t cpu = 0; cpu < cpus; ++cpu) {
      const uint32_t tid = 1000 + cpu;
      const ByteVec sp = sampledProfilePayload(0x1000, tid);
      acc.add(makeEvent(guids::kPerfInfo, 46, 2, t + cpu, tid, cpu, sp));
      ++n;
      if (static_cast<double>(n % 100) < stackFraction * 100.0) {
        const ByteVec st = stackWalkPayload(t + cpu, 100, tid, {1, 2, 3});
        acc.add(makeEvent(guids::kStackWalk, 32, 2, t + cpu + 1, tid, cpu, st));
      }
    }
  }
  acc.finish();
  return acc.snapshot();
}

}  // namespace

TEST(Report, FixtureNumbers) {
  const StatsAccumulator acc = loadFixture();
  TraceFileInfo info;
  ReportOptions opts;
  const Report r = buildReport(acc.snapshot(), info, opts);
  EXPECT_DOUBLE_EQ(r.matchedFraction, 1.0);
  EXPECT_DOUBLE_EQ(r.matchedFractionNonIdle, 1.0);
  EXPECT_NEAR(r.sampleSeconds, (30000.0 - 10000.0) / 10'000'000.0, 1e-12);
  ASSERT_EQ(r.cpus.size(), 2u);
  EXPECT_TRUE(r.cpus[0].busy);
  EXPECT_FALSE(r.cpus[1].busy);
  EXPECT_EQ(r.busyCpuCount, 1u);
  EXPECT_TRUE(r.matchAcceptance);
}

TEST(Report, ProcessFilterByImageAndPid) {
  const StatsAccumulator acc = loadFixture();
  ReportOptions byImage;
  byImage.image = "C:\\Games\\GAME.EXE";
  const Report a = buildReport(acc.snapshot(), TraceFileInfo{}, byImage);
  EXPECT_TRUE(a.hasProcessFilter);
  ASSERT_EQ(a.filterPids.size(), 1u);
  EXPECT_EQ(a.filterPids[0], 100u);
  EXPECT_EQ(a.processSamples, 3u);
  ASSERT_FALSE(a.topThreads.empty());
  EXPECT_EQ(a.topThreads[0].first, 101u);  // two samples on thread 101

  ReportOptions byPid;
  byPid.pid = 999;
  const Report b = buildReport(acc.snapshot(), TraceFileInfo{}, byPid);
  EXPECT_EQ(b.processSamples, 0u);

  const Report none = buildReport(acc.snapshot(), TraceFileInfo{}, ReportOptions{});
  EXPECT_FALSE(none.hasProcessFilter);
}

TEST(Report, RatePerBusyCoreAcceptanceLogic) {
  ReportOptions opts;
  const StatsSnapshot good = synthesize(4, 8190, 2, 1.0);
  const Report rg = buildReport(good, TraceFileInfo{}, opts);
  EXPECT_EQ(rg.busyCpuCount, 4u);
  EXPECT_NEAR(rg.minBusyRate, 8190.0, 100.0);
  EXPECT_TRUE(rg.rateAcceptance);
  EXPECT_TRUE(rg.matchAcceptance);

  const StatsSnapshot slow = synthesize(2, 4000, 2, 1.0);
  const Report rs = buildReport(slow, TraceFileInfo{}, opts);
  EXPECT_FALSE(rs.rateAcceptance);
}

TEST(Report, MatchedFractionThresholdIsStrict) {
  ReportOptions opts;
  const StatsSnapshot lowStacks = synthesize(1, 5000, 2, 0.90);
  const Report r = buildReport(lowStacks, TraceFileInfo{}, opts);
  EXPECT_LT(r.matchedFraction, 0.95);
  EXPECT_FALSE(r.matchAcceptance);
}

TEST(Report, TextContainsAcceptanceNumbers) {
  const StatsAccumulator acc = loadFixture();
  TraceFileInfo info;
  info.valid = true;
  info.eventsLost = 12;
  info.buffersLost = 3;
  info.numberOfProcessors = 2;
  ReportOptions opts;
  opts.image = "game.exe";
  const Report r = buildReport(acc.snapshot(), info, opts);
  const std::string text = formatReportText(acc.snapshot(), info, r, opts);
  EXPECT_NE(text.find("EventsLost=12 BuffersLost=3"), std::string::npos);
  EXPECT_NE(text.find("SampledProfile"), std::string::npos);
  EXPECT_NE(text.find("max stack depth: 5"), std::string::npos);
  EXPECT_NE(text.find("PRESENT"), std::string::npos);
  EXPECT_NE(text.find("RenderThread"), std::string::npos);
  EXPECT_NE(text.find("unmatched: 1"), std::string::npos);
  EXPECT_NE(text.find("process filter"), std::string::npos);
}

TEST(Report, JsonIsValidAndComplete) {
  const StatsAccumulator acc = loadFixture();
  TraceFileInfo info;
  info.valid = true;
  info.eventsLost = 7;
  ReportOptions opts;
  opts.pid = 100;
  const Report r = buildReport(acc.snapshot(), info, opts);
  const auto j = nlohmann::json::parse(formatReportJson(acc.snapshot(), info, r, opts));
  EXPECT_EQ(j["logfile"]["eventsLost"], 7);
  EXPECT_EQ(j["stacks"]["unmatched"], 1);
  EXPECT_EQ(j["stacks"]["maxDepth"], 5);
  EXPECT_TRUE(j["threadNames"]["present"].get<bool>());
  EXPECT_EQ(j["process"]["samples"], 3);
  EXPECT_EQ(j["sampling"]["sampledProfile"], 4);
  bool sawSampled = false;
  for (const auto& k : j["events"]) {
    if (k["providerName"] == "PerfInfo" && k["opcode"] == 46) {
      sawSampled = true;
      EXPECT_EQ(k["count"], 4);
      EXPECT_EQ(k["opcodeName"], "SampledProfile");
    }
  }
  EXPECT_TRUE(sawSampled);
}

TEST(Report, EmptyTraceDoesNotDivideByZero) {
  StatsAccumulator acc;
  acc.finish();
  ReportOptions opts;
  const Report r = buildReport(acc.snapshot(), TraceFileInfo{}, opts);
  EXPECT_EQ(r.busyCpuCount, 0u);
  EXPECT_FALSE(r.rateAcceptance);
  EXPECT_FALSE(formatReportText(acc.snapshot(), TraceFileInfo{}, r, opts).empty());
  EXPECT_FALSE(
      nlohmann::json::parse(formatReportJson(acc.snapshot(), TraceFileInfo{}, r, opts)).is_discarded());
}
