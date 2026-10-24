#include "core/result.h"

#include <gtest/gtest.h>

#include <memory>

namespace tachyon {
namespace {

Result<int> parsePositive(int v) {
  if (v <= 0) return makeError(ErrorCode::InvalidArgument, "not positive");
  return v;
}

Result<int> doubled(int v) {
  TACHYON_TRY_ASSIGN(int x, parsePositive(v));
  return x * 2;
}

Result<void> check(int v) {
  TACHYON_TRY(parsePositive(v));
  return ok();
}

TEST(Result, ValueAndError) {
  auto good = parsePositive(3);
  ASSERT_TRUE(good.ok());
  EXPECT_EQ(*good, 3);
  auto bad = parsePositive(-1);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(bad.error().toString(), "InvalidArgument: not positive");
  EXPECT_EQ(bad.valueOr(7), 7);
}

TEST(Result, TryMacrosPropagate) {
  EXPECT_EQ(*doubled(4), 8);
  EXPECT_EQ(doubled(0).error().code, ErrorCode::InvalidArgument);
  EXPECT_TRUE(check(1).ok());
  EXPECT_FALSE(check(-5).ok());
}

TEST(Result, MoveOnlyValue) {
  Result<std::unique_ptr<int>> r = std::make_unique<int>(5);
  ASSERT_TRUE(r.ok());
  std::unique_ptr<int> p = std::move(r).value();
  EXPECT_EQ(*p, 5);
}

}  // namespace
}  // namespace tachyon
