#include "core/strong_id.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace tachyon {
namespace {

TEST(StrongId, DefaultIsInvalid) {
  FrameIdx f;
  EXPECT_FALSE(f.valid());
  EXPECT_EQ(f.value, kInvalidIdx);
  EXPECT_EQ(FrameIdx::invalid(), f);
}

TEST(StrongId, CompareAndHash) {
  ThreadIdx a(1), b(2);
  EXPECT_LT(a, b);
  EXPECT_NE(a, b);
  std::unordered_set<ThreadIdx> set{a, b, ThreadIdx(1)};
  EXPECT_EQ(set.size(), 2u);
  static_assert(!std::is_convertible_v<ThreadIdx, FrameIdx>, "distinct id types must not convert");
}

}  // namespace
}  // namespace tachyon
