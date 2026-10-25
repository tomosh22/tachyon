#include "inst/sidecar_paths.h"

#include "../control/test_util.h"

#include <gtest/gtest.h>

#include <fstream>

namespace tachyon::inst {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;

void touch(const fs::path& p) {
  std::ofstream(p).put('x');
}

TEST(SidecarPaths, NumberedNameInsertsBeforeTheLastExtension) {
  EXPECT_EQ(numberedSidecarPath("dir/app-1234.tachyon-inst", 1), fs::path("dir/app-1234.1.tachyon-inst"));
  EXPECT_EQ(numberedSidecarPath("app-1234.tachyon-inst", 12), fs::path("app-1234.12.tachyon-inst"));
  EXPECT_EQ(numberedSidecarPath("/x/y/trace", 2), fs::path("/x/y/trace.2"));
  EXPECT_EQ(numberedSidecarPath("a.b.inst", 3), fs::path("a.b.3.inst"));
}

TEST(SidecarPaths, SiblingsComeInNumericOrder) {
  TempDir dir;
  const fs::path base = dir.path() / "app-1234.tachyon-inst";
  touch(base);
  for (uint32_t n : {10u, 2u, 1u, 3u}) touch(numberedSidecarPath(base, n));
  const auto all = expandSidecarSiblings(base);
  ASSERT_EQ(all.size(), 5u);
  EXPECT_EQ(all[0], base);
  EXPECT_EQ(all[1].filename().string(), "app-1234.1.tachyon-inst");
  EXPECT_EQ(all[2].filename().string(), "app-1234.2.tachyon-inst");
  EXPECT_EQ(all[3].filename().string(), "app-1234.3.tachyon-inst");
  EXPECT_EQ(all[4].filename().string(), "app-1234.10.tachyon-inst");
}

TEST(SidecarPaths, UnrelatedFilesAreIgnored) {
  TempDir dir;
  const fs::path base = dir.path() / "app-1234.tachyon-inst";
  touch(base);
  touch(dir.path() / "app-1234.1.tachyon-inst");
  touch(dir.path() / "app-12345.1.tachyon-inst");                // other pid
  touch(dir.path() / "app-1234.x.tachyon-inst");                 // not a number
  touch(dir.path() / "app-1234.0.tachyon-inst");                 // numbering starts at 1
  touch(dir.path() / "app-1234.1.2.tachyon-inst");               // two numbers
  touch(dir.path() / "app-1234.1.tachyon-trace");                // other extension
  fs::create_directory(dir.path() / "app-1234.5.tachyon-inst");  // a directory
  const auto all = expandSidecarSiblings(base);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[1].filename().string(), "app-1234.1.tachyon-inst");
}

TEST(SidecarPaths, GapsAreAllowedAndMissingBaseIsStillReturned) {
  TempDir dir;
  const fs::path base = dir.path() / "app.tachyon-inst";
  touch(numberedSidecarPath(base, 4));
  const auto all = expandSidecarSiblings(base);
  ASSERT_EQ(all.size(), 2u);
  EXPECT_EQ(all[0], base);
  EXPECT_EQ(all[1].filename().string(), "app.4.tachyon-inst");
}

TEST(SidecarPaths, FileWithoutExtension) {
  TempDir dir;
  const fs::path base = dir.path() / "stream";
  touch(base);
  touch(dir.path() / "stream.1");
  touch(dir.path() / "stream.2");
  EXPECT_EQ(expandSidecarSiblings(base).size(), 3u);
}

}  // namespace
}  // namespace tachyon::inst
