// The .symbols.json sidecar must carry everything JsonSymbolProvider (WP-20) needs for resolve(),
// linesForFunction() and functionBounds().
#include "reference.h"
#include "test_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

namespace tachyon::tracegen {
namespace {

using nlohmann::json;

class SymbolsTest : public ::testing::TestWithParam<const char*> {};

TEST_P(SymbolsTest, SidecarIsSelfConsistent) {
  const std::string name = GetParam();
  const json sym =
      testing_util::readJson(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / (name + ".symbols.json"));
  ASSERT_TRUE(sym.is_object());
  EXPECT_EQ(sym["schema"], "tachyon-symbols-1");
  EXPECT_EQ(sym["scenario"], name);

  auto reader =
      trace::TraceReader::open(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / (name + ".tachyon"));
  ASSERT_TRUE(reader.ok());
  auto mods = reader->readModuleInfos();
  ASSERT_TRUE(mods.ok());
  ASSERT_EQ(sym["modules"].size(), mods->size());

  size_t moduleIdx = 0;
  for (const json& m : sym["modules"]) {
    EXPECT_EQ(m["module_idx"].get<size_t>(), moduleIdx);
    const trace::ModuleInfo& rec = (*mods)[moduleIdx++];
    EXPECT_EQ(m["base"].get<uint64_t>(), rec.base);
    EXPECT_EQ(m["size"].get<uint64_t>(), rec.size);
    EXPECT_EQ(m["age"].get<uint32_t>(), rec.age);
    EXPECT_EQ(m["id"].get<std::string>().size(), static_cast<size_t>(rec.id_len) * 2);
    uint64_t prevEnd = 0;
    for (const json& f : m["functions"]) {
      const uint64_t rva = f["start_rva"].get<uint64_t>(), size = f["size"].get<uint64_t>();
      EXPECT_GT(size, 0u);
      EXPECT_GE(rva, prevEnd) << "functions must be sorted and non-overlapping";
      prevEnd = rva + size;
      EXPECT_LE(prevEnd, rec.size);
      EXPECT_FALSE(f["name"].get<std::string>().empty());
      EXPECT_FALSE(f["file"].get<std::string>().empty());
      // Line ranges tile the function exactly.
      uint64_t cursor = rva;
      ASSERT_FALSE(f["lines"].empty());
      for (const json& l : f["lines"]) {
        EXPECT_EQ(l["rva"].get<uint64_t>(), cursor);
        EXPECT_GT(l["size"].get<uint64_t>(), 0u);
        cursor += l["size"].get<uint64_t>();
      }
      EXPECT_EQ(cursor, rva + size);
      for (const json& in : f["inlines"]) {
        EXPECT_GE(in["rva"].get<uint64_t>(), rva);
        EXPECT_LE(in["rva"].get<uint64_t>() + in["size"].get<uint64_t>(), rva + size);
        ASSERT_FALSE(in["chain"].empty());
        for (const json& c : in["chain"]) {
          EXPECT_FALSE(c["function"].get<std::string>().empty());
          EXPECT_TRUE(c.contains("file") && c.contains("line"));
        }
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Fixtures, SymbolsTest, ::testing::Values("tiny", "medium", "scopes"));

TEST(SymbolsFixture, TinyHasInlineChainsInnermostFirst) {
  const json sym =
      testing_util::readJson(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.symbols.json");
  const json& update = sym["modules"][0]["functions"][1];
  EXPECT_EQ(update["name"], "Engine::update");
  ASSERT_EQ(update["inlines"].size(), 1u);
  EXPECT_EQ(update["inlines"][0]["chain"][0]["function"], "Vec3::length");
  EXPECT_EQ(update["inlines"][0]["chain"][1]["function"], "Entity::distance");
  EXPECT_EQ(update["inlines"][0]["rva"].get<uint64_t>(), 0x1200u + 144u);
}

}  // namespace
}  // namespace tachyon::tracegen
