// WP-47: the fuzzy matcher behind the Function Statistics search box. Tier order (exact > prefix >
// word-start > substring > subsequence), intra-tier scoring, case insensitivity, deterministic ties, top-N.
#include "viewer/stats/fuzzy_matcher.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace tachyon::viewer::stats;

namespace {

FuzzyIndex indexOf(const std::vector<std::string>& names) {
  FuzzyIndex idx;
  for (const std::string& n : names) idx.add(n);
  return idx;
}

std::vector<std::string> ranked(const std::vector<std::string>& names, const std::string& query,
                                size_t limit = 200) {
  const FuzzyIndex idx = indexOf(names);
  std::vector<std::string> out;
  for (const FuzzyResult& r : idx.search(query, limit)) out.emplace_back(idx.text(r.index));
  return out;
}

}  // namespace

TEST(FuzzyMatcher, TiersAreExactPrefixWordStartSubstringSubsequence) {
  EXPECT_EQ(fuzzyMatch("update", "update").tier, MatchTier::Exact);
  EXPECT_EQ(fuzzyMatch("update", "updateAll").tier, MatchTier::Prefix);
  EXPECT_EQ(fuzzyMatch("update", "Engine::update").tier, MatchTier::WordStart);
  EXPECT_EQ(fuzzyMatch("update", "game.exe!Engine::updateAll").tier, MatchTier::WordStart);
  EXPECT_EQ(fuzzyMatch("update", "Engine::reupdate").tier, MatchTier::Substring);
  EXPECT_EQ(fuzzyMatch("update", "uxpxdxaxtxe").tier, MatchTier::Subsequence);
  EXPECT_EQ(fuzzyMatch("update", "render").tier, MatchTier::None);
  EXPECT_FALSE(fuzzyMatch("update", "render").matched());
}

TEST(FuzzyMatcher, CamelCaseHumpsAndSeparatorsAreWordStarts) {
  EXPECT_EQ(fuzzyMatch("update", "GameUpdate").tier, MatchTier::WordStart);
  EXPECT_EQ(fuzzyMatch("update", "game_update").tier, MatchTier::WordStart);
  EXPECT_EQ(fuzzyMatch("update", "gameupdate").tier, MatchTier::Substring);
  EXPECT_EQ(fuzzyMatch("exe!eng", "game.exe!Engine").tier, MatchTier::WordStart);
}

TEST(FuzzyMatcher, TierOrderDominatesTheScore) {
  const auto m = [](const char* text) { return fuzzyMatch("update", text); };
  EXPECT_GT(m("update").score, m("updateAll").score);
  EXPECT_GT(m("updateAll").score, m("Engine::update").score);
  EXPECT_GT(m("Engine::update").score, m("Engine::reupdate").score);
  EXPECT_GT(m("Engine::reupdate").score, m("uxpxdxaxtxe").score);
  EXPECT_GT(m("uxpxdxaxtxe").score, 0);
}

TEST(FuzzyMatcher, RankingOfAMixedListIsExactPrefixWordStartSubstringSubsequence) {
  const std::vector<std::string> names = {"uxpxdxaxtxe", "Engine::reupdate", "Engine::update", "updateAll",
                                          "update",      "render",           "Sys::Update"};
  const std::vector<std::string> expected = {"update",         "updateAll",        "Sys::Update",
                                             "Engine::update", "Engine::reupdate", "uxpxdxaxtxe"};
  EXPECT_EQ(ranked(names, "update"), expected);
}

TEST(FuzzyMatcher, IsCaseInsensitiveAndIgnoresWhitespaceInTheQuery) {
  EXPECT_EQ(fuzzyMatch("UPDATE", "update").tier, MatchTier::Exact);
  EXPECT_EQ(fuzzyMatch("update", "UPDATE").tier, MatchTier::Exact);
  EXPECT_EQ(fuzzyMatch("eng upd", "Engine::update").tier, MatchTier::Subsequence);
  EXPECT_TRUE(fuzzyMatch(" e n g ", "Engine").matched());
  EXPECT_EQ(fuzzyMatch("  ", "anything").tier, MatchTier::Prefix);  // empty query matches everything
}

TEST(FuzzyMatcher, UpperCaseInTheQueryEarnsACaseBonus) {
  EXPECT_GT(fuzzyMatch("Update", "Engine::Update").score, fuzzyMatch("Update", "Engine::update").score);
  EXPECT_EQ(fuzzyMatch("Update", "Engine::Update").tier, fuzzyMatch("Update", "Engine::update").tier);
}

TEST(FuzzyMatcher, ShorterNamesAndEarlierMatchesRankHigherInsideATier) {
  EXPECT_GT(fuzzyMatch("upd", "update").score, fuzzyMatch("upd", "updateEverythingNow").score);
  EXPECT_GT(fuzzyMatch("upd", "A::update").score, fuzzyMatch("upd", "AAAAAAAA::update").score);
}

TEST(FuzzyMatcher, SubsequenceScoringPrefersContiguousRunsAndWordStarts) {
  // "gu" hits the two word starts of GameUpdate; in "bigunit" it is contiguous but mid-word.
  const FuzzyMatch humps = fuzzyMatch("gu", "GameUpdate");
  const FuzzyMatch scattered = fuzzyMatch("gu", "xxgxxxxxxxu");
  EXPECT_EQ(humps.tier, MatchTier::Subsequence);
  EXPECT_EQ(scattered.tier, MatchTier::Subsequence);
  EXPECT_GT(humps.score, scattered.score);
  // Longer contiguous runs beat broken ones.
  EXPECT_GT(fuzzyMatch("rndr", "rendr_x").score, fuzzyMatch("rndr", "rexnxdxr").score);
  // A tighter window wins over the first greedy match.
  EXPECT_GT(fuzzyMatch("abc", "a....a.b.c").score, fuzzyMatch("abc", "a...........b....c").score);
}

TEST(FuzzyMatcher, TiesAreBrokenByLengthThenIndex) {
  // Same score and length: the lower index first; same score, different length: the shorter first.
  const std::vector<std::string> names = {"ab", "ab", "ab"};
  const FuzzyIndex idx = indexOf(names);
  const auto r = idx.search("ab", 10);
  ASSERT_EQ(r.size(), 3u);
  EXPECT_EQ(r[0].index, 0u);
  EXPECT_EQ(r[1].index, 1u);
  EXPECT_EQ(r[2].index, 2u);
  // Equal-tier scores are never lost to insertion order: repeat with the list reversed.
  const std::vector<std::string> names2 = {"xa_b", "xa_b_long", "a_b"};
  const auto order = ranked(names2, "a_b");
  EXPECT_EQ(order, (std::vector<std::string>{"a_b", "xa_b", "xa_b_long"}));
}

TEST(FuzzyMatcher, SearchReturnsTheTopNInOrderAndIsStable) {
  std::vector<std::string> names;
  for (int i = 0; i < 1000; ++i) names.push_back("Module::Function" + std::to_string(i));
  names.push_back("Function");
  names.push_back("function");
  const FuzzyIndex idx = indexOf(names);
  const auto top = idx.search("function", 5);
  ASSERT_EQ(top.size(), 5u);
  // The two exact matches first (lower index first), then word-start matches by shortness.
  EXPECT_EQ(idx.text(top[0].index), "Function");
  EXPECT_EQ(idx.text(top[1].index), "function");
  for (size_t i = 1; i < top.size(); ++i) EXPECT_GE(top[i - 1].match.score, top[i].match.score);
  EXPECT_EQ(idx.search("function", 5).size(), 5u);
  // Deterministic: identical on a second run.
  const auto again = idx.search("function", 5);
  for (size_t i = 0; i < top.size(); ++i) EXPECT_EQ(top[i].index, again[i].index);
  // The limit cuts but never reorders: the first 3 of a top-5 equal a top-3.
  const auto top3 = idx.search("function", 3);
  for (size_t i = 0; i < 3; ++i) EXPECT_EQ(top3[i].index, top[i].index);
}

TEST(FuzzyMatcher, EmptyQueryListsTheFirstEntriesInIndexOrder) {
  const FuzzyIndex idx = indexOf({"b", "a", "c"});
  const auto r = idx.search("", 2);
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0].index, 0u);
  EXPECT_EQ(r[1].index, 1u);
  EXPECT_TRUE(idx.search("zzz", 5).empty());
  EXPECT_TRUE(idx.search("a", 0).empty());
  EXPECT_TRUE(FuzzyIndex().search("a", 5).empty());
}

TEST(FuzzyMatcher, QueryLongerThanTheNameNeverMatches) {
  EXPECT_FALSE(fuzzyMatch("updates", "update").matched());
  const FuzzyIndex idx = indexOf({"ab"});
  EXPECT_TRUE(idx.search("abc", 5).empty());
}

TEST(FuzzyMatcher, SearchEqualsBruteForceMatchOverTheIndex) {
  std::vector<std::string> names = {"game.exe!Engine::update",
                                    "game.exe!Engine::physics",
                                    "game.exe!main",
                                    "ntdll.dll!RtlUserThreadStart",
                                    "game.exe!Job::wait",
                                    "Frame",
                                    "JobRun",
                                    "JobWait",
                                    "engine_update_all",
                                    "UpdateEngine"};
  const FuzzyIndex idx = indexOf(names);
  for (const char* q : {"eng", "upd", "jw", "jobw", "ntdll", "x", "Frame", "ee", "main", "r"}) {
    std::vector<std::pair<int32_t, uint32_t>> expected;
    for (uint32_t i = 0; i < names.size(); ++i) {
      const FuzzyMatch m = fuzzyMatch(q, names[i]);
      if (m.matched()) expected.emplace_back(m.score, i);
    }
    std::stable_sort(expected.begin(), expected.end(), [&](const auto& a, const auto& b) {
      if (a.first != b.first) return a.first > b.first;
      if (names[a.second].size() != names[b.second].size())
        return names[a.second].size() < names[b.second].size();
      return a.second < b.second;
    });
    const auto got = idx.search(q, 200);
    ASSERT_EQ(got.size(), expected.size()) << q;
    for (size_t i = 0; i < got.size(); ++i) {
      EXPECT_EQ(got[i].index, expected[i].second) << q << " #" << i;
      EXPECT_EQ(got[i].match.score, expected[i].first) << q << " #" << i;
    }
  }
}
