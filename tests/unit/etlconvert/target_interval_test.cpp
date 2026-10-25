// ConvertOptions::targetIntervals (WP-13b): a pid is a target only inside its accepted intervals, so the
// samples of an unrelated process that reuses the pid are dropped.
#include "test_util.h"

#include <gtest/gtest.h>

namespace tachyon::etlconvert {
namespace {

using namespace testing;

// pid 100 runs thread 101 until t=2000; the pid is reused by an unrelated process with thread 301 at t=4000.
EventScript reuseScenario() {
  EventScript s;
  s.process(opcodes::kProcessStart, 100, 100, 4, "game.exe");
  s.thread(opcodes::kThreadStart, 110, 100, 101);
  s.sample(1000, 101, 100, 0, 0x1000);
  s.sample(1500, 101, 100, 0, 0x1000);
  s.thread(opcodes::kThreadEnd, 1900, 100, 101);
  s.process(opcodes::kProcessEnd, 2000, 100, 4, "game.exe");
  s.process(opcodes::kProcessStart, 3500, 100, 9, "unrelated.exe");
  s.thread(opcodes::kThreadStart, 3600, 100, 301);
  s.sample(4000, 301, 100, 0, 0x2000);
  s.sample(4500, 301, 100, 0, 0x2000);
  s.sample(4600, 301, 100, 0, 0x2000);
  return s;
}

ConvertOptions baseOptions() {
  ConvertOptions o;
  o.threaded = false;
  o.stackWindowMs = 1;
  return o;
}

Result<ConvertStats> run(const ConvertOptions& o, const char* name) {
  return reuseScenario().run(std::filesystem::temp_directory_path() / name, o);
}

TEST(TargetIntervals, PlainTargetPidKeepsTheReusedPidsSamples) {
  ConvertOptions o = baseOptions();
  o.targetPids = {100};
  auto st = run(o, "tachyon_interval_plain.tachyon");
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 5u) << "the unscoped pid list cannot tell the two processes apart";
}

TEST(TargetIntervals, IntervalDropsSamplesOfTheUnrelatedProcessThatReusesThePid) {
  ConvertOptions o = baseOptions();
  o.targetIntervals = {{100, 0, 2000}};
  auto st = run(o, "tachyon_interval_scoped.tachyon");
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 2u);
  EXPECT_EQ(st->samplesDropped, 3u);
}

TEST(TargetIntervals, SeveralIntervalsOfOnePidAndOpenEnds) {
  ConvertOptions o = baseOptions();
  o.targetIntervals = {{100, 0, 1200}, {100, 4400, ~uint64_t{0}}};
  auto st = run(o, "tachyon_interval_multi.tachyon");
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 3u);  // t=1000, 4500, 4600
}

TEST(TargetIntervals, IntervalsOnlyBehavesAsProcessModeNotSystemMode) {
  ConvertOptions o = baseOptions();
  o.targetIntervals = {{999, 0, 10}};  // nothing in the scenario
  auto st = run(o, "tachyon_interval_none.tachyon");
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 0u) << "a non-empty interval list is a target filter, not 'keep everything'";
}

}  // namespace
}  // namespace tachyon::etlconvert
