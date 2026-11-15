// WP-67f: the pure logic of the per-instance duration chart of Function Statistics: percentile, the clipped /
// log vertical scale, the threshold text, and the persisted preferences.
#include "viewer/paths.h"
#include "viewer/settings.h"
#include "viewer/stats/stats_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace tachyon::viewer;
using namespace tachyon::viewer::stats;
namespace fs = std::filesystem;

TEST(StatsScale, PercentileOfSmallAndDegenerateLists) {
  EXPECT_EQ(percentileTicks({}, 0.99), 0u);
  EXPECT_EQ(percentileTicks({7}, 0.99), 7u);           // n == 1
  EXPECT_EQ(percentileTicks({5, 5, 5, 5}, 0.99), 5u);  // all equal
  // n < 100: the 99th percentile is the maximum.
  std::vector<uint64_t> d;
  for (uint64_t i = 1; i <= 99; ++i) d.push_back(i * 10);
  std::reverse(d.begin(), d.end());  // unsorted input
  EXPECT_EQ(percentileTicks(d, 0.99), 990u);
  EXPECT_EQ(percentileTicks(d, 0.5), 500u);  // nearest rank: the ceil(0.5 * 99) = 50th value
  EXPECT_EQ(percentileTicks(d, 0.0), 10u);
  EXPECT_EQ(percentileTicks(d, 1.0), 990u);
}

TEST(StatsScale, ClipsOnlyWhenTheP99IsBelowTheMaximum) {
  std::vector<uint64_t> d(200, 100);
  d[17] = 100000;  // one outlier of 200: p99 = the 198th value = 100
  const InstanceScale clipped = makeInstanceScale(d, true, false);
  EXPECT_TRUE(clipped.clipping);
  EXPECT_EQ(clipped.top, 100u);
  EXPECT_EQ(clipped.maxTicks, 100000u);
  EXPECT_TRUE(barClipped(clipped, 100000));
  EXPECT_FALSE(barClipped(clipped, 100));
  EXPECT_DOUBLE_EQ(barFraction(clipped, 100000), 1.0);
  EXPECT_DOUBLE_EQ(barFraction(clipped, 100), 1.0);
  EXPECT_DOUBLE_EQ(barFraction(clipped, 50), 0.5);

  const InstanceScale off = makeInstanceScale(d, false, false);
  EXPECT_FALSE(off.clipping);
  EXPECT_EQ(off.top, 100000u);
  EXPECT_FALSE(barClipped(off, 100000));
  EXPECT_DOUBLE_EQ(barFraction(off, 100), 0.001);

  // Nothing to clip: p99 == max (all equal, n == 1, small n).
  EXPECT_FALSE(makeInstanceScale(std::vector<uint64_t>(300, 42), true, false).clipping);
  EXPECT_FALSE(makeInstanceScale({9}, true, false).clipping);
  EXPECT_EQ(makeInstanceScale({9}, true, false).top, 9u);
  EXPECT_FALSE(makeInstanceScale({1, 2, 1000}, true, false).clipping);
  // Empty and all-zero lists keep a usable scale.
  EXPECT_EQ(makeInstanceScale({}, true, false).top, 1u);
  EXPECT_EQ(makeInstanceScale({0, 0}, true, true).top, 1u);
}

TEST(StatsScale, LogMappingIsMonotonicAndFiniteForZero) {
  const InstanceScale s = makeInstanceScale({0, 1, 10, 1000, 1000000}, false, true);
  double prev = -1.0;
  for (const uint64_t t : {0ull, 1ull, 2ull, 10ull, 100ull, 1000ull, 123456ull, 1000000ull}) {
    const double f = barFraction(s, t);
    EXPECT_TRUE(std::isfinite(f)) << t;
    EXPECT_GE(f, 0.0);
    EXPECT_LE(f, 1.0);
    EXPECT_GT(f, prev) << t;
    prev = f;
  }
  EXPECT_DOUBLE_EQ(barFraction(s, 0), 0.0);
  EXPECT_DOUBLE_EQ(barFraction(s, 1000000), 1.0);
  // A 1000x shorter instance is still clearly visible on the log scale.
  EXPECT_GT(barFraction(s, 1000), 0.4);
  // Log scale combined with clipping tops out at the p99.
  std::vector<uint64_t> d(200, 1000);
  d[3] = 50000000;
  const InstanceScale both = makeInstanceScale(d, true, true);
  EXPECT_TRUE(both.clipping);
  EXPECT_TRUE(both.log);
  EXPECT_DOUBLE_EQ(barFraction(both, 1000), 1.0);
  EXPECT_DOUBLE_EQ(barFraction(both, 50000000), 1.0);
  EXPECT_LT(barFraction(both, 10), 1.0);
}

TEST(StatsScale, TicksAtFractionInvertsTheBarHeight) {
  for (const bool log : {false, true}) {
    const InstanceScale s = makeInstanceScale({0, 500, 1000000}, false, log);
    EXPECT_EQ(ticksAtFraction(s, 0.0), 0u);
    EXPECT_EQ(ticksAtFraction(s, 1.0), 1000000u);
    EXPECT_EQ(ticksAtFraction(s, 7.0), 1000000u);  // clamped
    for (const uint64_t t : {1ull, 40ull, 5000ull, 999999ull}) {
      const uint64_t back = ticksAtFraction(s, barFraction(s, t));
      EXPECT_NEAR(static_cast<double>(back), static_cast<double>(t),
                  std::max(1.0, static_cast<double>(t) * 1e-6))
          << t << " " << log;
    }
  }
}

TEST(StatsScale, ParsesThresholdText) {
  EXPECT_EQ(parseDurationUs("143 ms"), std::optional<double>(143000.0));
  EXPECT_EQ(parseDurationUs("1.5ms"), std::optional<double>(1500.0));
  EXPECT_EQ(parseDurationUs("  300us "), std::optional<double>(300.0));
  EXPECT_EQ(parseDurationUs("2 S"), std::optional<double>(2000000.0));
  EXPECT_EQ(parseDurationUs("800ns"), std::optional<double>(0.8));
  EXPECT_EQ(parseDurationUs("12 \xC2\xB5s"), std::optional<double>(12.0));
  EXPECT_EQ(parseDurationUs("5"), std::optional<double>(5000.0));  // a bare number is milliseconds
  EXPECT_EQ(parseDurationUs(""), std::optional<double>(0.0));
  EXPECT_EQ(parseDurationUs("0"), std::optional<double>(0.0));
  EXPECT_EQ(parseDurationUs("abc"), std::nullopt);
  EXPECT_EQ(parseDurationUs("-3 ms"), std::nullopt);
  EXPECT_EQ(parseDurationUs("3 parsecs"), std::nullopt);
}

TEST(StatsScale, PreferencesRoundTripAndDefaults) {
  EXPECT_TRUE(Settings{}.statsClipOutliers);
  EXPECT_FALSE(Settings{}.statsLogScale);
  EXPECT_EQ(Settings{}.statsThresholdUs, 0.0);
  const fs::path dir = fs::path(TACHYON_BINARY_DIR) / "viewer_stats_chart_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  Settings s;
  s.statsClipOutliers = false;
  s.statsLogScale = true;
  s.statsThresholdUs = 143000.0;
  const fs::path file = dir / "settings.json";
  ASSERT_TRUE(s.save(file).ok());
  const Settings back = Settings::load(file);
  EXPECT_FALSE(back.statsClipOutliers);
  EXPECT_TRUE(back.statsLogScale);
  EXPECT_EQ(back.statsThresholdUs, 143000.0);
  EXPECT_EQ(back, s);
  // Invalid values fall back to the defaults.
  std::ofstream(file, std::ios::binary)
      << R"({"statsClipOutliers": 3, "statsLogScale": "x", "statsThresholdUs": -5})";
  const Settings bad = Settings::load(file);
  EXPECT_TRUE(bad.statsClipOutliers);
  EXPECT_FALSE(bad.statsLogScale);
  EXPECT_EQ(bad.statsThresholdUs, 0.0);
  fs::remove_all(dir);
}
