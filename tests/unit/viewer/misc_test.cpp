// Small pure pieces of the viewer: CLI parsing, command matching, directory listing, scale maths.
#include "viewer/cli_args.h"
#include "viewer/command_registry.h"
#include "viewer/file_browser.h"
#include "viewer/paths.h"
#include "viewer/settings.h"
#include "viewer/theme.h"

#include <gtest/gtest.h>

#include <map>

#include <fstream>

using namespace tachyon::viewer;
namespace fs = std::filesystem;

TEST(CliArgs, ParsesPositionalConfigAndSelftest) {
  const CliArgs a = parseCliArgs({"--config", "c.json", "trace.tachyon"});
  EXPECT_TRUE(a.error.empty());
  EXPECT_EQ(a.openPath, fs::path("trace.tachyon"));
  EXPECT_EQ(a.configPath, fs::path("c.json"));
  EXPECT_FALSE(a.selftestPath);

  const CliArgs b = parseCliArgs({"--selftest", "t.tachyon"});
  EXPECT_EQ(b.selftestPath, fs::path("t.tachyon"));
  EXPECT_TRUE(parseCliArgs({"-h"}).help);
}

TEST(CliArgs, RejectsBadInput) {
  EXPECT_FALSE(parseCliArgs({"--selftest"}).error.empty());
  EXPECT_FALSE(parseCliArgs({"--bogus"}).error.empty());
  EXPECT_FALSE(parseCliArgs({"a", "b"}).error.empty());
}

TEST(CommandRegistry, MatchesAndRuns) {
  CommandRegistry r;
  int ran = 0;
  r.add({"a", "File: Open Trace", "", [&] { ++ran; }});
  r.add({"b", "View: Toggle Theme", "", nullptr});
  r.add({"c", "Help: Open Palette", "", nullptr});

  EXPECT_EQ(r.match("").size(), 3u);
  const auto open = r.match("open");
  ASSERT_EQ(open.size(), 2u);
  const auto prefix = r.match("view");
  ASSERT_EQ(prefix.size(), 1u);
  EXPECT_EQ(prefix[0]->id, "b");
  const auto fuzzy = r.match("ttheme");  // subsequence
  ASSERT_EQ(fuzzy.size(), 1u);
  EXPECT_EQ(fuzzy[0]->id, "b");
  EXPECT_TRUE(r.match("zzz").empty());

  EXPECT_TRUE(r.run("a"));
  EXPECT_EQ(ran, 1);
  EXPECT_FALSE(r.run("missing"));

  r.add({"a", "File: Open Trace v2", "", nullptr});  // same id replaces
  EXPECT_EQ(r.commands().size(), 3u);
}

TEST(FileBrowser, ListsDirectoriesFirstAndFiltersByExtension) {
  const fs::path dir = fs::path(TACHYON_BINARY_DIR) / "viewer_listing_test";
  fs::remove_all(dir);
  fs::create_directories(dir / "zdir");
  fs::create_directories(dir / "Adir");
  std::ofstream(dir / "b.tachyon") << "x";
  std::ofstream(dir / "A.TACHYON") << "xy";
  std::ofstream(dir / "c.txt") << "x";

  const DirListing filtered = listDirectory(dir, ".tachyon");
  ASSERT_TRUE(filtered.error.empty());
  ASSERT_EQ(filtered.entries.size(), 4u);
  EXPECT_EQ(filtered.entries[0].name, "Adir");
  EXPECT_EQ(filtered.entries[1].name, "zdir");
  EXPECT_EQ(filtered.entries[2].name, "A.TACHYON");
  EXPECT_EQ(filtered.entries[2].size, 2u);
  EXPECT_EQ(filtered.entries[3].name, "b.tachyon");

  EXPECT_EQ(listDirectory(dir, "").entries.size(), 5u);
  EXPECT_FALSE(listDirectory(dir / "nope", "").error.empty());
  fs::remove_all(dir);
}

TEST(Theme, EffectiveScaleIsClamped) {
  EXPECT_FLOAT_EQ(effectiveUiScale(1.0f, 1.0f), 1.0f);
  EXPECT_FLOAT_EQ(effectiveUiScale(2.0f, 1.5f), 3.0f);
  EXPECT_FLOAT_EQ(effectiveUiScale(4.0f, 4.0f), Settings::kMaxUiScale);
  EXPECT_FLOAT_EQ(effectiveUiScale(0.1f, 0.1f), Settings::kMinUiScale);
}

TEST(ViewerPaths, SymbolCacheDirFollowsPlatformConventions) {
  const auto env = [](std::map<std::string, std::string> vars) {
    return [vars](const char* name) {
      const auto it = vars.find(name);
      return it == vars.end() ? std::string() : it->second;
    };
  };
  EXPECT_EQ(symbolCacheDirFromEnv(env({{"XDG_CACHE_HOME", "/x"}, {"HOME", "/h"}}), false),
            std::filesystem::path("/x/tachyon/symbols"));
  EXPECT_EQ(symbolCacheDirFromEnv(env({{"HOME", "/h"}}), false),
            std::filesystem::path("/h/.cache/tachyon/symbols"));
  EXPECT_EQ(symbolCacheDirFromEnv(env({{"LOCALAPPDATA", "C:/L"}}), true),
            std::filesystem::path("C:/L/Tachyon/symbols"));
  EXPECT_TRUE(symbolCacheDirFromEnv(env({}), false).empty());
  EXPECT_TRUE(symbolCacheDirFromEnv(env({}), true).empty());
  // No symbol servers unless configured.
  EXPECT_TRUE(Settings{}.symbolServers.empty());
}
