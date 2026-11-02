// WP-34: instructionsForFunction exactness on hand-built models.
#include "lines_test_util.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace tachyon::analysis {
namespace {

using namespace linetest;

TEST(InstructionsTest, ExactAddressesSortedAndWeighted) {
  Probe p(2);
  // Leaf positions use the exact address; non-leaf positions the return address itself.
  p.samples(p.h.stack({kMain + 0x20, kF + 0x44, kG + 0x08}), 2, 0);  // F@0x1044 as return address
  p.samples(p.h.stack({kMain + 0x20, kF + 0x04}), 3, 1);             // F@0x1004 leaf
  p.samples(p.h.stack({kMain + 0x20, kF + 0x44}), 1, 1);             // F@0x1044 leaf
  p.samples(p.h.stack({kMain + 0x20, kF + 0x90}), 4, 0);             // F@0x1090 leaf
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);

  auto ins = instructionsForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(ins.ok()) << ins.error().toString();
  ASSERT_EQ(ins->size(), 3u);
  EXPECT_EQ((*ins)[0].rva, kF + 0x04);
  EXPECT_EQ((*ins)[0].ticks, 300u);
  EXPECT_EQ((*ins)[1].rva, kF + 0x44);
  EXPECT_EQ((*ins)[1].ticks, 300u);  // 2 as return address + 1 as leaf
  EXPECT_EQ((*ins)[2].rva, kF + 0x90);
  EXPECT_EQ((*ins)[2].ticks, 400u);
  EXPECT_TRUE(std::is_sorted(ins->begin(), ins->end(),
                             [](const InstructionAgg& a, const InstructionAgg& b) { return a.rva < b.rva; }));

  uint64_t total = 0;
  for (const InstructionAgg& i : *ins) total += i.ticks;
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  for (const FunctionRow& r : *rows)
    if (r.key == fk(kF)) EXPECT_EQ(total, r.inclusiveTicks);

  // Thread filter.
  Filter f;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto t1 = instructionsForFunction(ctx, f, fk(kF));
  ASSERT_TRUE(t1.ok());
  ASSERT_EQ(t1->size(), 2u);
  EXPECT_EQ((*t1)[0].rva, kF + 0x04);
  EXPECT_EQ((*t1)[1].rva, kF + 0x44);
  EXPECT_EQ((*t1)[1].ticks, 100u);
}

TEST(InstructionsTest, OnlyRunningSamplesCount) {
  Probe p;
  p.h.m.statesByThread[0].clear();
  p.h.state(0, 0, 400, ThreadState::Running);
  p.h.state(0, 400, 1000, ThreadState::Waiting);
  p.h.state(0, 1000, 5000, ThreadState::Running);
  const CallstackIdx s = p.h.stack({kMain + 0x20, kF + 0x04});
  p.h.m.statesByThread[0][1].blocking_stack = s;
  p.samples(s, 8);  // ts 0..1400: samples at 400, 600, 800 are in the Waiting interval (weight 0)
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto ins = instructionsForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(ins.ok());
  ASSERT_EQ(ins->size(), 1u);
  EXPECT_EQ((*ins)[0].ticks, 500u);  // samples at 0, 200 and 1000, 1200, 1400 (5 x 100)
  // Waiting time is only visible per line.
  auto lines = linesForFunction(ctx, {}, fk(kF));
  ASSERT_TRUE(lines.ok());
  ASSERT_EQ(lines->size(), 1u);
  EXPECT_EQ((*lines)[0].runningTicks, 500u);
  EXPECT_EQ((*lines)[0].waitingTicks, 600u);
}

TEST(InstructionsTest, UnmappedKeyUsesTheAbsoluteAddress) {
  Probe p;
  const CallstackIdx s = p.h.pushAbs(p.h.stack({kMain + 0x20}), 0x7FF000001234ull);
  p.samples(s, 3);
  p.finish();
  QueryContext ctx(p.h.m, *p.h.svc);
  auto ins = instructionsForFunction(ctx, {}, FunctionKey{ModuleIdx::invalid(), 0x7FF000001234ull});
  ASSERT_TRUE(ins.ok());
  ASSERT_EQ(ins->size(), 1u);
  EXPECT_EQ((*ins)[0].rva, 0x7FF000001234ull);
  EXPECT_EQ((*ins)[0].ticks, 300u);
}

}  // namespace
}  // namespace tachyon::analysis
