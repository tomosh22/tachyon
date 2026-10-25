// Command line quoting and environment merging for launch mode (WP-13), platform independent.
#include "capture/windows_etw/launch.h"

#include <gtest/gtest.h>

namespace tachyon::capture::etw {
namespace {

TEST(QuoteArgument, PlainEmptyAndSpecial) {
  EXPECT_EQ(quoteArgument("abc"), "abc");
  EXPECT_EQ(quoteArgument(""), "\"\"");
  EXPECT_EQ(quoteArgument("a b"), "\"a b\"");
  EXPECT_EQ(quoteArgument("a\tb"), "\"a\tb\"");
  EXPECT_EQ(quoteArgument("say \"hi\""), "\"say \\\"hi\\\"\"");
  EXPECT_EQ(quoteArgument("C:\\dir\\file.txt"), "C:\\dir\\file.txt");  // backslashes alone are literal
  EXPECT_EQ(quoteArgument("C:\\my dir\\"), "\"C:\\my dir\\\\\"");      // trailing backslashes double
  EXPECT_EQ(quoteArgument("a\\\"b"), "\"a\\\\\\\"b\"");                // backslash before a quote
}

TEST(BuildCommandLine, QuotesExeAndArguments) {
  EXPECT_EQ(buildCommandLine("C:\\Program Files\\game.exe", {"--scenario", "spin", "--name", "a b"}),
            "\"C:\\Program Files\\game.exe\" --scenario spin --name \"a b\"");
  EXPECT_EQ(buildCommandLine("game.exe", {}), "game.exe");
}

TEST(SplitCommandLine, BasicTokens) {
  EXPECT_EQ(splitCommandLine("--scenario spin --duration 3"),
            (std::vector<std::string>{"--scenario", "spin", "--duration", "3"}));
  EXPECT_EQ(splitCommandLine("  a   b\tc  "), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_TRUE(splitCommandLine("").empty());
  EXPECT_TRUE(splitCommandLine("   ").empty());
}

TEST(SplitCommandLine, QuotesAndBackslashes) {
  EXPECT_EQ(splitCommandLine("a \"b c\" d"), (std::vector<std::string>{"a", "b c", "d"}));
  EXPECT_EQ(splitCommandLine("\"\" x"), (std::vector<std::string>{"", "x"}));
  EXPECT_EQ(splitCommandLine("say \\\"hi\\\""), (std::vector<std::string>{"say", "\"hi\""}));
  EXPECT_EQ(splitCommandLine("C:\\dir\\file.txt"), (std::vector<std::string>{"C:\\dir\\file.txt"}));
  EXPECT_EQ(splitCommandLine("\"C:\\my dir\\\\\" y"), (std::vector<std::string>{"C:\\my dir\\", "y"}));
  EXPECT_EQ(splitCommandLine("pre\"mid dle\"post"), (std::vector<std::string>{"premid dlepost"}));
}

TEST(SplitCommandLine, RoundTripsQuoteArgument) {
  const std::vector<std::string> args = {"plain",     "with space",  "",         "q\"uote",
                                         "a\\b",      "a b\\",       "a\\\"b c", "\\\\server\\share x",
                                         "tab\there", "--flag=\"v\""};
  std::string line;
  for (const std::string& a : args) {
    if (!line.empty()) line += ' ';
    line += quoteArgument(a);
  }
  EXPECT_EQ(splitCommandLine(line), args);
}

TEST(MergeEnvironment, OverridesWinCaseInsensitivelyAndResultIsSorted) {
  const std::vector<EnvVar> parent = {
      {"Path", "C:\\bin"}, {"TEMP", "C:\\tmp"}, {"=C:", "C:\\work"}, {"A", "1"}};
  const auto merged =
      mergeEnvironment(parent, {{"path", "D:\\other"}, {"TACHYON_AGENT_PIPE", "pipe0"}, {"B", "2"}});
  const std::vector<EnvVar> expected = {
      {"=C:", "C:\\work"}, {"A", "1"}, {"B", "2"}, {"path", "D:\\other"}, {"TACHYON_AGENT_PIPE", "pipe0"},
      {"TEMP", "C:\\tmp"}};
  EXPECT_EQ(merged, expected);
}

TEST(MergeEnvironment, CaseSensitiveKeepsBothSpellings) {
  const auto merged = mergeEnvironment({{"Path", "1"}}, {{"path", "2"}}, false);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].first, "Path");
  EXPECT_EQ(merged[1].first, "path");
}

TEST(MergeEnvironment, DropsEmptyNamesAndKeepsEmptyValues) {
  const auto merged = mergeEnvironment({{"", "x"}, {"K", ""}}, {{"", "y"}});
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0], (EnvVar{"K", ""}));
}

TEST(MergeEnvironment, AgentPipeIsAlwaysLastWord) {
  // The injected variable replaces a user-supplied one of the same name: the caller merges it last.
  std::map<std::string, std::string> env = {{"TACHYON_AGENT_PIPE", "user"}};
  env[std::string(kAgentPipeEnvVar)] = "agent";
  const auto merged = mergeEnvironment({}, env);
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0].second, "agent");
}

}  // namespace
}  // namespace tachyon::capture::etw
