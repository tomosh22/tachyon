#include "core/hash.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace tachyon::core {
namespace {

TEST(Hash, DeterministicAndSeeded) {
  EXPECT_EQ(hashString("tachyon"), hashString("tachyon"));
  EXPECT_NE(hashString("tachyon"), hashString("tachyoN"));
  EXPECT_NE(hashString("tachyon", 1), hashString("tachyon", 2));
  const uint64_t v = 42;
  EXPECT_EQ(hashPod(v), hashBytes(&v, sizeof v));
}

TEST(Hash, PairHashSpreads) {
  std::unordered_set<uint64_t> seen;
  for (uint32_t p = 0; p < 100; ++p)
    for (uint32_t f = 0; f < 100; ++f) seen.insert(hashU32Pair(p, f));
  EXPECT_EQ(seen.size(), 10'000u);
  EXPECT_NE(hashCombine(1, 2), hashCombine(2, 1));
}

}  // namespace
}  // namespace tachyon::core
