#include "trace/string_table.h"

#include <gtest/gtest.h>

#include <string>

namespace tachyon::trace {
namespace {

TEST(StringTable, IndexZeroIsEmpty) {
  StringTable t;
  EXPECT_EQ(t.size(), 1u);
  EXPECT_EQ(t.get(StringIdx(0)), "");
  EXPECT_EQ(t.intern(""), StringIdx(0));
  EXPECT_EQ(t.get(StringIdx::invalid()), "");
}

TEST(StringTable, InternDedupsAndGrows) {
  StringTable t;
  std::vector<StringIdx> idx;
  for (int i = 0; i < 20000; ++i) idx.push_back(t.intern("string-" + std::to_string(i)));
  EXPECT_EQ(t.size(), 20001u);
  for (int i = 0; i < 20000; ++i) {
    EXPECT_EQ(t.intern("string-" + std::to_string(i)), idx[i]);
    EXPECT_EQ(t.get(idx[i]), "string-" + std::to_string(i));
  }
  EXPECT_FALSE(t.find("nope").valid());
}

TEST(StringTable, SerializeRoundTrip) {
  StringTable a;
  a.intern("héllo");
  a.intern("");
  a.intern("world");
  std::vector<std::byte> bytes;
  a.serialize(bytes, 0, a.size());
  StringTable b;
  // Index 0 is already present in b; skip the first (4-byte, empty) record.
  ASSERT_TRUE(b.deserializeAppend(std::span<const std::byte>(bytes).subspan(4), a.size() - 1).ok());
  ASSERT_EQ(b.size(), a.size());
  for (uint32_t i = 0; i < a.size(); ++i) EXPECT_EQ(a.get(StringIdx(i)), b.get(StringIdx(i)));
  EXPECT_EQ(b.find("world"), StringIdx(2));
}

TEST(StringTable, TruncatedPayloadIsRejected) {
  StringTable a;
  a.intern("abcdef");
  std::vector<std::byte> bytes;
  a.serialize(bytes, 1, 1);
  bytes.pop_back();
  StringTable b;
  EXPECT_FALSE(b.deserializeAppend(bytes, 1).ok());
}

}  // namespace
}  // namespace tachyon::trace
