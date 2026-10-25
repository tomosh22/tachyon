// DiaSymbolTable (WP-22): interval lookup over nested / overlapping symbols and bounded public extents.
#include "symbols/dia/dia_symbol_table.h"

#include <gtest/gtest.h>

namespace tachyon::symbols {
namespace {

std::string nameAt(const DiaSymbolTable& t, uint64_t rva) {
  const auto f = t.find(rva);
  return f ? f->name : "<none>";
}

TEST(DiaSymbolTable, NestedPrivateSymbolsResolveToTheInnermost) {
  DiaSymbolTable t;
  t.build({{0x1000, 0x100, "outer"}, {0x1040, 0x20, "inner"}}, {}, {});
  EXPECT_EQ(nameAt(t, 0x1010), "outer");
  EXPECT_EQ(nameAt(t, 0x1050), "inner");
  EXPECT_EQ(nameAt(t, 0x1070), "outer");  // after the nested one, back to the outer
  EXPECT_EQ(nameAt(t, 0x10FF), "outer");
  EXPECT_EQ(nameAt(t, 0x1100), "<none>");
  EXPECT_EQ(nameAt(t, 0xFFF), "<none>");
}

TEST(DiaSymbolTable, OverlappingSymbolsAndLongOuterAfterManySmallOnes) {
  DiaSymbolTable t;
  t.build({{0x2000, 0x100, "A"},
           {0x2080, 0x100, "B"},
           {0x3000, 0x400, "long"},
           {0x3010, 0x10, "s1"},
           {0x3100, 0x10, "s2"},
           {0x3200, 0x10, "s3"}},
          {}, {});
  EXPECT_EQ(nameAt(t, 0x2050), "A");
  EXPECT_EQ(nameAt(t, 0x2090), "B");  // both contain it; the greatest start wins
  EXPECT_EQ(nameAt(t, 0x2150), "B");
  EXPECT_EQ(nameAt(t, 0x2180), "<none>");
  EXPECT_EQ(nameAt(t, 0x3015), "s1");
  EXPECT_EQ(nameAt(t, 0x3300), "long");  // reached past three smaller symbols
  EXPECT_EQ(nameAt(t, 0x33FF), "long");
}

TEST(DiaSymbolTable, PublicExtentIsBoundedByTheNextCodeSymbol) {
  DiaSymbolTable t;
  // Next code symbol of pub1 is the private function; of pub2 the third public.
  t.build({{0x1100, 0x40, "priv"}}, {{0x1000, 0, "pub1"}, {0x2000, 0, "pub2"}, {0x2080, 0, "pub3"}}, {});
  auto p1 = t.find(0x1000);
  ASSERT_TRUE(p1);
  EXPECT_TRUE(p1->isPublicSymbol);
  EXPECT_EQ(p1->size, 0x100u);
  EXPECT_EQ(nameAt(t, 0x10FF), "pub1");
  EXPECT_EQ(nameAt(t, 0x1100), "priv");    // privates win
  EXPECT_EQ(nameAt(t, 0x1140), "<none>");  // gap after priv: pub1 does not extend over it
  EXPECT_EQ(nameAt(t, 0x207F), "pub2");
  EXPECT_EQ(nameAt(t, 0x2080), "pub3");
}

TEST(DiaSymbolTable, PublicExtentIsClippedToTheCodeContribution) {
  DiaSymbolTable t;
  t.build({}, {{0x1000, 0, "last_in_obj"}, {0x5000, 0, "far"}}, {{0x1000, 0x80}, {0x5000, 0x40}});
  auto p = t.find(0x1000);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->size, 0x80u);  // clipped at the contribution end, not at the far next symbol
  EXPECT_EQ(nameAt(t, 0x107F), "last_in_obj");
  EXPECT_EQ(nameAt(t, 0x1080), "<none>");
  EXPECT_EQ(nameAt(t, 0x5030), "far");
  EXPECT_EQ(nameAt(t, 0x5040), "<none>");
}

TEST(DiaSymbolTable, UnboundedPublicResolvesWithSizeZero) {
  DiaSymbolTable t;
  t.build({}, {{0x1000, 0, "tail"}}, {});  // no next code symbol, no contribution
  auto p = t.find(0x9000);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->name, "tail");
  EXPECT_EQ(p->size, 0u);
  EXPECT_FALSE(t.find(0xFFF));
}

TEST(DiaSymbolTable, AliasedPublicsKeepTheFirstAndZeroSizedPrivatesAreDropped) {
  DiaSymbolTable t;
  t.build({{0x100, 0, "empty"}}, {{0x1000, 0, "first"}, {0x1000, 0, "alias"}, {0x1010, 0, "next"}}, {});
  EXPECT_EQ(t.privateCount(), 0u);
  EXPECT_EQ(t.publicCount(), 2u);
  EXPECT_EQ(nameAt(t, 0x1008), "first");
  auto p = t.find(0x1000);
  ASSERT_TRUE(p);
  EXPECT_EQ(p->size, 0x10u);
}

}  // namespace
}  // namespace tachyon::symbols
