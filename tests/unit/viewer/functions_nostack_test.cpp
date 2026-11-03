// WP-33b problem 2: samples with no frames ("<no stack>") are in the Function List total, so its percentages
// equal the Call Graph's for the same function, in every inline x kernel mode; the row is named "<no stack>"
// in the Function List and the Butterfly.
#include "analysis/aggregation_test_util.h"
#include "analysis/query.h"
#include "viewer/callgraph/callgraph_model.h"
#include "viewer/common/function_names.h"
#include "viewer/functions/butterfly_model.h"
#include "viewer/functions/function_list_model.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fl = tachyon::viewer::functions;
using analysis::testutil::HandModel;

namespace {

// 6 samples of A->B (leaf B), 4 of A (leaf A) and 10 frameless ones: the frameless samples are half of the
// trace.
struct Frameless {
  Frameless() {
    const CallstackIdx ab = h.stack({0x1010, 0x2010});
    const CallstackIdx a = h.stack({0x1010});
    uint64_t ts = 0;
    for (int i = 0; i < 6; ++i, ts += 100) h.sample(0, ts, ab);
    for (int i = 0; i < 4; ++i, ts += 100) h.sample(0, ts, a);
    for (int i = 0; i < 10; ++i, ts += 100) h.sample(0, ts, CallstackIdx(0));
    h.state(0, 0, 100000, model::ThreadState::Running);
    h.finish();
  }
  HandModel h;
};

}  // namespace

TEST(FunctionListNoStack, TotalAndPercentagesEqualTheCallGraphInEveryMode) {
  Frameless f;
  for (const bool inl : {false, true}) {
    for (const auto kernel :
         {analysis::KernelFrames::Show, analysis::KernelFrames::Group, analysis::KernelFrames::Hide}) {
      analysis::AggregationOptions options;
      options.expandInline = inl;
      options.kernelFrames = kernel;
      analysis::QueryContext ctx(f.h.m, *f.h.svc, options);
      const std::string what = std::string(inl ? "inline " : "plain ") + std::to_string(int(kernel));
      const auto tree = analysis::topDownTree(ctx, {});
      const auto rows = analysis::bottomUpFunctions(ctx, {});
      ASSERT_TRUE(tree.ok() && rows.ok()) << what;
      const auto table = fl::buildTable(
          *rows, [&](const symbols::FunctionKey& k) { return common::splitFunctionName(&ctx, k, true); },
          1e9);
      EXPECT_EQ(table->totalTicks, tree->inclusiveTicks) << what;
      EXPECT_EQ(table->totalTicks, 2000u) << what;

      // The same function has the same % in both views (the root is 100 %).
      for (const analysis::NodeAgg& child : tree->children) {
        const fl::Entry* e = nullptr;
        for (const fl::Entry& x : table->entries)
          if (x.key == child.key) e = &x;
        ASSERT_NE(e, nullptr) << what;
        EXPECT_DOUBLE_EQ(fl::percentOfTotal(e->inclusiveTicks, table->totalTicks),
                         callgraph::percentOf(child.inclusiveTicks, tree->inclusiveTicks,
                                              tree->inclusiveTicks, callgraph::PercentMode::OfRoot))
            << what;
        EXPECT_DOUBLE_EQ(fl::percentOfTotal(e->exclusiveTicks, table->totalTicks),
                         callgraph::percentOf(child.exclusiveTicks, tree->inclusiveTicks,
                                              tree->inclusiveTicks, callgraph::PercentMode::OfRoot))
            << what;
      }
      // The A function is 50 % of the total (10 of 20 samples), not 100 %.
      const auto a = std::find_if(table->entries.begin(), table->entries.end(), [](const fl::Entry& e) {
        return e.key == symbols::FunctionKey{ModuleIdx(0), 0x1010};
      });
      ASSERT_NE(a, table->entries.end()) << what;
      EXPECT_DOUBLE_EQ(fl::percentOfTotal(a->inclusiveTicks, table->totalTicks), 50.0) << what;

      // The frameless samples: "<no stack>" (Show / Group) or "<kernel>" (Hide), in both views.
      const symbols::FunctionKey key =
          kernel == analysis::KernelFrames::Hide ? analysis::kKernelKey : analysis::kRootKey;
      const auto row = std::find_if(table->entries.begin(), table->entries.end(),
                                    [&](const fl::Entry& e) { return e.key == key; });
      ASSERT_NE(row, table->entries.end()) << what;
      EXPECT_EQ(row->exclusiveTicks, 1000u) << what;
      EXPECT_EQ(row->inclusiveTicks, 1000u) << what;
      EXPECT_EQ(row->function, kernel == analysis::KernelFrames::Hide ? "<kernel>" : "<no stack>") << what;
    }
  }
}

TEST(FunctionListNoStack, ButterflyNamesTheRowNoStack) {
  Frameless f;
  analysis::QueryContext ctx(f.h.m, *f.h.svc);
  const auto bf = analysis::butterfly(ctx, {}, analysis::kRootKey);
  ASSERT_TRUE(bf.ok());
  EXPECT_EQ(bf->self.inclusiveTicks, 1000u);
  const fl::ButterflyView v = fl::buildButterflyView(
      *bf, [&](const symbols::FunctionKey& k) { return common::splitFunctionName(&ctx, k); });
  EXPECT_EQ(v.self.name.function, "<no stack>");
  EXPECT_EQ(v.self.name.full, "<no stack>");
  // The call graph root keeps its name.
  EXPECT_EQ(common::splitFunctionName(&ctx, analysis::kRootKey).function, "<root>");
}
