// Perf budgets (WP-43), asserted at 2x:
//  * building the visible-row array for a fully expanded tree of 100 000 nodes: < 20 ms;
//  * one panel frame with 5 000 visible rows (virtualized by the list clipper): < 4 ms of CPU.
// The numbers are printed.
#include "viewer/analysis_context.h"
#include "viewer/app.h"
#include "viewer/callgraph/callgraph_model.h"
#include "viewer/callgraph/callgraph_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace cg = tachyon::viewer::callgraph;
using analysis::NodeAgg;
using symbols::FunctionKey;

namespace {

constexpr double kFlattenBudgetMs = 20.0;
constexpr double kFrameBudgetMs = 4.0;

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A tree of exactly `count` nodes (root included), fan-out 8, built breadth first without recursion.
NodeAgg wideTree(size_t count) {
  NodeAgg root;
  root.key = analysis::kRootKey;
  root.inclusiveTicks = 1000000;
  root.sampleCount = 1000;
  std::vector<NodeAgg*> level{&root};
  size_t made = 1;
  uint64_t key = 1;
  while (made < count && !level.empty()) {
    std::vector<NodeAgg*> next;
    for (NodeAgg* parent : level) {
      const size_t kids = std::min<size_t>(8, count - made);
      if (kids == 0) break;
      parent->children.resize(kids);  // never resized again: the pointers below stay valid
      for (NodeAgg& c : parent->children) {
        c.key = FunctionKey{ModuleIdx(0), key++};
        c.inclusiveTicks = 1000;
        c.exclusiveTicks = 10;
        c.sampleCount = 1;
        next.push_back(&c);
      }
      made += kids;
    }
    level = std::move(next);
  }
  return root;
}

}  // namespace

TEST(CallGraphPerf, FlattenFullyExpandedHundredThousandNodes) {
  const NodeAgg root = wideTree(100'000);
  cg::ExpandSet expanded;
  ASSERT_FALSE(cg::expandAll(root, expanded, 200'000));
  std::vector<cg::Row> rows;
  double best = 1e30;
  for (int i = 0; i < 9; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    cg::flattenVisible(root, expanded, cg::kMaxVisibleRows + 1, rows);
    best = std::min(best, msSince(t0));
  }
  ASSERT_EQ(rows.size(), 100'000u);
  double bestExpand = 1e30;
  for (int i = 0; i < 5; ++i) {
    cg::ExpandSet e;
    const auto t0 = std::chrono::steady_clock::now();
    cg::expandAll(root, e, 200'000);
    bestExpand = std::min(bestExpand, msSince(t0));
  }
  std::printf("[callgraph perf] flatten 100k expanded rows: %.2f ms (budget %.0f ms); expandAll: %.2f ms\n",
              best, kFlattenBudgetMs, bestExpand);
  EXPECT_LT(best, kFlattenBudgetMs * 2.0);
}

TEST(CallGraphPerf, PanelFrameWithFiveThousandVisibleRows) {
  AppOptions options;
  options.persist = false;
  options.pacing = false;
  options.configPath =
      std::filesystem::path(TACHYON_BINARY_DIR) / "viewer_callgraph_perf_no_such_settings.json";
  App app(makeNullRenderer(), options);
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  auto* panel = dynamic_cast<CallGraphPanel*>(app.panels().find(CallGraphPanel::kName)->panel.get());
  ASSERT_NE(panel, nullptr);
  for (PanelEntry& e : app.panels().entries())
    if (e.panel.get() != panel) e.open = false;

  app.openFile(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.tachyon");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  const auto settled = [&] {
    if (!app.document()) return false;
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    return p.total > 0 && p.done == p.total && !panel->pending() && panel->tree() &&
           panel->requestedSpec().symbolGeneration == app.symbolGeneration();
  };
  while (!settled()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 3; ++i) app.frame(0);

  // 5 000 children under the root, all visible.
  auto tree = std::make_shared<NodeAgg>();
  tree->key = analysis::kRootKey;
  tree->inclusiveTicks = 5'000'000;
  tree->sampleCount = 5000;
  tree->children.resize(5000);
  for (size_t i = 0; i < tree->children.size(); ++i) {
    NodeAgg& c = tree->children[i];
    c.key = FunctionKey{ModuleIdx(0), 0x1000 + i};
    c.inclusiveTicks = 1000;
    c.exclusiveTicks = 1000;
    c.sampleCount = 1;
  }
  const auto snapshot = app.document()->analysis().snapshot(analysis::AggregationOptions{},
                                                            app.symbolGeneration(), &app.queryExecutor());
  ASSERT_TRUE(snapshot);
  panel->injectTree(tree, snapshot, 1e7);
  ASSERT_EQ(panel->rows().size(), 5001u);    // the hot path from the root expands the one level
  for (int i = 0; i < 5; ++i) app.frame(0);  // warm up: fonts, name cache for the visible rows
  ASSERT_FALSE(panel->drawnRows().empty()) << "the panel must really draw rows";
  std::printf("[callgraph perf] rows drawn per frame (clipped): %zu\n", panel->drawnRows().size());

  double best = 1e30, sum = 0;
  constexpr int kFrames = 30;
  for (int i = 0; i < kFrames; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    app.frame(0);
    const double ms = msSince(t0);
    best = std::min(best, ms);
    sum += ms;
  }
  std::printf(
      "[callgraph perf] panel frame with 5000 visible rows: best %.2f ms, mean %.2f ms (budget %.0f ms)\n",
      best, sum / kFrames, kFrameBudgetMs);
  EXPECT_LT(best, kFrameBudgetMs * 2.0);
  panel->injectTree(nullptr, nullptr, 1e7);
}
