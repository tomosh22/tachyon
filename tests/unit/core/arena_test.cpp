#include "core/arena.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace tachyon::core {
namespace {

TEST(Arena, AlignmentAndGrowth) {
  Arena arena(128);
  for (int i = 0; i < 100; ++i) {
    void* p = arena.allocate(24, 16);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u);
  }
  EXPECT_EQ(arena.bytesAllocated(), 2400u);
  void* big = arena.allocate(10'000, 64);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(big) % 64, 0u);
  arena.reset();
  EXPECT_EQ(arena.bytesAllocated(), 0u);
}

TEST(Arena, ArraysAndStrings) {
  Arena arena;
  auto arr = arena.allocateArray<uint32_t>(10);
  ASSERT_EQ(arr.size(), 10u);
  for (uint32_t v : arr) EXPECT_EQ(v, 0u);
  auto s = arena.copyString("hello");
  EXPECT_EQ(s, "hello");
  struct Pod {
    int a;
    double b;
  };
  Pod* pod = arena.make<Pod>(Pod{1, 2.0});
  EXPECT_EQ(pod->a, 1);
}

}  // namespace
}  // namespace tachyon::core
