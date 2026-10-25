// Reproducibility: every committed scenario must regenerate byte-for-byte (same SHA-256) on every platform.
// The manifest (tests/fixtures/tracegen/manifest.json) was produced on Linux; Windows CI runs this same test.
#include "manifest.h"
#include "test_util.h"

#include <gtest/gtest.h>

namespace tachyon::tracegen {
namespace {

using nlohmann::json;
using testing_util::TempDir;

json loadManifest() {
  json m = testing_util::readJson(testing_util::scenarioDir() / "manifest.json");
  EXPECT_TRUE(m.is_object()) << "tests/fixtures/tracegen/manifest.json is missing or invalid";
  return m;
}

void expectEntryMatches(const std::string& name, const json& expectedEntry,
                        const std::filesystem::path& dir) {
  ASSERT_TRUE(expectedEntry.is_object()) << name;
  ASSERT_EQ(expectedEntry.size(), 3u) << name;
  for (const auto& [file, hash] : expectedEntry.items())
    EXPECT_EQ(testing_util::sha(dir / file), hash.get<std::string>()) << file;
}

TEST(FixtureManifest, ListsEveryScenarioExceptLarge) {
  const json m = loadManifest();
  EXPECT_EQ(m["schema"], "tachyon-fixture-manifest-1");
  std::vector<std::string> names;
  for (const auto& item : m["scenarios"].items()) names.push_back(item.key());
  EXPECT_EQ(names, (std::vector<std::string>{"fibers", "frames", "medium", "partial", "recursion", "scopes",
                                             "tiny"}));
}

// The fixtures built by the tachyon_fixtures target (the tracegen executable) match the manifest.
TEST(FixtureManifest, BuiltFixturesMatchManifest) {
  const json m = loadManifest();
  for (const auto& [name, entry] : m["scenarios"].items())
    expectEntryMatches(name, entry, TACHYON_GENERATED_FIXTURE_DIR);
}

// Regenerating in-process (fresh process state, different directory) reproduces the same bytes.
class RegenerateTest : public ::testing::TestWithParam<const char*> {};

TEST_P(RegenerateTest, MatchesManifest) {
  const std::string name = GetParam();
  const json m = loadManifest();
  ASSERT_TRUE(m["scenarios"].contains(name));
  auto scenario = loadScenario((testing_util::scenarioDir() / (name + ".json")).string());
  ASSERT_TRUE(scenario.ok()) << scenario.error().toString();
  TempDir dir;
  auto report = generateFixture(*scenario, dir.path(), GenerateOptions{});
  ASSERT_TRUE(report.ok()) << report.error().toString();
  auto entry = manifestEntry(*report);
  ASSERT_TRUE(entry.ok());
  EXPECT_EQ(*entry, m["scenarios"][name]);
  expectEntryMatches(name, m["scenarios"][name], dir.path());
}

INSTANTIATE_TEST_SUITE_P(Scenarios, RegenerateTest,
                         ::testing::Values("tiny", "scopes", "fibers", "frames", "recursion", "partial",
                                           "medium"));

}  // namespace
}  // namespace tachyon::tracegen
