// Perf budgets (WP-44), asserted at 2x:
//  * sorting and text-filtering a 100 000 row function table (every sort column): < 20 ms;
//  * one Function List panel frame with 100 000 rows (virtualized by the list clipper): < 2 ms of CPU.
// The numbers are printed.
#include "viewer/app.h"
#include "viewer/functions/function_list_model.h"
#include "viewer/functions/function_list_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fl = tachyon::viewer::functions;
using analysis::FunctionRow;
using symbols::FunctionKey;

namespace {

constexpr double kViewBudgetMs = 20.0;
constexpr double kFrameBudgetMs = 2.0;
constexpr size_t kRows = 100'000;

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::shared_ptr<const fl::Table> bigTable() {
  std::vector<FunctionRow> rows(kRows);
  uint64_t x = 0x9E3779B97F4A7C15ull;
  const auto next = [&] {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
  };
  for (size_t i = 0; i < kRows; ++i) {
    rows[i].key = FunctionKey{ModuleIdx(static_cast<uint32_t>(i % 40)), 0x1000 + i * 16};
    rows[i].exclusiveTicks = next() % 100000;
    rows[i].inclusiveTicks = rows[i].exclusiveTicks + next() % 100000;
    rows[i].instances = static_cast<uint32_t>(next() % 5000);
  }
  return fl::buildTable(
      rows,
      [](const FunctionKey& k) {
        common::NameParts p;
        p.module = "module" + std::to_string(k.module.get()) + ".dll";
        p.function =
            "Namespace::Class" + std::to_string(k.value % 977) + "::method_" + std::to_string(k.value);
        p.full = p.module + "!" + p.function;
        return p;
      },
      1e9);
}

}  // namespace

TEST(FunctionsPerf, SortAndFilterHundredThousandRows) {
  const auto table = bigTable();
  ASSERT_EQ(table->entries.size(), kRows);
  struct Case {
    const char* name;
    fl::SortSpec sort;
    const char* text;
  };
  const Case cases[] = {
      {"exclusive desc, no filter", {fl::SortColumn::Exclusive, false}, ""},
      {"inclusive asc, no filter", {fl::SortColumn::Inclusive, true}, ""},
      {"instances desc, no filter", {fl::SortColumn::Instances, false}, ""},
      {"function asc, no filter", {fl::SortColumn::Function, true}, ""},
      {"module desc, no filter", {fl::SortColumn::Module, false}, ""},
      {"exclusive desc, filter \"class12\"", {fl::SortColumn::Exclusive, false}, "class12"},
      {"function asc, filter \"method_1\"", {fl::SortColumn::Function, true}, "method_1"},
  };
  double worst = 0;
  for (const Case& c : cases) {
    double best = 1e30;
    size_t size = 0;
    for (int i = 0; i < 7; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      const auto view = fl::buildView(*table, fl::normalizeFilterText(c.text), c.sort);
      best = std::min(best, msSince(t0));
      size = view.size();
    }
    std::printf("[functions perf] %-36s %6zu rows: %.2f ms (budget %.0f ms)\n", c.name, size, best,
                kViewBudgetMs);
    worst = std::max(worst, best);
    EXPECT_LT(best, kViewBudgetMs * 2.0) << c.name;
  }
  // Grouping on top of the unfiltered view.
  const fl::SortSpec sort{fl::SortColumn::Exclusive, false};
  const auto view = fl::buildView(*table, "", sort);
  double bestGroup = 1e30;
  for (int i = 0; i < 7; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto groups = fl::groupByModule(*table, view, sort);
    const auto items = fl::groupedItems(groups, {});
    bestGroup = std::min(bestGroup, msSince(t0));
    ASSERT_EQ(items.size(), kRows + 40);
  }
  std::printf("[functions perf] group by module (40 modules): %.2f ms; worst sort/filter %.2f ms\n",
              bestGroup, worst);
  EXPECT_LT(bestGroup, kViewBudgetMs * 2.0);
}

TEST(FunctionsPerf, PanelFrameWithHundredThousandRows) {
  AppOptions options;
  options.persist = false;
  options.pacing = false;
  options.configPath =
      std::filesystem::path(TACHYON_BINARY_DIR) / "viewer_functions_perf_no_such_settings.json";
  App app(makeNullRenderer(), options);
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  auto* panel = dynamic_cast<FunctionListPanel*>(app.panels().find(FunctionListPanel::kName)->panel.get());
  ASSERT_NE(panel, nullptr);
  for (PanelEntry& e : app.panels().entries())
    if (e.panel.get() != panel) e.open = false;

  app.openFile(std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.tachyon");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  const auto settled = [&] {
    if (!app.document()) return false;
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    return p.total > 0 && p.done == p.total && !panel->pending() && panel->table() &&
           panel->requestedSpec().symbolGeneration == app.symbolGeneration();
  };
  while (!settled()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 3; ++i) app.frame(0);

  panel->injectTable(bigTable());
  ASSERT_EQ(panel->items().size(), kRows);
  for (int i = 0; i < 5; ++i) app.frame(0);  // warm up: fonts, layout
  ASSERT_FALSE(panel->drawnRows().empty()) << "the panel must really draw rows";
  std::printf("[functions perf] rows drawn per frame (clipped): %zu of %zu\n", panel->drawnRows().size(),
              kRows);

  double best = 1e30, sum = 0;
  constexpr int kFrames = 30;
  for (int i = 0; i < kFrames; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    app.frame(0);
    const double ms = msSince(t0);
    best = std::min(best, ms);
    sum += ms;
  }
  std::printf("[functions perf] panel frame with %zu rows: best %.2f ms, mean %.2f ms (budget %.0f ms)\n",
              kRows, best, sum / kFrames, kFrameBudgetMs);
  EXPECT_LT(best, kFrameBudgetMs * 2.0);
}
