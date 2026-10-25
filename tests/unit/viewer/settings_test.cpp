#include "viewer/settings.h"
#include "viewer/paths.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

class SettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_dir = fs::path(TACHYON_BINARY_DIR) / "viewer_settings_test" /
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(m_dir);
    fs::create_directories(m_dir);
  }
  void TearDown() override { fs::remove_all(m_dir); }
  static void write(const fs::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
  }
  fs::path m_dir;
};

}  // namespace

TEST_F(SettingsTest, RoundTrip) {
  Settings s;
  s.symbolServers = {"https://a.example/symbols", "https://b.example"};
  s.cacheDir = "/var/cache/sym";
  s.sourcePathMappings = {{"C:/build/src", "/home/me/src"}, {"D:/x", "/y"}};
  s.theme = ThemeKind::Light;
  s.uiScale = 1.5f;
  s.addRecentFile("/a.tachyon");
  s.addRecentFile("/b.tachyon");

  const fs::path file = m_dir / "nested" / "settings.json";  // parent directory is created
  ASSERT_TRUE(s.save(file).ok());
  EXPECT_EQ(Settings::load(file), s);
  EXPECT_FALSE(fs::exists(file.string() + ".tmp"));
}

TEST_F(SettingsTest, MissingFileGivesDefaults) {
  EXPECT_EQ(Settings::load(m_dir / "nope.json"), Settings{});
}

TEST_F(SettingsTest, CorruptFileFallsBackToDefaults) {
  const fs::path file = m_dir / "settings.json";
  write(file, "{ this is not json");
  EXPECT_EQ(Settings::load(file), Settings{});
  write(file, "[1,2,3]");
  EXPECT_EQ(Settings::load(file), Settings{});
  write(file, "");
  EXPECT_EQ(Settings::load(file), Settings{});
}

TEST_F(SettingsTest, MissingFieldsAreDefaulted) {
  const fs::path file = m_dir / "settings.json";
  write(file, R"({"theme":"light","recentFiles":["x.tachyon"]})");
  const Settings s = Settings::load(file);
  Settings expected;
  expected.theme = ThemeKind::Light;
  expected.recentFiles = {"x.tachyon"};
  EXPECT_EQ(s, expected);
}

TEST_F(SettingsTest, WrongTypesAreDefaultedPerField) {
  const fs::path file = m_dir / "settings.json";
  write(file, R"({"theme":5,"uiScale":"big","symbolServers":"x","cacheDir":"/c",
                  "sourcePathMappings":[{"from":"a","to":"b"},{"from":1},"junk"]})");
  const Settings s = Settings::load(file);
  EXPECT_EQ(s.theme, ThemeKind::Dark);
  EXPECT_EQ(s.uiScale, 1.0f);
  EXPECT_EQ(s.symbolServers, Settings{}.symbolServers);
  EXPECT_EQ(s.cacheDir, "/c");
  ASSERT_EQ(s.sourcePathMappings.size(), 1u);
  EXPECT_EQ(s.sourcePathMappings[0], (SourcePathMapping{"a", "b"}));
}

TEST_F(SettingsTest, UiScaleIsClamped) {
  const fs::path file = m_dir / "settings.json";
  write(file, R"({"uiScale":100})");
  EXPECT_EQ(Settings::load(file).uiScale, Settings::kMaxUiScale);
  write(file, R"({"uiScale":0.01})");
  EXPECT_EQ(Settings::load(file).uiScale, Settings::kMinUiScale);
}

TEST_F(SettingsTest, RecentFilesAreDeduplicatedAndCapped) {
  Settings s;
  for (int i = 0; i < 30; ++i) s.addRecentFile("f" + std::to_string(i));
  EXPECT_EQ(s.recentFiles.size(), Settings::kMaxRecentFiles);
  EXPECT_EQ(s.recentFiles.front(), "f29");
  s.addRecentFile("f25");
  EXPECT_EQ(s.recentFiles.front(), "f25");
  EXPECT_EQ(std::count(s.recentFiles.begin(), s.recentFiles.end(), "f25"), 1);
}

TEST_F(SettingsTest, SaveFailureIsReported) {
  const fs::path blocker = m_dir / "file";
  write(blocker, "x");
  EXPECT_FALSE(Settings{}.save(blocker / "settings.json").ok());  // parent is a regular file
}

TEST(ConfigDir, FollowsPlatformConventions) {
  auto env = [](std::map<std::string, std::string> vars) {
    return [vars](const char* name) {
      const auto it = vars.find(name);
      return it == vars.end() ? std::string() : it->second;
    };
  };
  EXPECT_EQ(configDirFromEnv(env({{"XDG_CONFIG_HOME", "/xdg"}, {"HOME", "/home/u"}}), false),
            fs::path("/xdg") / "tachyon");
  EXPECT_EQ(configDirFromEnv(env({{"HOME", "/home/u"}}), false), fs::path("/home/u") / ".config" / "tachyon");
  EXPECT_EQ(configDirFromEnv(env({}), false), fs::path());
  EXPECT_EQ(configDirFromEnv(env({{"APPDATA", "/ad"}}), true), fs::path("/ad") / "Tachyon");
  EXPECT_EQ(configDirFromEnv(env({}), true), fs::path());
}
