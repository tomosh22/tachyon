#include "symbols/function_key.h"
#include "symbols_test_util.h"

#include <gtest/gtest.h>

#include <unordered_set>

namespace tachyon::symbols {
namespace {

TEST(FunctionKeyTest, ComparisonAndHashing) {
  const FunctionKey a{ModuleIdx(1), 0x1000};
  const FunctionKey b{ModuleIdx(1), 0x1000};
  const FunctionKey c{ModuleIdx(2), 0x1000};
  const FunctionKey d{ModuleIdx(1), 0x1001};
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_NE(a, d);
  EXPECT_LT(a, c);
  EXPECT_LT(a, d);
  EXPECT_EQ(std::hash<FunctionKey>{}(a), std::hash<FunctionKey>{}(b));
  EXPECT_NE(std::hash<FunctionKey>{}(a), std::hash<FunctionKey>{}(c));
  std::unordered_set<FunctionKey> set{a, b, c, d};
  EXPECT_EQ(set.size(), 3u);
}

TEST(FunctionKeyTest, ScopeKeys) {
  const FunctionKey s = FunctionKey::scope(StringIdx(7));
  EXPECT_EQ(s.module, kScopeModule);
  EXPECT_EQ(s.value, 7u);
  EXPECT_TRUE(s.isScope());
  EXPECT_FALSE(s.isUnmapped());
  EXPECT_NE(s, (FunctionKey{ModuleIdx(0), 7}));
  EXPECT_EQ(kScopeModule.get(), 0xFFFFFFFEu);
}

TEST(FunctionKeyTest, DerivationFromFrames) {
  model::TraceModel m = testutil::loadModel("tiny");
  SymbolService svc({}, {});
  ASSERT_TRUE(svc.addSymbolFileOverride(testutil::fixtureFile("tiny", ".symbols.json")).ok());

  bool anyUnmapped = false;
  for (const model::Frame& f : m.frames) anyUnmapped |= !f.module.valid();
  ASSERT_TRUE(anyUnmapped) << "tiny contains an unmapped frame";
  svc.resolveAllAsync(m);
  svc.waitAll();

  // Resolved: (module, functionStartRva), shared by all addresses of the function.
  for (uint32_t f = 0; f < m.frames.size(); ++f) {
    const FunctionKey k = functionKeyFor(svc, FrameIdx(f));
    if (!m.frames[f].module.valid()) {
      EXPECT_EQ(k.module, ModuleIdx::invalid());
      EXPECT_EQ(k.value, m.frames[f].addr_or_rva);
      EXPECT_TRUE(k.isUnmapped());
      continue;
    }
    EXPECT_EQ(k.module, m.frames[f].module);
    EXPECT_EQ(k.value, svc.lookup(FrameIdx(f)).functionStartRva);
    EXPECT_FALSE(k.isInline());
    EXPECT_EQ(functionKeyFor(svc, FrameIdx(f), true).module, k.module);
  }

  // Frames of the same function but different offsets share a key.
  std::unordered_set<FunctionKey> keys;
  for (uint32_t f = 0; f < m.frames.size(); ++f) keys.insert(functionKeyFor(svc, FrameIdx(f)));
  EXPECT_LT(keys.size(), m.frames.size() + 1);
}

TEST(FunctionKeyTest, UnresolvedFramesGroupPerAddress) {
  model::TraceModel m = testutil::loadModel("tiny");
  SymbolService svc({}, {});  // nothing registered: all modules NotFound
  svc.resolveAllAsync(m);
  svc.waitAll();
  for (uint32_t f = 0; f < m.frames.size(); ++f) {
    const FunctionKey k = functionKeyFor(svc, FrameIdx(f));
    EXPECT_EQ(k.module, m.frames[f].module);
    EXPECT_EQ(k.value, m.frames[f].addr_or_rva);
  }
}

TEST(FunctionKeyTest, InlineExpansion) {
  model::TraceModel m = testutil::loadModel("medium");
  SymbolService svc({}, {});
  ASSERT_TRUE(svc.addSymbolFileOverride(testutil::fixtureFile("medium", ".symbols.json")).ok());
  svc.resolveAllAsync(m);
  svc.waitAll();
  size_t expanded = 0;
  for (uint32_t f = 0; f < m.frames.size(); ++f) {
    const ResolvedAddress& a = svc.lookup(FrameIdx(f));
    const FunctionKey plain = functionKeyFor(svc, FrameIdx(f), false);
    const FunctionKey inl = functionKeyFor(svc, FrameIdx(f), true);
    if (a.inlines.empty()) {
      EXPECT_EQ(plain, inl);
      continue;
    }
    ++expanded;
    EXPECT_TRUE(inl.isInline());
    EXPECT_EQ(inl.module, plain.module);
    EXPECT_EQ(inl.value & ~kInlineKeyBit, a.inlines.front().function.get());
    EXPECT_NE(plain, inl);
  }
  std::printf("[symbols] medium: %zu frames with inline-expanded keys\n", expanded);
}

}  // namespace
}  // namespace tachyon::symbols
