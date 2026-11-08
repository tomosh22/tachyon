// WP-69: the symbol-service setup shared by tachyon-report and `tachyon-cli export`.
#include "report/symbol_setup.h"

#include "report_test_util.h"

#include <gtest/gtest.h>

namespace tachyon::report {
namespace {

namespace fs = std::filesystem;
using Kind = SymbolSetupProblem::Kind;

std::vector<SymbolSetupProblem> setup(const SymbolSetupOptions& o) {
  symbols::SymbolService service(o.servers, o.cacheDir);
  return setupSymbolService(service, o);
}

TEST(SymbolSetup, NothingToDoHasNoProblems) {
  EXPECT_TRUE(setup({}).empty());
}

TEST(SymbolSetup, ValidOverrideFileIsAccepted) {
  SymbolSetupOptions o;
  o.symbolPaths = {testutil::generated("frames.symbols.json")};
  EXPECT_TRUE(setup(o).empty());
}

TEST(SymbolSetup, MissingPathAndBrokenOverrideAreReportedInOrder) {
  const fs::path dir = testutil::tempDir("symbol_setup_problems");
  testutil::writeText(dir / "broken.json", "{ not json");
  SymbolSetupOptions o;
  o.symbolPaths = {dir / "nope", dir / "broken.json", testutil::generated("frames.symbols.json")};
  const auto problems = setup(o);
  ASSERT_EQ(problems.size(), 2u);
  EXPECT_EQ(problems[0].kind, Kind::MissingPath);
  EXPECT_EQ(problems[0].path, dir / "nope");
  EXPECT_EQ(problems[1].kind, Kind::BadOverride);
  EXPECT_EQ(problems[1].path, dir / "broken.json");
  EXPECT_FALSE(problems[1].detail.empty());
}

TEST(SymbolSetup, DirectoriesAreSearchPathsAndOnlyContributeSidecarsWhenAsked) {
  const fs::path dir = testutil::tempDir("symbol_setup_dir");
  testutil::writeText(dir / "a.symbols.json", "{ not json");
  testutil::writeText(dir / "ignored.txt", "x");
  SymbolSetupOptions o;
  o.symbolPaths = {dir};
  EXPECT_TRUE(setup(o).empty());  // report behaviour: a directory is only a search path
  o.directorySidecars = true;     // export behaviour: its *.symbols.json files are overrides too
  const auto problems = setup(o);
  ASSERT_EQ(problems.size(), 1u);
  EXPECT_EQ(problems[0].kind, Kind::BadOverride);
  EXPECT_EQ(problems[0].path.filename(), "a.symbols.json");
}

TEST(SymbolSetup, NonJsonFilesAreSearchPathsNotOverrides) {
  const fs::path dir = testutil::tempDir("symbol_setup_file");
  testutil::writeText(dir / "game.pdb", "not a pdb");
  SymbolSetupOptions o;
  o.symbolPaths = {dir / "game.pdb"};
  EXPECT_TRUE(setup(o).empty());
}

TEST(SymbolSetup, SidecarFileIsAppliedOnlyWhenItExists) {
  const fs::path dir = testutil::tempDir("symbol_setup_sidecar");
  SymbolSetupOptions o;
  o.sidecarFile = dir / "t.symbols.json";
  EXPECT_TRUE(setup(o).empty());  // absent: silently ignored
  testutil::writeText(o.sidecarFile, "{ not json");
  const auto problems = setup(o);
  ASSERT_EQ(problems.size(), 1u);
  EXPECT_EQ(problems[0].kind, Kind::BadOverride);
  EXPECT_EQ(problems[0].path, o.sidecarFile);
}

}  // namespace
}  // namespace tachyon::report
