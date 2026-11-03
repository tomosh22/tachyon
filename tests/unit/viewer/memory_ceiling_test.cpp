// WP-64: the memory ceiling. The governor's stages on fake sources, the setting, and the real thing in the
// app: a ceiling below what the trace needs makes the viewer free timeline detail levels and stop caching
// analysis results, tells the user, and keeps everything working.
#include "viewer/app.h"
#include "viewer/memory_governor.h"
#include "viewer/settings.h"
#include "viewer/timeline/timeline_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

fs::path fixture(const char* name) {
  return fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name;
}

struct Fake {
  uint64_t model = 100, lod = 50, cache = 30;
  uint64_t lodFloor = 10;  // the coarsest levels that cannot be freed
  int lodTrims = 0, cacheLimits = 0;
  uint64_t lastCacheBudget = ~0ull;
  MemorySources sources() {
    MemorySources s;
    s.modelBytes = [this] { return model; };
    s.lodBytes = [this] { return lod; };
    s.cacheBytes = [this] { return cache; };
    s.trimLod = [this](uint64_t bytes) {
      ++lodTrims;
      const uint64_t freed = std::min(std::max<uint64_t>(bytes, 1), lod - lodFloor);
      lod -= freed;
      return freed;
    };
    s.limitCaches = [this](uint64_t budget) {
      ++cacheLimits;
      lastCacheBudget = budget;
      cache = std::min(cache, budget);
    };
    return s;
  }
};

}  // namespace

TEST(MemoryGovernor, DoesNothingBelowTheCeilingOrWithoutOne) {
  Fake f;
  MemoryGovernor g;
  EXPECT_FALSE(g.update(f.sources(), 1000));
  EXPECT_FALSE(g.update(f.sources(), 0));
  EXPECT_EQ(g.status().stage, MemoryStage::Normal);
  EXPECT_EQ(g.status().usedBytes, 180u);
  EXPECT_EQ(f.lodTrims, 0);
  EXPECT_EQ(f.cacheLimits, 0);
}

TEST(MemoryGovernor, FreesTheTimelineLevelsFirst) {
  Fake f;  // 180 used
  MemoryGovernor g;
  EXPECT_TRUE(g.update(f.sources(), 150));
  EXPECT_EQ(g.status().stage, MemoryStage::LodReduced);
  EXPECT_EQ(f.lod, 20u);  // freed exactly the 30 over the ceiling
  EXPECT_EQ(f.cacheLimits, 0) << "the caches are only touched when the levels were not enough";
  EXPECT_EQ(g.status().usedBytes, 150u);
  EXPECT_EQ(g.status().lodFreedBytes, 30u);
  EXPECT_NE(g.status().message.find("timeline detail"), std::string::npos);
  EXPECT_FALSE(g.update(f.sources(), 150)) << "no new stage while within the ceiling";
}

TEST(MemoryGovernor, ThenLimitsTheCachesThenOnlyReports) {
  Fake f;
  MemoryGovernor g;
  // Levels alone (40 freeable) are not enough for 120: caches are limited too.
  EXPECT_TRUE(g.update(f.sources(), 120));
  EXPECT_EQ(g.status().stage, MemoryStage::CachesLimited);
  EXPECT_EQ(f.cacheLimits, 1);
  EXPECT_LE(g.status().usedBytes, 120u + 10u);
  EXPECT_NE(g.status().message.find("no longer cached"), std::string::npos);

  // The model alone does not fit: nothing more to free, the stage says so once.
  Fake big;
  big.model = 500;
  MemoryGovernor g2;
  EXPECT_TRUE(g2.update(big.sources(), 200));
  EXPECT_EQ(g2.status().stage, MemoryStage::ModelOverCeiling);
  EXPECT_NE(g2.status().message.find("trace needs"), std::string::npos);
  EXPECT_FALSE(g2.update(big.sources(), 200));
}

TEST(MemoryGovernor, StagesAdvanceWhenTheCachesGrowBackAndResetStartsOver) {
  Fake f;
  MemoryGovernor g;
  EXPECT_TRUE(g.update(f.sources(), 150));
  EXPECT_EQ(g.status().stage, MemoryStage::LodReduced);
  f.cache += 60;  // the user ran more queries
  EXPECT_TRUE(g.update(f.sources(), 150));
  EXPECT_EQ(g.status().stage, MemoryStage::CachesLimited);
  g.reset();
  EXPECT_EQ(g.status().stage, MemoryStage::Normal);
  EXPECT_TRUE(g.status().message.empty());
}

TEST(MemoryGovernor, CeilingFromPercentOfPhysicalMemory) {
  EXPECT_EQ(memoryCeilingBytes(75, 16ull << 30), 12ull << 30);
  EXPECT_EQ(memoryCeilingBytes(0, 16ull << 30), 0u);
  EXPECT_EQ(memoryCeilingBytes(75, 0), 0u);
  EXPECT_EQ(memoryCeilingBytes(500, 100), 100u);
  EXPECT_GT(physicalMemoryBytes(), 0u);
  EXPECT_EQ(formatBytes(3ull << 29), "1.5 GiB");
  EXPECT_EQ(formatBytes(320ull << 20), "320 MiB");
}

TEST(MemorySettings, DefaultRoundTripAndClamp) {
  const fs::path dir = fs::path(TACHYON_BINARY_DIR) / "viewer_memory_settings_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  Settings s;
  EXPECT_EQ(s.memoryCeilingPercent, 75u);
  s.memoryCeilingPercent = 60;
  ASSERT_TRUE(s.save(dir / "a.json").ok());
  EXPECT_EQ(Settings::load(dir / "a.json").memoryCeilingPercent, 60u);
  std::ofstream(dir / "b.json") << R"({"memoryCeilingPercent": 3})";
  EXPECT_EQ(Settings::load(dir / "b.json").memoryCeilingPercent, 10u) << "clamped to the minimum";
  std::ofstream(dir / "c.json") << R"({"memoryCeilingPercent": 0})";
  EXPECT_EQ(Settings::load(dir / "c.json").memoryCeilingPercent, 0u) << "0 = no ceiling";
  std::ofstream(dir / "d.json") << R"({"memoryCeilingPercent": "lots"})";
  EXPECT_EQ(Settings::load(dir / "d.json").memoryCeilingPercent, 75u);
  std::ofstream(dir / "e.json") << R"({"memoryCeilingPercent": 400})";
  EXPECT_EQ(Settings::load(dir / "e.json").memoryCeilingPercent, 100u);
  fs::remove_all(dir);
}

namespace {

class AppHarness {
 public:
  explicit AppHarness(uint64_t ceiling) : app(makeNullRenderer(), options(ceiling)) {
    std::string error;
    EXPECT_TRUE(app.init(error)) << error;
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    auto* panel =
        dynamic_cast<ThreadsTimelinePanel*>(app.panels().find(ThreadsTimelinePanel::kName)->panel.get());
    timeline = &panel->widget();
    for (PanelEntry& e : app.panels().entries()) e.open = e.panel.get() == panel;
  }
  static AppOptions options(uint64_t ceiling) {
    AppOptions o;
    o.persist = false;
    o.pacing = false;
    o.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_memory_test_no_such_settings.json";
    o.streamingLoad = false;
    o.memoryCeilingBytes = ceiling;
    o.memoryCheckMs = 0;  // every frame
    return o;
  }
  bool until(const std::function<bool()>& pred, int seconds = 60) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  void open(const char* name) {
    app.openFile(fixture(name));
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    ASSERT_TRUE(until([&] { return timeline->lodFinished(); }));
    for (int i = 0; i < 3; ++i) app.frame(0);
  }

  App app;
  timeline::TimelineWidget* timeline = nullptr;
};

}  // namespace

TEST(MemoryCeilingApp, NothingIsDegradedWithRoom) {
  AppHarness h(uint64_t{64} << 30);
  h.open("medium.tachyon");
  EXPECT_EQ(h.app.memoryStatus().stage, MemoryStage::Normal);
  EXPECT_EQ(h.timeline->lodTrim().droppedLevels, 0u);
  EXPECT_FALSE(h.timeline->lodTrim().fineDropped);
  EXPECT_EQ(h.timeline->lodDegradedBaseLevels(), 0u);
  EXPECT_FALSE(h.app.warningsBanner().visible());
  EXPECT_GT(h.timeline->lodBytes(), 0u);
}

TEST(MemoryCeilingApp, ARuntimeCeilingBelowTheNeedFreesLodLevelsAndTellsTheUser) {
  AppHarness h(
      0);  // no ceiling at first (and the settings default is not used: override 0 = settings percent)
  h.app.setMemoryCeilingPercent(0);
  h.open("medium.tachyon");
  ASSERT_EQ(h.app.memoryStatus().stage, MemoryStage::Normal);
  const size_t lodBefore = h.timeline->lodBytes();
  const uint64_t model = h.app.document()->model().residentBytes();
  ASSERT_GT(lodBefore, 0u);

  // A ceiling that fits the model and a third of the timeline index.
  const uint64_t toasts = h.app.toasts().toasts().size();
  h.app.setMemoryCeilingOverride(model + lodBefore / 3);
  const MemoryStatus& st = h.app.memoryStatus();
  EXPECT_EQ(st.stage, MemoryStage::LodReduced);
  EXPECT_GT(st.lodFreedBytes, 0u);
  EXPECT_LT(h.timeline->lodBytes(), lodBefore);
  EXPECT_LE(h.timeline->lodBytes() + model, model + lodBefore / 3 + lodBefore / 8)
      << "within the ceiling (+slack of a level)";
  EXPECT_TRUE(h.timeline->lodTrim().fineDropped || h.timeline->lodTrim().droppedLevels > 0);
  // The user is told: a toast and the banner.
  EXPECT_GT(h.app.toasts().toasts().size(), toasts);
  EXPECT_NE(h.app.toasts().toasts().back().text.find("Memory ceiling"), std::string::npos);
  ASSERT_TRUE(h.app.warningsBanner().visible());
  bool bannerHasIt = false;
  for (const CaptureWarning& w : h.app.warningsBanner().warnings())
    bannerHasIt = bannerHasIt || w.kind == CaptureWarningKind::MemoryDegraded;
  EXPECT_TRUE(bannerHasIt);

  // The timeline keeps drawing every row; nothing is pending or broken.
  for (int i = 0; i < 5; ++i) h.app.frame(0);
  EXPECT_EQ(h.timeline->frameStats().rowsPending, 0u);
  EXPECT_GT(h.timeline->frameStats().rowsDrawn, 0u);

  // A ceiling below the model: caches are limited and then it is only reported.
  h.app.setMemoryCeilingOverride(model / 2);
  EXPECT_EQ(h.app.memoryStatus().stage, MemoryStage::ModelOverCeiling);
  EXPECT_LT(h.timeline->lodBytes(), lodBefore / 20) << "only the coarsest levels are left";
  for (int i = 0; i < 5; ++i) h.app.frame(0);
  EXPECT_GT(h.timeline->frameStats().rowsDrawn, 0u) << "and the timeline still works";
}

TEST(MemoryCeilingApp, ACeilingKnownAtOpenBuildsTheIndexCoarse) {
  AppHarness h(uint64_t{40} << 20);  // the model of `medium` is ~120 MiB: the index gets the 32 MiB floor
  h.open("medium.tachyon");
  EXPECT_GT(h.timeline->lodDegradedBaseLevels(), 0u);
  EXPECT_LE(h.timeline->lodBytes(), uint64_t{40} << 20);
  for (int i = 0; i < 5; ++i) h.app.frame(0);
  EXPECT_EQ(h.timeline->frameStats().rowsPending, 0u);
  EXPECT_GT(h.timeline->frameStats().rowsDrawn, 0u);
}

TEST(MemoryCeilingApp, TheMenuSettingIsSavedAndAppliedAtOnce) {
  AppHarness h(0);
  h.app.setMemoryCeilingPercent(0);
  h.open("medium.tachyon");
  EXPECT_EQ(h.app.memoryCeiling(), 0u);
  h.app.setMemoryCeilingPercent(5);  // below the minimum: clamped
  EXPECT_EQ(h.app.settings().memoryCeilingPercent, Settings::kMinMemoryCeilingPercent);
  EXPECT_EQ(h.app.memoryCeiling(),
            memoryCeilingBytes(Settings::kMinMemoryCeilingPercent, physicalMemoryBytes()));
}
