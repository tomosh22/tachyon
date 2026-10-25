#include "inst/clock_mapper.h"

#include <gtest/gtest.h>

namespace tachyon::inst {
namespace {

TEST(ClockMapper, IdentityIsTheDefaultWhenFrequenciesMatch) {
  ClockContext ctx;
  ctx.streamClockHz = 1'000'000'000;
  ctx.traceClockHz = 1'000'000'000;
  auto m = makeClockMapper(ctx);
  EXPECT_TRUE(m->isIdentity());
  EXPECT_EQ(m->toTraceTicks(123456789012345ull), 123456789012345ull);
  EXPECT_EQ(m->toTraceTicks(~uint64_t{0}), ~uint64_t{0});
}

TEST(ClockMapper, UnknownFrequenciesAreIdentity) {
  ClockContext ctx;
  ctx.streamClockHz = 0;
  ctx.traceClockHz = 10'000'000;
  EXPECT_TRUE(makeClockMapper(ctx)->isIdentity());
  ctx.streamClockHz = 5;
  ctx.traceClockHz = 0;
  EXPECT_TRUE(makeClockMapper(ctx)->isIdentity());
}

TEST(ClockMapper, ScaledByFrequencyRatioWhenNoCalibration) {
  ClockContext ctx;
  ctx.streamClockHz = 1'000'000'000;  // ns
  ctx.traceClockHz = 10'000'000;      // QPC
  std::string note;
  auto m = makeClockMapper(ctx, &note);
  EXPECT_FALSE(m->isIdentity());
  EXPECT_FALSE(note.empty());
  EXPECT_EQ(m->toTraceTicks(1'000'000'000), 10'000'000u);
  EXPECT_EQ(m->toTraceTicks(150), 1u);  // floor
  // 128-bit intermediate: no overflow for large timestamps.
  EXPECT_EQ(m->toTraceTicks(10'000'000'000'000'000'000ull), 100'000'000'000'000'000ull);
}

TEST(ClockMapper, ScaledSaturatesInsteadOfWrapping) {
  ScaledClockMapper up(1, 1'000'000);
  EXPECT_EQ(up.toTraceTicks(~uint64_t{0}), ~uint64_t{0});
}

TEST(ClockMapper, CalibratedInterpolatesAndExtrapolates) {
  std::vector<trace::ClockCalibrationRec> cals(3);
  // TSC ticks 1000 -> trace 100, 2000 -> 200, 4000 -> 400 (slope 0.1).
  const uint64_t other[3] = {1000, 2000, 4000};
  const uint64_t tr[3] = {100, 200, 400};
  for (int i = 0; i < 3; ++i) {
    cals[i].trace_ts = tr[i];
    cals[i].other_clock_ts = other[i];
    cals[i].other_clock_kind = trace::kClockTsc;
  }
  trace::ClockCalibrationRec gpu{};
  gpu.trace_ts = 5;
  gpu.other_clock_ts = 5;
  gpu.other_clock_kind = trace::kClockGpu;  // ignored
  cals.push_back(gpu);
  ClockContext ctx;
  ctx.streamClockHz = 2'100'000'000;
  ctx.traceClockHz = 1'000'000'000;
  ctx.calibrations = cals;
  std::string note;
  auto m = makeClockMapper(ctx, &note);
  EXPECT_TRUE(note.empty());
  EXPECT_EQ(m->toTraceTicks(1500), 150u);
  EXPECT_EQ(m->toTraceTicks(3000), 300u);
  EXPECT_EQ(m->toTraceTicks(1000), 100u);
  EXPECT_EQ(m->toTraceTicks(6000), 600u);  // extrapolated with the last segment
  EXPECT_EQ(m->toTraceTicks(0), 0u);       // extrapolated before the first point, clamped at 0
  EXPECT_EQ(m->toTraceTicks(500), 50u);
}

TEST(ClockMapper, CalibratedHandlesDecreasingTraceTime) {
  CalibratedClockMapper m({{0, 1000}, {1000, 0}});
  EXPECT_EQ(m.toTraceTicks(250), 750u);
  EXPECT_EQ(m.toTraceTicks(2000), 0u);
}

TEST(ClockMapper, OneCalibrationPointFallsBackToScaling) {
  std::vector<trace::ClockCalibrationRec> cals(2);
  cals[0].trace_ts = 100;
  cals[0].other_clock_ts = 1000;
  cals[0].other_clock_kind = trace::kClockTsc;
  cals[1] = cals[0];  // same point twice: still one distinct point
  ClockContext ctx;
  ctx.streamClockHz = 100;
  ctx.traceClockHz = 10;
  ctx.calibrations = cals;
  std::string note;
  auto m = makeClockMapper(ctx, &note);
  EXPECT_FALSE(note.empty());
  EXPECT_EQ(m->toTraceTicks(1000), 100u);
}

}  // namespace
}  // namespace tachyon::inst
