// WP-34: SourceLocator (as-is, remap rules, search roots, caching, containment).
#include "symbols/source_locator.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>

namespace tachyon::symbols {
namespace {

namespace fs = std::filesystem;

struct Tree {
  fs::path root;
  Tree() {
    static int counter = 0;
    root =
        fs::temp_directory_path() /
        ("tachyon_wp34_src_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         "_" + std::to_string(counter++));
    fs::remove_all(root);
    fs::create_directories(root);
  }
  ~Tree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  fs::path file(const fs::path& rel) const {
    const fs::path p = root / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "// source\n";
    return p;
  }
};

TEST(SourceLocatorTest, PathAsIsWins) {
  Tree t;
  const fs::path f = t.file("a/b.cpp");
  SourceLocatorConfig cfg;
  cfg.remaps.push_back({f.string(), "/nonexistent"});
  SourceLocator loc(cfg);
  auto r = loc.locate(f.string());
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, f);
}

TEST(SourceLocatorTest, RemapReplacesPrefixOnComponentBoundaries) {
  Tree t;
  const fs::path f = t.file("engine/src/game/a.cpp");
  SourceLocatorConfig cfg;
  cfg.remaps.push_back({"C:\\build\\eng", (t.root / "wrong").string()});      // not a whole-component prefix
  cfg.remaps.push_back({"C:\\build\\engine", (t.root / "engine").string()});  // matches
  SourceLocator loc(cfg);
  auto r = loc.locate("C:\\build\\engine\\src\\game\\a.cpp");
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(*r, f);
  // Windows paths compare case-insensitively and accept either separator.
  auto r2 = loc.locate("c:/BUILD/Engine/src/game/a.cpp");
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(*r2, f);
  // A rule whose result does not exist is skipped; nothing else matches.
  EXPECT_FALSE(loc.locate("C:\\build\\engine\\src\\game\\missing.cpp").ok());
}

TEST(SourceLocatorTest, RulesAreTriedInOrderAndFallThrough) {
  Tree t;
  const fs::path f = t.file("second/x/y.cpp");
  SourceLocatorConfig cfg;
  cfg.remaps.push_back({"/build", (t.root / "first").string()});   // result does not exist
  cfg.remaps.push_back({"/build", (t.root / "second").string()});  // exists
  SourceLocator loc(cfg);
  auto r = loc.locate("/build/x/y.cpp");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, f);
}

TEST(SourceLocatorTest, RemapNeverLeavesTheTargetDirectory) {
  Tree t;
  t.file("secret/pw.txt");
  t.file("repo/ok.cpp");
  SourceLocatorConfig cfg;
  cfg.remaps.push_back({"/build", (t.root / "repo").string()});
  SourceLocator loc(cfg);
  EXPECT_FALSE(loc.locate("/build/../secret/pw.txt").ok());
  EXPECT_TRUE(loc.locate("/build/ok.cpp").ok());
#if !defined(_WIN32)
  // A symlink inside the target that points out of it is not followed either.
  std::error_code ec;
  fs::create_symlink(t.root / "secret", t.root / "repo" / "link", ec);
  if (!ec) EXPECT_FALSE(loc.locate("/build/link/pw.txt").ok());
#endif
}

TEST(SourceLocatorTest, SearchRootsMatchTrailingComponentsLongestFirst) {
  Tree t;
  const fs::path deep = t.file("rootA/src/game/util.cpp");
  const fs::path shallow = t.file("rootB/util.cpp");
  SourceLocatorConfig cfg;
  cfg.searchRoots = {t.root / "rootB", t.root / "rootA"};
  SourceLocator loc(cfg);
  // The longer suffix src/game/util.cpp beats the basename match in the earlier root.
  auto r = loc.locate("D:\\jenkins\\ws\\src\\game\\util.cpp");
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(*r, deep);
  // Only the basename exists in rootB.
  auto r2 = loc.locate("/other/place/util.cpp");
  ASSERT_TRUE(r2.ok());
  EXPECT_EQ(*r2, shallow);
  EXPECT_FALSE(loc.locate("/other/place/none.cpp").ok());
  EXPECT_FALSE(loc.locate("").ok());
}

TEST(SourceLocatorTest, SearchNeverFollowsParentComponents) {
  Tree t;
  t.file("outside/pw.txt");
  t.file("roots/inner/keep.cpp");
  SourceLocatorConfig cfg;
  cfg.searchRoots = {t.root / "roots" / "inner"};
  SourceLocator loc(cfg);
  EXPECT_FALSE(loc.locate("/x/../../outside/pw.txt").ok());
  EXPECT_TRUE(loc.locate("/x/keep.cpp").ok());
}

TEST(SourceLocatorTest, ResultsAreCachedUntilCleared) {
  Tree t;
  SourceLocatorConfig cfg;
  cfg.searchRoots = {t.root};
  SourceLocator loc(cfg);
  EXPECT_FALSE(loc.locate("/p/late.cpp").ok());
  const fs::path f = t.file("late.cpp");
  EXPECT_FALSE(loc.locate("/p/late.cpp").ok());  // negative result cached
  loc.clearCache();
  auto r = loc.locate("/p/late.cpp");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(*r, f);
  fs::remove(f);
  EXPECT_TRUE(loc.locate("/p/late.cpp").ok());  // positive result cached
  SourceLocatorConfig empty;
  loc.configure(empty);
  EXPECT_FALSE(loc.locate("/p/late.cpp").ok());
  EXPECT_TRUE(loc.config().searchRoots.empty());
}

TEST(SourceLocatorTest, CacheIsBoundedAndKeepsRecentEntries) {
  Tree t;
  t.file("keep.cpp");
  SourceLocatorConfig cfg;
  cfg.searchRoots = {t.root};
  SourceLocator loc(cfg);
  ASSERT_TRUE(loc.locate("/p/keep.cpp").ok());
  for (size_t i = 0; i < SourceLocator::kMaxCacheEntries + 100; ++i) {
    (void)loc.locate("/p/missing" + std::to_string(i) + ".cpp");
    if (i % 1000 == 0) (void)loc.locate("/p/keep.cpp");  // stays recently used
  }
  EXPECT_LE(loc.cacheSize(), SourceLocator::kMaxCacheEntries);
  EXPECT_GE(loc.cacheSize(), SourceLocator::kMaxCacheEntries - 1);
  // The oldest negative entry was evicted, so it is looked up again and now found.
  t.file("missing0.cpp");
  EXPECT_TRUE(loc.locate("/p/missing0.cpp").ok());
}

}  // namespace
}  // namespace tachyon::symbols
