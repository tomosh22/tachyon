#include "trace/callstack_interner.h"

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <vector>

namespace tachyon::trace {
namespace {

TEST(CallstackInterner, RootAndEmptyStack) {
  CallstackInterner in;
  ASSERT_EQ(in.size(), 1u);
  EXPECT_FALSE(in.node(CallstackIdx(0)).parent.valid());
  EXPECT_FALSE(in.node(CallstackIdx(0)).frame.valid());
  EXPECT_EQ(in.internStack({}), CallstackIdx(0));
}

TEST(CallstackInterner, DedupAndSharedPrefix) {
  CallstackInterner in;
  const FrameIdx main(1), a(2), b(3);
  const FrameIdx s1[] = {a, main};  // leaf-first
  const FrameIdx s2[] = {b, main};
  const CallstackIdx i1 = in.internStack(s1);
  const CallstackIdx i2 = in.internStack(s2);
  EXPECT_NE(i1, i2);
  EXPECT_EQ(in.internStack(s1), i1);
  EXPECT_EQ(in.node(i1).parent, in.node(i2).parent);  // shared "main" node
  EXPECT_EQ(in.node(i1).frame, a);
  EXPECT_EQ(in.size(), 4u);
}

TEST(CallstackInterner, LoadFactorStaysBounded) {
  CallstackInterner in;
  for (uint32_t i = 0; i < 100000; ++i) {
    in.intern(CallstackIdx(0), FrameIdx(i));
    ASSERT_LE((in.size() - 1) * 10, in.hashTableSlots() * 7);
  }
}

// 10 M random stacks; the first 100 k are checked against a std::map oracle.
TEST(CallstackInterner, RandomStacksMatchOracle) {
  constexpr size_t kStacks = 10'000'000;
  constexpr size_t kOracleStacks = 100'000;
  CallstackInterner in;
  std::mt19937_64 rng(12345);
  std::map<std::vector<uint32_t>, CallstackIdx> oracle;
  std::vector<std::vector<uint32_t>> oracleStacks;
  std::vector<uint32_t> stack;
  std::vector<FrameIdx> frames;
  for (size_t n = 0; n < kStacks; ++n) {
    const size_t depth = 1 + rng() % 6;
    stack.clear();
    frames.clear();
    for (size_t d = 0; d < depth; ++d) {
      const uint32_t f = static_cast<uint32_t>(rng() % 12);  // small alphabet: many duplicates
      stack.push_back(f);
      frames.push_back(FrameIdx(f));
    }
    const CallstackIdx idx = in.internStack(frames);
    if (n < kOracleStacks) {
      auto [it, inserted] = oracle.try_emplace(stack, idx);
      if (!inserted)
        ASSERT_EQ(it->second, idx) << "duplicate stack got a different index";
      else
        oracleStacks.push_back(stack);
    } else if (n % 1000 == 0) {
      // Rebuild the stack from the DAG and verify it.
      CallstackIdx cur = idx;
      for (size_t d = 0; d < depth; ++d) {
        ASSERT_NE(cur, CallstackIdx(0));
        ASSERT_EQ(in.node(cur).frame, frames[d]);
        cur = in.node(cur).parent;
      }
      ASSERT_EQ(cur, CallstackIdx(0));
    }
  }
  // Distinct oracle stacks must have distinct leaf nodes and be stable on re-intern.
  std::map<CallstackIdx, int> leaves;
  for (const auto& [key, idx] : oracle) {
    leaves[idx]++;
    std::vector<FrameIdx> fr;
    for (uint32_t f : key) fr.push_back(FrameIdx(f));
    ASSERT_EQ(in.internStack(fr), idx);
  }
  EXPECT_EQ(leaves.size(), oracle.size());
  // Unique (parent, frame) pairs: every node is reachable by exactly one key.
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> seen;
  for (size_t i = 1; i < in.size(); ++i) {
    auto n = in.node(CallstackIdx(static_cast<uint32_t>(i)));
    ASSERT_TRUE(seen.emplace(std::make_pair(n.parent.get(), n.frame.get()), static_cast<uint32_t>(i)).second);
  }
}

TEST(CallstackInterner, CompactKeepsContentAndShrinksMemory) {
  CallstackInterner in;
  std::vector<CallstackIdx> idx;
  for (uint32_t i = 0; i < 300000; ++i) idx.push_back(in.intern(CallstackIdx(i / 3), FrameIdx(i % 977)));
  const size_t before = in.size();
  const double beforeBytes = in.bytesPerNode();
  in.compact();
  EXPECT_EQ(in.size(), before);
  EXPECT_LE(in.bytesPerNode(), beforeBytes);
  EXPECT_LE((in.size() - 1) * 10, in.hashTableSlots() * 7);
  EXPECT_LE(in.bytesPerNode(), 8.0 + 4.0 / 0.7 + 0.1);
  for (uint32_t i = 0; i < 300000; ++i) ASSERT_EQ(in.intern(CallstackIdx(i / 3), FrameIdx(i % 977)), idx[i]);
  EXPECT_EQ(in.size(), before);
  // Interning keeps working (and growing) after compaction.
  for (uint32_t i = 0; i < 100000; ++i) in.intern(CallstackIdx(0), FrameIdx(100000 + i));
  EXPECT_LE((in.size() - 1) * 10, in.hashTableSlots() * 7);
}

TEST(CallstackInterner, ReserveAvoidsResizeAndKeepsContent) {
  CallstackInterner in;
  in.reserve(1'000'000);
  const size_t slots = in.hashTableSlots();
  for (uint32_t i = 0; i < 1'000'000; ++i) in.intern(CallstackIdx(0), FrameIdx(i));
  EXPECT_EQ(in.hashTableSlots(), slots);
  EXPECT_EQ(in.intern(CallstackIdx(0), FrameIdx(777)), CallstackIdx(778));
}

}  // namespace
}  // namespace tachyon::trace
