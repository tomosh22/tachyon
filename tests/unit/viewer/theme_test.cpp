// WP-67e: themes. System / Light / Dark resolution, the OS probes (parsers, injected probe, live switch),
// settings migration, palette contrast (WCAG) and the thread state colours of both palettes.
#include "viewer/theme.h"
#include "viewer/app.h"
#include "viewer/os_theme.h"
#include "viewer/settings.h"
#include "viewer/timeline/timeline_colors.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

AppOptions headless() {
  AppOptions o;
  o.persist = false;
  o.pacing = false;
  o.autoConnectAgent = false;
  o.configPath = fs::path(TACHYON_BINARY_DIR) / "theme_test_no_such_settings.json";
  return o;
}

// Runs frames until `done()` or a deadline; true when it became true.
template <typename Done>
bool pumpUntil(App& app, Done done, int ms = 5000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

struct TempDir {
  explicit TempDir(const char* name) : path(fs::path(TACHYON_BINARY_DIR) / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path path;
};

void writeFile(const fs::path& p, const std::string& text) {
  std::ofstream(p, std::ios::binary) << text;
}

}  // namespace

// ---- resolution -------------------------------------------------------------------------------------------

TEST(Theme, SystemResolvesToTheOsAnswerAndExplicitChoicesIgnoreIt) {
  for (const ColorScheme os : {ColorScheme::Light, ColorScheme::Dark}) {
    EXPECT_EQ(resolveTheme(ThemeKind::System, os), os);
    EXPECT_EQ(resolveTheme(ThemeKind::Light, os), ColorScheme::Light);
    EXPECT_EQ(resolveTheme(ThemeKind::Dark, os), ColorScheme::Dark);
  }
  EXPECT_EQ(Settings{}.theme, ThemeKind::System);
}

// ---- OS probes: the pure parts ----------------------------------------------------------------------------

TEST(OsTheme, PortalAnswerParsing) {
  EXPECT_EQ(parsePortalColorScheme("(<<uint32 1>>,)\n"), ColorScheme::Dark);
  EXPECT_EQ(parsePortalColorScheme("(<<uint32 2>>,)"), ColorScheme::Light);
  EXPECT_EQ(parsePortalColorScheme("(<uint32 1>,)"), ColorScheme::Dark);
  EXPECT_EQ(parsePortalColorScheme("(<<uint32 0>>,)"), std::nullopt);  // no preference: ask the next source
  EXPECT_EQ(parsePortalColorScheme("(<<uint32 7>>,)"), std::nullopt);
  EXPECT_EQ(parsePortalColorScheme("1"), ColorScheme::Dark);
  EXPECT_EQ(parsePortalColorScheme(""), std::nullopt);
  EXPECT_EQ(parsePortalColorScheme("Error: GDBus.Error:org.freedesktop.DBus.Error.ServiceUnknown"),
            std::nullopt);
}

TEST(OsTheme, GsettingsAndGtkThemeParsing) {
  EXPECT_EQ(parseGsettingsColorScheme("'prefer-dark'\n"), ColorScheme::Dark);
  EXPECT_EQ(parseGsettingsColorScheme("'prefer-light'"), ColorScheme::Light);
  EXPECT_EQ(parseGsettingsColorScheme("'default'"), std::nullopt);
  EXPECT_EQ(parseGsettingsColorScheme(""), std::nullopt);
  EXPECT_EQ(parseGtkTheme("Adwaita:dark"), ColorScheme::Dark);
  EXPECT_EQ(parseGtkTheme("Breeze-Dark"), ColorScheme::Dark);
  EXPECT_EQ(parseGtkTheme("Adwaita"), ColorScheme::Light);
  EXPECT_EQ(parseGtkTheme(""), std::nullopt);
  EXPECT_EQ(parseGtkTheme("  "), std::nullopt);
}

TEST(OsTheme, WindowsAppsUseLightThemeMapping) {
  EXPECT_EQ(schemeFromAppsUseLightTheme(0u), ColorScheme::Dark);
  EXPECT_EQ(schemeFromAppsUseLightTheme(1u), ColorScheme::Light);
  EXPECT_EQ(schemeFromAppsUseLightTheme(std::nullopt), ColorScheme::Light);  // missing = light
}

TEST(OsTheme, RealProbeNeverFailsAndAgreesWithTheRegistryOnWindows) {
  // On Linux this runs gdbus / gsettings with a short timeout (absent or failing tools fall through to dark);
  // on Windows it reads HKCU\...\Personalize\AppsUseLightTheme.
  const ColorScheme scheme = queryOsColorScheme();
  EXPECT_TRUE(scheme == ColorScheme::Light || scheme == ColorScheme::Dark);
#ifdef _WIN32
  EXPECT_EQ(scheme, schemeFromAppsUseLightTheme(readAppsUseLightTheme()));
#else
  EXPECT_EQ(readAppsUseLightTheme(), std::nullopt);
#endif
}

TEST(OsTheme, WatcherPublishesProbeChangesWithoutBlockingTheCaller) {
  std::atomic<int> answer{static_cast<int>(ColorScheme::Light)};
  std::atomic<int> changes{0};
  std::atomic<bool> release{false};
  OsThemeWatcher watcher(
      [&] {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));  // a slow probe
        return static_cast<ColorScheme>(answer.load());
      },
      10, ColorScheme::Dark, [&] { ++changes; });
  // The constructor returned although the probe is still blocked, and current() is the initial value.
  EXPECT_EQ(watcher.current(), ColorScheme::Dark);
  release = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (watcher.current() != ColorScheme::Light && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  EXPECT_EQ(watcher.current(), ColorScheme::Light);
  EXPECT_EQ(changes.load(), 1);
  answer = static_cast<int>(ColorScheme::Dark);
  while (watcher.current() != ColorScheme::Dark && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  EXPECT_EQ(watcher.current(), ColorScheme::Dark);
  EXPECT_EQ(changes.load(), 2);
}

// ---- settings migration -----------------------------------------------------------------------------------

TEST(ThemeSettings, OldDarkDefaultBecomesSystemAndLightStays) {
  TempDir dir("theme_test_settings");
  const fs::path file = dir.path / "settings.json";
  writeFile(file, R"({"theme":"dark","uiScale":1.25})");  // written before WP-67e: dark was the default
  EXPECT_EQ(Settings::load(file).theme, ThemeKind::System);
  EXPECT_FLOAT_EQ(Settings::load(file).uiScale, 1.25f);
  writeFile(file, R"({"theme":"light"})");
  EXPECT_EQ(Settings::load(file).theme, ThemeKind::Light);
  writeFile(file, R"({"settingsVersion":2,"theme":"dark"})");  // written by this version: an explicit choice
  EXPECT_EQ(Settings::load(file).theme, ThemeKind::Dark);
  writeFile(file, R"({"theme":"system"})");
  EXPECT_EQ(Settings::load(file).theme, ThemeKind::System);
  writeFile(file, R"({"theme":"solarized"})");
  EXPECT_EQ(Settings::load(file).theme, ThemeKind::System);
}

TEST(ThemeSettings, EveryChoiceSurvivesASaveAndLoad) {
  TempDir dir("theme_test_roundtrip");
  for (const ThemeKind kind : {ThemeKind::System, ThemeKind::Light, ThemeKind::Dark}) {
    Settings s;
    s.theme = kind;
    ASSERT_TRUE(s.save(dir.path / "settings.json").ok());
    EXPECT_EQ(Settings::load(dir.path / "settings.json").theme, kind) << themeName(kind);
  }
}

TEST(ThemeSettings, MigrationHappensOnceThroughTheApp) {
  TempDir dir("theme_test_app_migration");
  writeFile(dir.path / "settings.json", R"({"theme":"dark"})");
  AppOptions o = headless();
  o.persist = true;
  o.configPath = dir.path / "settings.json";
  {
    App app(makeNullRenderer(), o);
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    EXPECT_EQ(app.settings().theme, ThemeKind::System);
    app.frame(0);
  }  // saved: the file now carries settingsVersion 2 and "system"
  EXPECT_EQ(Settings::load(dir.path / "settings.json").theme, ThemeKind::System);
  {
    App app(makeNullRenderer(), o);
    std::string error;
    ASSERT_TRUE(app.init(error)) << error;
    app.setTheme(ThemeKind::Dark);  // an explicit Dark chosen now must survive the next start
    app.frame(0);
  }
  EXPECT_EQ(Settings::load(dir.path / "settings.json").theme, ThemeKind::Dark);
}

// ---- the App: injected probe, live switch -----------------------------------------------------------------

TEST(ThemeApp, WithoutAProbeSystemIsDark) {
  App app(makeNullRenderer(), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  app.frame(0);
  EXPECT_EQ(app.settings().theme, ThemeKind::System);
  EXPECT_EQ(app.colorScheme(), ColorScheme::Dark);
}

TEST(ThemeApp, SystemFollowsTheInjectedProbeLive) {
  std::atomic<int> os{static_cast<int>(ColorScheme::Light)};
  AppOptions o = headless();
  o.osThemeProbe = [&os] { return static_cast<ColorScheme>(os.load()); };
  o.osThemePollMs = 10;
  App app(makeNullRenderer(), o);
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;

  ASSERT_TRUE(pumpUntil(app, [&] { return app.colorScheme() == ColorScheme::Light; }));
  EXPECT_GT(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x, 0.9f);  // the light window background
  os = static_cast<int>(ColorScheme::Dark);  // the user flips the desktop to dark while the viewer runs
  ASSERT_TRUE(pumpUntil(app, [&] { return app.colorScheme() == ColorScheme::Dark; }));
  EXPECT_LT(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x, 0.3f);
  os = static_cast<int>(ColorScheme::Light);
  ASSERT_TRUE(pumpUntil(app, [&] { return app.colorScheme() == ColorScheme::Light; }));

  // An explicit choice ignores the OS from then on, and System follows it again.
  app.setTheme(ThemeKind::Dark);
  app.frame(0);
  EXPECT_EQ(app.colorScheme(), ColorScheme::Dark);
  os = static_cast<int>(ColorScheme::Dark);
  os = static_cast<int>(ColorScheme::Light);
  for (int i = 0; i < 20; ++i) {
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(app.colorScheme(), ColorScheme::Dark);
  app.setTheme(ThemeKind::System);
  ASSERT_TRUE(pumpUntil(app, [&] { return app.colorScheme() == ColorScheme::Light; }));
}

TEST(ThemeApp, ThePaletteOfTheContextFollowsTheTheme) {
  App app(makeNullRenderer(), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  app.setTheme(ThemeKind::Light);
  app.frame(0);
  EXPECT_EQ(app.palette().scheme, ColorScheme::Light);
  EXPECT_EQ(&currentPalette(), &paletteFor(ColorScheme::Light));
  app.setTheme(ThemeKind::Dark);
  app.frame(0);
  EXPECT_EQ(&currentPalette(), &paletteFor(ColorScheme::Dark));
}

TEST(ThemeApp, CycleCommandWalksSystemLightDarkSystem) {
  App app(makeNullRenderer(), headless());
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  ASSERT_EQ(app.settings().theme, ThemeKind::System);
  ASSERT_TRUE(app.commands().run("view.theme.toggle"));
  EXPECT_EQ(app.settings().theme, ThemeKind::Light);
  ASSERT_TRUE(app.commands().run("view.theme.toggle"));
  EXPECT_EQ(app.settings().theme, ThemeKind::Dark);
  ASSERT_TRUE(app.commands().run("view.theme.toggle"));
  EXPECT_EQ(app.settings().theme, ThemeKind::System);
}

// ---- palettes ---------------------------------------------------------------------------------------------

TEST(Palette, ContrastRatioMatchesTheWcagDefinition) {
  EXPECT_NEAR(contrastRatio(makeRgba(0, 0, 0), makeRgba(255, 255, 255)), 21.0, 0.01);
  EXPECT_NEAR(contrastRatio(makeRgba(255, 255, 255), makeRgba(255, 255, 255)), 1.0, 1e-9);
  EXPECT_NEAR(contrastRatio(makeRgba(119, 119, 119), makeRgba(255, 255, 255)), 4.48, 0.02);  // #777 on white
}

TEST(Palette, BodyTextReadsOnEveryPanelBackgroundInBothThemes) {
  for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
    const ThemePalette& p = paletteFor(scheme);
    const char* name = scheme == ColorScheme::Light ? "light" : "dark";
    for (const Rgba bg : {p.windowBg, p.chromeBg, p.frameBg, p.hover, p.accentSoft, p.laneBg, p.laneBgAlt}) {
      EXPECT_GE(contrastRatio(p.text, bg), 4.5) << name << " text on " << std::hex << bg;
    }
    for (const Rgba bg : {p.windowBg, p.chromeBg, p.frameBg, p.laneBg}) {
      EXPECT_GE(contrastRatio(p.textMuted, bg), 4.5) << name << " muted text on " << std::hex << bg;
    }
    for (const Rgba fg : {p.warn, p.error, p.info}) {
      EXPECT_GE(contrastRatio(fg, p.windowBg), 4.5) << name << " semantic text " << std::hex << fg;
      EXPECT_GE(contrastRatio(fg, p.chromeBg), 4.5) << name << " semantic text on chrome " << std::hex << fg;
    }
    // The accent (selection, active tab, links) is visible against the panel (WCAG 3:1 for UI parts).
    EXPECT_GE(contrastRatio(p.accent, p.windowBg), 3.0) << name;
    EXPECT_GE(contrastRatio(p.accent, p.chromeBg), 3.0) << name;
    // Borders and hover are visible but quiet.
    EXPECT_GT(colorDistance(p.border, p.windowBg), 15.0) << name;
    EXPECT_GT(colorDistance(p.hover, p.windowBg), 10.0) << name;
    EXPECT_GT(colorDistance(p.accentSoft, p.windowBg), 30.0) << name;
  }
}

TEST(Palette, LightAndDarkAreBothNeutralAndNotPureBlackOrWhiteBackgrounds) {
  const ThemePalette& dark = paletteFor(ColorScheme::Dark);
  EXPECT_GT(rgbaR(dark.windowBg), 12);  // a neutral dark, not pure black
  EXPECT_LT(rgbaR(dark.windowBg), 60);
  const ThemePalette& light = paletteFor(ColorScheme::Light);
  EXPECT_GT(rgbaR(light.windowBg), 240);
  EXPECT_GT(rgbaR(light.chromeBg), 220);  // light grey chrome
  EXPECT_LT(rgbaR(light.chromeBg), 250);
  EXPECT_NE(dark.windowBg, light.windowBg);
}

TEST(Palette, ThreadStateColoursStayDistinctInBothThemes) {
  for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
    const StatePalette& s = paletteFor(scheme).states;
    const char* name = scheme == ColorScheme::Light ? "light" : "dark";
    const std::vector<std::pair<const char*, Rgba>> states = {
        {"running", s.running}, {"ready", s.ready},           {"syncWait", s.syncWait},
        {"ioWait", s.ioWait},   {"memoryWait", s.memoryWait}, {"otherWait", s.otherWait}};
    for (size_t i = 0; i < states.size(); ++i) {
      for (size_t j = i + 1; j < states.size(); ++j) {
        EXPECT_GE(colorDistance(states[i].second, states[j].second), 60.0)
            << name << ": " << states[i].first << " vs " << states[j].first;
      }
      // "Unknown" is background-ish: every real state stands out from it.
      EXPECT_GE(colorDistance(states[i].second, s.unknown), 60.0) << name << ": " << states[i].first;
    }
    // Colour-blind aware: green and red differ in lightness too (a deuteranope tells them apart by it), and
    // so do the two blues.
    EXPECT_GE(contrastRatio(s.running, s.syncWait), 1.4) << name;
    EXPECT_GE(contrastRatio(s.ioWait, s.memoryWait), 2.0) << name;
    // Unknown is close to the panel background (the band looks empty), not to any state.
    EXPECT_LT(colorDistance(s.unknown, paletteFor(scheme).laneBg), 45.0) << name;
    EXPECT_LT(colorDistance(s.unknown, paletteFor(scheme).windowBg), 45.0) << name;
  }
}

TEST(Palette, TimelineColoursDifferBetweenThemesWhereIntended) {
  using namespace tachyon::viewer::timeline;
  const StatePalette& dark = paletteFor(ColorScheme::Dark).states;
  const StatePalette& light = paletteFor(ColorScheme::Light).states;
  EXPECT_NE(dark.unknown, light.unknown);  // the "no data" colour follows the background
  EXPECT_LT(rgbaR(dark.unknown), 80);
  EXPECT_GT(rgbaR(light.unknown), 200);
  // The same band column is drawn in the active theme's colours.
  analysis::BandColumn run;
  run.running = 1;
  run.unknown = 0;
  analysis::BandColumn nothing;  // unknown = 1
  EXPECT_NE(bandColumnColor(nothing, dark), bandColumnColor(nothing, light));
  EXPECT_NEAR(colorR(bandColumnColor(nothing, light)), rgbaR(light.unknown), 16);
  EXPECT_NEAR(colorG(bandColumnColor(run, light)), rgbaG(light.running), 16);
  EXPECT_NEAR(colorG(bandColumnColor(run, dark)), rgbaG(dark.running), 16);
  // Every state keeps its hue family in both themes: green, amber, red/pink, dark blue, light blue, grey.
  for (const StatePalette* s : {&dark, &light}) {
    EXPECT_GT(rgbaG(s->running), rgbaR(s->running) + 60);
    EXPECT_GT(rgbaR(s->ready), 200);
    EXPECT_LT(rgbaB(s->ready), 80);
    EXPECT_GT(rgbaR(s->syncWait), rgbaB(s->syncWait) + 60);
    EXPECT_GT(rgbaB(s->ioWait), rgbaR(s->ioWait) + 80);
    EXPECT_GT(rgbaB(s->memoryWait), 200);
    EXPECT_LT(std::abs(static_cast<int>(rgbaR(s->otherWait)) - static_cast<int>(rgbaB(s->otherWait))), 25);
  }
}

TEST(Palette, ApplyThemeSetsTheImGuiColoursFromThePalette) {
  ImGuiContext* ctx = ImGui::CreateContext();
  for (const ColorScheme scheme : {ColorScheme::Light, ColorScheme::Dark}) {
    applyTheme(scheme, 1.0f);
    const ThemePalette& p = paletteFor(scheme);
    const ImVec4 text = ImGui::GetStyle().Colors[ImGuiCol_Text];
    EXPECT_NEAR(text.x, rgbaR(p.text) / 255.0f, 0.01f);
    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    EXPECT_NEAR(bg.y, rgbaG(p.windowBg) / 255.0f, 0.01f);
    EXPECT_FLOAT_EQ(bg.w, 1.0f);
    const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_TabSelectedOverline];
    EXPECT_NEAR(accent.z, rgbaB(p.accent) / 255.0f, 0.01f);
    EXPECT_EQ(&currentPalette(), &p);
  }
  ImGui::DestroyContext(ctx);
}
