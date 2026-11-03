// WP-45: the pure parts of the Source panel: SourceText (line splitting, binary / size checks, search),
// display helpers, the Locate-file rule derivation against the real SourceLocator, the heat scale and the new
// source path settings.
#include "source_test_util.h"

#include "symbols/source_locator.h"
#include "viewer/paths.h"
#include "viewer/settings.h"
#include "viewer/source/source_model.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::source;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<const SourceText> parseOk(std::string text) {
  auto r = SourceText::parse(std::move(text));
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  return r.ok() ? *r : nullptr;
}

using tachyon::viewer::source_test::TempTree;

}  // namespace

TEST(SourceText, SplitsAllLineEndingsAndKeepsTheLastLine) {
  auto t = parseOk("a\nb\r\nc\rd");
  ASSERT_NE(t, nullptr);
  ASSERT_EQ(t->lineCount(), 4u);
  EXPECT_EQ(t->line(1), "a");
  EXPECT_EQ(t->line(2), "b");
  EXPECT_EQ(t->line(3), "c");
  EXPECT_EQ(t->line(4), "d");
  EXPECT_EQ(t->line(0), "");
  EXPECT_EQ(t->line(5), "");
}

TEST(SourceText, TrailingTerminatorDoesNotAddALine) {
  EXPECT_EQ(parseOk("a\nb\n")->lineCount(), 2u);
  EXPECT_EQ(parseOk("a\n\n")->lineCount(), 2u);  // the empty second line counts
  EXPECT_EQ(parseOk("")->lineCount(), 0u);
  EXPECT_EQ(parseOk("\n")->lineCount(), 1u);
}

TEST(SourceText, SkipsAByteOrderMark) {
  auto t = parseOk("\xEF\xBB\xBFint x;\nfoo\n");
  ASSERT_EQ(t->lineCount(), 2u);
  EXPECT_EQ(t->line(1), "int x;");
}

TEST(SourceText, BinaryContentIsRejected) {
  std::string bin = "MZ\x90";
  bin.push_back('\0');
  bin += "rest";
  auto r = SourceText::parse(bin);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::CorruptData);
}

TEST(SourceText, MaxColumnsExpandsTabs) {
  auto t = parseOk("\tx\n\t\t\tfoo\nab\tc\n");
  // "\tx" = 4 + 1; "\t\t\tfoo" = 12 + 3; "ab\tc": tab stops at 4 -> a b . . c = 5
  EXPECT_EQ(t->maxColumns(), 15u);
}

TEST(SourceText, FileSizeCapAndMissingFile) {
  TempTree tree("size_cap");
  const fs::path big = tree.root() / "big.cpp";
  {
    std::ofstream out(big, std::ios::binary);
    std::string chunk(1 << 20, 'x');
    chunk.back() = '\n';
    for (int i = 0; i < 17; ++i) out << chunk;  // 17 MiB
  }
  auto r = SourceText::load(big);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  const fs::path ok = tree.write("ok.cpp", "int main() {}\n");
  auto good = SourceText::load(ok);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ((*good)->lineCount(), 1u);
  auto missing = SourceText::load(tree.root() / "nope.cpp");
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
}

TEST(SourceText, SimpleLinesAreTheOnesWhereColumnsEqualBytes) {
  auto t = parseOk("plain text\n\ttab\ncaf\xC3\xA9\nctl\x01x\n");
  ASSERT_EQ(t->lineCount(), 4u);
  EXPECT_TRUE(t->lineIsSimple(1));
  EXPECT_FALSE(t->lineIsSimple(2));
  EXPECT_FALSE(t->lineIsSimple(3));
  EXPECT_FALSE(t->lineIsSimple(4));
  EXPECT_FALSE(t->lineIsSimple(9));
  // A stray continuation byte counts a cell like expandForDisplay does.
  auto stray = parseOk("a\x80\x80z\n");
  EXPECT_EQ(stray->maxColumns(), static_cast<uint32_t>(expandForDisplay(stray->line(1)).size()));
}

TEST(SourceText, AbsurdLineCountsAreRejected) {
  auto r = SourceText::parse(std::string(kMaxSourceLines + 1u, '\n'));
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
  EXPECT_TRUE(SourceText::parse(std::string(kMaxSourceLines, '\n')).ok());
}

TEST(SourceText, LoadHonoursTheCancelFlag) {
  TempTree tree("cancel");
  const fs::path p = tree.write("a.cpp", std::string(3 << 20, 'a') + "\n");
  std::atomic<bool> stop{true};
  auto r = SourceText::load(p, analysis::Cancel{&stop});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}

TEST(SourceText, FindNextAndPreviousAreCaseInsensitiveAndWrapFree) {
  auto t = parseOk("Alpha\nbeta\nGAMMA beta\ndelta\n");
  EXPECT_EQ(t->findNext("beta", 1), 2u);
  EXPECT_EQ(t->findNext("beta", 3), 3u);
  EXPECT_EQ(t->findNext("beta", 4), 0u);
  EXPECT_EQ(t->findNext("ALPHA", 1), 1u);
  EXPECT_EQ(t->findNext("zzz", 1), 0u);
  EXPECT_EQ(t->findNext("", 1), 0u);
  EXPECT_EQ(t->findNext("a\nb", 1), 0u);  // a needle with a line break never matches
  EXPECT_EQ(t->findPrev("beta", 4), 3u);
  EXPECT_EQ(t->findPrev("beta", 2), 2u);
  EXPECT_EQ(t->findPrev("beta", 1), 0u);
  EXPECT_EQ(t->findPrev("gamma", 100), 3u);  // clamped to the last line
}

TEST(SourceText, FindAgreesWithABruteForceScanOnALargeFile) {
  std::string text;
  std::vector<std::string> lines;
  uint32_t x = 12345;
  for (int i = 0; i < 4000; ++i) {
    x = x * 1664525u + 1013904223u;
    std::string l = "line " + std::to_string(i) + " v" + std::to_string(x % 97);
    if (x % 5 == 0) l += " NeedleX";
    lines.push_back(l);
    text += l + (i % 3 == 0 ? "\r\n" : "\n");
  }
  auto t = parseOk(text);
  ASSERT_EQ(t->lineCount(), 4000u);
  std::vector<uint32_t> expected, got;
  for (uint32_t i = 0; i < lines.size(); ++i)
    if (findInsensitive(lines[i], "needlex") != std::string::npos) expected.push_back(i + 1);
  for (uint32_t l = 0; (l = t->findNext("needleX", l + 1)) != 0;) got.push_back(l);
  EXPECT_EQ(got, expected);
  ASSERT_FALSE(expected.empty());
  EXPECT_EQ(t->findPrev("NEEDLEX", 4000), expected.back());
}

TEST(SourceDisplay, ExpandsTabsAndReplacesWhatTheFontCannotShow) {
  EXPECT_EQ(expandForDisplay("a\tb"), "a   b");
  EXPECT_EQ(expandForDisplay("\tx"), "    x");
  EXPECT_EQ(expandForDisplay("ab\x01"), "ab.");
  EXPECT_EQ(expandForDisplay("caf\xC3\xA9!"), "caf?!");  // one cell per code point
  EXPECT_EQ(expandForDisplay("\xE2\x82\xAC"), "?");
  EXPECT_EQ(expandForDisplay("\xFF\xFE"), "??");  // malformed bytes: one cell each
  EXPECT_EQ(expandForDisplay("abcdefghij", 4), "abcd");
  EXPECT_EQ(displayColumn("a\tb", 0), 0u);
  EXPECT_EQ(displayColumn("a\tb", 2), 4u);
  EXPECT_EQ(displayColumn("caf\xC3\xA9!", 5), 4u);
  EXPECT_EQ(displayColumn("caf\xC3\xA9!", 6), 5u);
}

TEST(SourceDisplay, FindInsensitive) {
  EXPECT_EQ(findInsensitive("Hello World", "WORLD"), 6u);
  EXPECT_EQ(findInsensitive("Hello World", "o", 5), 7u);
  EXPECT_EQ(findInsensitive("abc", "abcd"), std::string_view::npos);
  EXPECT_EQ(findInsensitive("abc", ""), 0u);
  EXPECT_EQ(findInsensitive("a_B", "_b"), 1u);
}

TEST(SourceHeat, IsMonotonicAndLogScaled) {
  EXPECT_EQ(heatOf(0, 100), 0.0f);
  EXPECT_EQ(heatOf(100, 100), 1.0f);
  EXPECT_EQ(heatOf(500, 100), 1.0f);
  EXPECT_EQ(heatOf(5, 0), 0.0f);
  float prev = 0.0f;
  for (uint64_t t = 1; t <= 1000; t += 7) {
    const float h = heatOf(t, 1000);
    EXPECT_GE(h, prev);
    EXPECT_LE(h, 1.0f);
    prev = h;
  }
  // Log scale: 1% of the maximum is far above 1% of the heat.
  EXPECT_GT(heatOf(10, 1000), 0.3f);
}

// ---- Locate file: derive a rule and check it with the real SourceLocator ----

TEST(SourceLocate, WindowsDebugPathMapsToACheckout) {
  TempTree tree("locate_win");
  const fs::path file = tree.write("checkout/engine/src/a.cpp", "x\n");
  const std::string debug = "C:\\build\\agent\\engine\\src\\a.cpp";
  const auto rule = deriveRemapRule(debug, file);
  ASSERT_TRUE(rule.has_value());
  EXPECT_EQ(rule->from, "C:\\build\\agent");
  EXPECT_EQ(fs::path(rule->to), tree.root() / "checkout");
  symbols::SourceLocator locator(symbols::SourceLocatorConfig{{*rule}, {}});
  auto found = locator.locate(debug);
  ASSERT_TRUE(found.ok()) << found.error().toString();
  EXPECT_EQ(fs::weakly_canonical(*found), fs::weakly_canonical(file));
  // The rule is a prefix rule: a sibling file in the same tree resolves too.
  tree.write("checkout/engine/src/b.cpp", "y\n");
  EXPECT_TRUE(locator.locate("C:\\build\\agent\\engine\\src\\b.cpp").ok());
}

TEST(SourceLocate, CaseDifferencesStillGiveARuleThatResolvesToTheChosenFile) {
  TempTree tree("locate_case");
  // The suffix match ignores case, a prefix remap does not on a case-sensitive file system: the rule that is
  // returned must resolve to the chosen file either way (an exact-path rule when the prefix rule would not).
  const fs::path file = tree.write("engine/src/a.cpp", "x\n");
  for (const char* debug : {"C:\\Build\\Engine\\SRC\\A.CPP", "D:/Work/engine/src/a.cpp"}) {
    const auto rule = deriveRemapRule(debug, file);
    ASSERT_TRUE(rule.has_value()) << debug;
    symbols::SourceLocator locator(symbols::SourceLocatorConfig{{*rule}, {}});
    auto found = locator.locate(debug);
    ASSERT_TRUE(found.ok()) << debug << " -> " << rule->from << " => " << rule->to;
    EXPECT_TRUE(fs::equivalent(*found, file)) << debug;
    const LocateOutcome o = locateOutcome(debug, file, false);
    ASSERT_TRUE(o.rule.has_value());
    EXPECT_EQ(o.rule->from, rule->from);
  }
  // A prefix rule is kept when it resolves (the usual same-case checkout).
  const auto same = deriveRemapRule("D:/Work/engine/src/a.cpp", file);
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(same->from, "D:/Work");
}

TEST(SourceLocate, AbsolutePosixPathsKeepTheRootAsTarget) {
  TempTree tree("locate_posix");
  const fs::path file = tree.write("srcs/lib/b.cpp", "x\n");
  // Debug path from another machine; the chosen file shares only "lib/b.cpp".
  const auto rule = deriveRemapRule("/home/ci/work/lib/b.cpp", file);
  ASSERT_TRUE(rule.has_value());
  EXPECT_EQ(rule->from, "/home/ci/work");
  EXPECT_EQ(fs::path(rule->to), tree.root() / "srcs");
#ifndef _WIN32
  // Same absolute path under a new root: everything of the chosen path is shared.
  const auto shifted = deriveRemapRule("/nonexistent-root" + pathToUtf8(file), file);
  ASSERT_TRUE(shifted.has_value());
  EXPECT_EQ(shifted->from, "/nonexistent-root");
  EXPECT_EQ(shifted->to, "/");
  symbols::SourceLocator locator(symbols::SourceLocatorConfig{{*shifted}, {}});
  EXPECT_TRUE(locator.locate("/nonexistent-root" + pathToUtf8(file)).ok());
#endif
}

TEST(SourceLocate, FileWithAnotherNameBecomesAnExactMapping) {
  TempTree tree("locate_rename");
  const fs::path file = tree.write("elsewhere/renamed.cpp", "x\n");
  const auto rule = deriveRemapRule("C:/b/old.cpp", file);
  ASSERT_TRUE(rule.has_value());
  EXPECT_EQ(rule->from, "C:/b/old.cpp");
  symbols::SourceLocator locator(symbols::SourceLocatorConfig{{*rule}, {}});
  auto found = locator.locate("C:/b/old.cpp");
  ASSERT_TRUE(found.ok()) << found.error().toString();
  EXPECT_EQ(fs::weakly_canonical(*found), fs::weakly_canonical(file));
}

TEST(SourceLocate, RelativeDebugPathsBecomeSearchRoots) {
  TempTree tree("locate_root");
  const fs::path file = tree.write("proj/src/engine.cpp", "x\n");
  EXPECT_FALSE(deriveRemapRule("src/engine.cpp", file).has_value());
  const LocateOutcome o = locateOutcome("src/engine.cpp", file, false);
  EXPECT_FALSE(o.rule.has_value());
  ASSERT_TRUE(o.root.has_value());
  EXPECT_EQ(*o.root, tree.root() / "proj");
  symbols::SourceLocator locator(symbols::SourceLocatorConfig{{}, {*o.root}});
  EXPECT_TRUE(locator.locate("src/engine.cpp").ok());
}

TEST(SourceLocate, ADirectoryIsASearchRoot) {
  TempTree tree("locate_dir");
  const LocateOutcome o = locateOutcome("C:/x/y.cpp", tree.root(), true);
  EXPECT_FALSE(o.rule.has_value());
  ASSERT_TRUE(o.root.has_value());
  EXPECT_EQ(*o.root, tree.root());
}

// ---- Settings ----

TEST(SourceSettings, RulesAndRootsAreAddedOnceAndPersist) {
  TempTree tree("settings");
  Settings s;
  EXPECT_TRUE(s.addSourcePathMapping({"C:/b", "/home/me"}));
  EXPECT_FALSE(s.addSourcePathMapping({"C:/b", "/home/me"}));
  EXPECT_FALSE(s.addSourcePathMapping({"", "/x"}));
  EXPECT_FALSE(s.addSourcePathMapping({"C:/c", ""}));
  EXPECT_TRUE(s.addSourceRoot("/srv/src"));
  EXPECT_FALSE(s.addSourceRoot("/srv/src"));
  EXPECT_FALSE(s.addSourceRoot(""));
  const fs::path file = tree.root() / "settings.json";
  ASSERT_TRUE(s.save(file).ok());
  const Settings loaded = Settings::load(file);
  EXPECT_EQ(loaded.sourcePathMappings, s.sourcePathMappings);
  EXPECT_EQ(loaded.sourceRoots, s.sourceRoots);
  EXPECT_EQ(loaded, s);
}

TEST(SourceSettings, AnInvalidRootsFieldFallsBackToEmpty) {
  TempTree tree("settings_bad");
  const fs::path file = tree.write("s.json", R"({"sourceRoots": 5, "sourcePathMappings": []})");
  const Settings loaded = Settings::load(file);
  EXPECT_TRUE(loaded.sourceRoots.empty());
}
