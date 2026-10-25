// child_filter glob matcher (WP-13): '*' and '?' only, platform independent.
#include "capture/windows_etw/glob_match.h"

#include <gtest/gtest.h>

#include <string>

namespace tachyon::capture::etw {
namespace {

TEST(GlobMatch, LiteralAndEmpty) {
  EXPECT_TRUE(globMatch("game.exe", "game.exe", false));
  EXPECT_FALSE(globMatch("game.exe", "game.exe2", false));
  EXPECT_FALSE(globMatch("game.exe", "game.ex", false));
  EXPECT_TRUE(globMatch("", "", false));
  EXPECT_FALSE(globMatch("", "x", false));
  EXPECT_FALSE(globMatch("x", "", false));
}

TEST(GlobMatch, Star) {
  EXPECT_TRUE(globMatch("*", "", false));
  EXPECT_TRUE(globMatch("*", "anything.exe", false));
  EXPECT_TRUE(globMatch("*.exe", "tachyon-testbed.exe", false));
  EXPECT_FALSE(globMatch("*.exe", "tachyon-testbed.dll", false));
  EXPECT_TRUE(globMatch("game*", "game", false));
  EXPECT_TRUE(globMatch("game*", "gameworker.exe", false));
  EXPECT_TRUE(globMatch("a*b*c", "aXXbYYc", false));
  EXPECT_TRUE(globMatch("a*b*c", "abc", false));
  EXPECT_FALSE(globMatch("a*b*c", "acb", false));
  EXPECT_TRUE(globMatch("**x", "x", false));
  EXPECT_TRUE(globMatch("*.e*e", "a.exe", false));
  // The star must be able to give characters back.
  EXPECT_TRUE(globMatch("*ab", "aab", false));
  EXPECT_TRUE(globMatch("*aab", "aaab", false));
}

TEST(GlobMatch, QuestionMark) {
  EXPECT_TRUE(globMatch("?", "a", false));
  EXPECT_FALSE(globMatch("?", "", false));
  EXPECT_FALSE(globMatch("?", "ab", false));
  EXPECT_TRUE(globMatch("worker-?.exe", "worker-7.exe", false));
  EXPECT_FALSE(globMatch("worker-?.exe", "worker-10.exe", false));
  EXPECT_TRUE(globMatch("w?r*", "worker.exe", false));
}

TEST(GlobMatch, OtherCharactersAreLiteral) {
  EXPECT_TRUE(globMatch("[ab].exe", "[ab].exe", false));
  EXPECT_FALSE(globMatch("[ab].exe", "a.exe", false));
  EXPECT_TRUE(globMatch("a.b", "a.b", false));
  EXPECT_FALSE(globMatch("a.b", "aXb", false));  // '.' is not a wildcard
  EXPECT_TRUE(globMatch("a\\b", "a\\b", false));
}

TEST(GlobMatch, CaseSensitivity) {
  EXPECT_FALSE(globMatch("Game.EXE", "game.exe", false));
  EXPECT_TRUE(globMatch("Game.EXE", "game.exe", true));
  EXPECT_TRUE(globMatch("*.EXE", "Tachyon-Testbed.exe", true));
  EXPECT_TRUE(globMatch("W?RKER*", "worker.exe", true));
  EXPECT_FALSE(globMatch("W?RKER*", "worker.exe", false));
}

TEST(GlobMatch, PathologicalPatternIsFast) {
  // Exponential for a naive recursive matcher.
  const std::string text(5000, 'a');
  EXPECT_FALSE(globMatch("*a*a*a*a*a*a*a*a*a*a*b", text, false));
  EXPECT_TRUE(globMatch("*a*a*a*a*a*a*a*a*a*a*", text, false));
}

}  // namespace
}  // namespace tachyon::capture::etw
