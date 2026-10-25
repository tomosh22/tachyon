// "Keep the last N seconds" conversion used by flush, driven from the WP-01 hex fixture.
#include "capture/windows_etw/time_window.h"

#include "core/platform.h"
#include "etlconvert/hex_events.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <cstring>

namespace tachyon::capture::etw {
namespace {

using namespace etlconvert;

TEST(WindowMath, StartIsMaxMinusWindowSaturating) {
  EXPECT_EQ(windowStartTimestamp(100, 30), 70u);
  EXPECT_EQ(windowStartTimestamp(100, 100), 0u);
  EXPECT_EQ(windowStartTimestamp(100, 200), 0u);
  EXPECT_EQ(windowStartTimestamp(100, 0), 0u);  // 0 = keep everything
  EXPECT_EQ(windowStartTimestamp(0, 5), 0u);
}

EtwEvent plainEvent(uint64_t ts) {
  EtwEvent e;
  e.provider = guids::kPerfInfo;
  e.opcode = opcodes::kPerfInfoSampledProfile;
  e.timestamp = ts;
  return e;
}

TEST(WindowMath, StackWalkIsJudgedByItsParentTimestamp) {
  std::vector<std::byte> payload(24);
  const uint64_t parent = 500;
  std::memcpy(payload.data(), &parent, sizeof(parent));
  EtwEvent stack;
  stack.provider = guids::kStackWalk;
  stack.opcode = opcodes::kStackWalkStack;
  stack.timestamp = 900;
  stack.payload = Bytes(payload.data(), payload.size());
  EXPECT_EQ(windowEventTime(stack), 500u);

  stack.opcode = opcodes::kStackWalkKeyKernel;  // other StackWalk opcodes keep their header time
  EXPECT_EQ(windowEventTime(stack), 900u);

  stack.opcode = opcodes::kStackWalkStack;
  stack.payload = Bytes(payload.data(), 4);  // too short to hold the parent timestamp
  EXPECT_EQ(windowEventTime(stack), 900u);

  EXPECT_EQ(windowEventTime(plainEvent(77)), 77u);
}

class CountingSink final : public EventSink {
 public:
  void onEvent(const EtwEvent& e) override { times.push_back(e.timestamp); }
  std::vector<uint64_t> times;
};

TEST(TimeWindowSinkTest, ForwardsOnlyEventsAtOrAfterTheCutoff) {
  CountingSink inner;
  TimeWindowSink sink(inner, 100);
  for (uint64_t ts : {50, 99, 100, 101, 5000}) sink.onEvent(plainEvent(ts));
  EXPECT_EQ(inner.times, (std::vector<uint64_t>{100, 101, 5000}));
  EXPECT_EQ(sink.forwarded(), 3u);
  EXPECT_EQ(sink.dropped(), 2u);
}

// ---- end to end over the hex fixture ----------------------------------------------------------------------
// synthetic_basic.hexevents: target process 100 with samples at ts 10000, 20100 and 30000 (plus an idle one
// at 20000), a context switch at 40000, a ready event at 41000, a SetName at 60000 (newest event).

struct Run {
  ConvertStats stats;
  Result<void> status = ok();
};

Result<ConvertStats> convertFixture(const std::string& outName, uint64_t windowTicks) {
  auto events = loadHexEventFile(std::string(TACHYON_FIXTURE_DIR) + "/etw/synthetic_basic.hexevents");
  EXPECT_TRUE(events.ok());
  static const DevicePathMapper mapper;  // Linux: paths are used as logged
  ConvertOptions o;
  o.targetPids = {100};
  o.threaded = false;
  o.deviceMapper = &mapper;
  EtlFileInfo info;
  info.clockFrequencyHz = 10'000'000;
  const auto out = std::filesystem::path(TACHYON_BINARY_DIR) / outName;
  ReplayFn replay = [&](EventSink& sink) -> Result<void> {
    for (const HexEvent& e : *events) sink.onEvent(e.view());
    return ok();
  };
  return convertWindowed(out, o, info, windowTicks, replay);
}

TEST(ConvertWindowed, ZeroWindowKeepsEverything) {
  auto st = convertFixture("window_all.tachyon", 0);
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 3u);
  EXPECT_EQ(st->cswitchesWritten, 1u);
  EXPECT_EQ(st->readiesWritten, 1u);
}

TEST(ConvertWindowed, KeepsOnlyTheLastTicksButAllMetadata) {
  // Newest event is at 60000; a 25000 tick window starts at 35000: only the switch and the ready event
  // remain.
  auto st = convertFixture("window_tail.tachyon", 25000);
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 0u);
  EXPECT_EQ(st->cswitchesWritten, 1u);
  EXPECT_EQ(st->readiesWritten, 1u);
  // Threads and the process were defined before the window; the metadata pass still saw them.
  EXPECT_EQ(st->processesWritten, 1u);
  EXPECT_GE(st->threadsWritten, 2u);
  EXPECT_GE(st->startTs, 35000u);

  auto reader = trace::TraceReader::open(std::filesystem::path(TACHYON_BINARY_DIR) / "window_tail.tachyon");
  ASSERT_TRUE(reader.ok());
  auto procs = reader->readProcesses();
  ASSERT_TRUE(procs.ok());
  EXPECT_EQ(procs->size(), 1u);
}

TEST(ConvertWindowed, MiddleWindowKeepsTheLaterSamplesAndTheirStacks) {
  // Cutoff 15000: the samples at 20100 and 30000 stay (the one at 10000 goes, the idle one is filtered
  // anyway).
  auto st = convertFixture("window_mid.tachyon", 45000);
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 2u);
  EXPECT_EQ(st->cswitchesWritten, 1u);
  EXPECT_EQ(st->parentsWithoutStack[0], 0u);  // the kept samples kept their stacks
}

TEST(ConvertWindowed, WindowLargerThanTheFileKeepsEverything) {
  auto st = convertFixture("window_big.tachyon", 10'000'000);
  ASSERT_TRUE(st.ok()) << st.error().toString();
  EXPECT_EQ(st->samplesWritten, 3u);
}

TEST(ConvertWindowed, ReplayFailurePropagates) {
  ConvertOptions o;
  o.threaded = false;
  EtlFileInfo info;
  ReplayFn fail = [](EventSink&) -> Result<void> { return makeError(ErrorCode::IoError, "boom"); };
  auto st =
      convertWindowed(std::filesystem::path(TACHYON_BINARY_DIR) / "window_fail.tachyon", o, info, 10, fail);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.error().code, ErrorCode::IoError);
}

#if !TACHYON_PLATFORM_WINDOWS
TEST(ConvertEtlLastSeconds, UnsupportedOffWindows) {
  auto st = convertEtlLastSeconds("x.etl", "y.tachyon", {}, 5);
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.error().code, ErrorCode::Unsupported);
}
#endif

}  // namespace
}  // namespace tachyon::capture::etw
