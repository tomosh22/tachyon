// Perf budgets (WP-45), asserted at 2x:
//  * opening a 50 000-line source file with a hot function on the worker (locate, read, split, line and
//    instruction queries, disassembly attempt): < 50 ms;
//  * one Source panel frame showing that file (virtualized by the list clipper): < 2 ms of CPU.
// The numbers are printed.
#include "analysis/query.h"
#include "viewer/app.h"
#include "viewer/source/source_model.h"
#include "viewer/source/source_panel.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace tachyon;
using namespace tachyon::viewer;
namespace fs = std::filesystem;

namespace {

constexpr double kWorkerBudgetMs = 50.0;
constexpr double kFrameBudgetMs = 2.0;
constexpr uint32_t kFileLines = 50'000;
constexpr uint64_t kFnStart = 0x1000, kFnSize = 0x20000, kBytesPerLine = 0x20;
constexpr uint32_t kFirstLine = 20'000;  // the hot function covers lines [kFirstLine, kFirstLine + 4096)
constexpr size_t kSamples = 100'000;

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

fs::path writeBigSource() {
  const fs::path dir = fs::path(TACHYON_BINARY_DIR) / "viewer_source_perf_tmp";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path file = dir / "big_source.cpp";
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  for (uint32_t i = 1; i <= kFileLines; ++i)
    out << "    result += compute(items[" << i << "], scale) * factor + offset;  // line " << i << "\n";
  return file;
}

// One module with one function "hot" at kFnStart; each kBytesPerLine bytes belong to the next source line.
class HotProvider final : public symbols::ISymbolProvider {
 public:
  HotProvider(symbols::SymbolStringTable& strings, const std::string& file)
      : m_name(strings.intern("hot")), m_file(strings.intern(file)) {}
  Result<void> load(const model::Module&, const fs::path&) override { return {}; }
  symbols::ResolvedAddress resolve(uint64_t rva) override {
    symbols::ResolvedAddress r;
    if (rva < kFnStart || rva >= kFnStart + kFnSize) return r;
    r.resolved = true;
    r.function = m_name;
    r.functionStartRva = kFnStart;
    r.file = m_file;
    r.line = kFirstLine + static_cast<uint32_t>((rva - kFnStart) / kBytesPerLine) % 4096;
    return r;
  }
  Result<std::vector<symbols::LineRange>> linesForFunction(uint64_t start) override {
    if (start != kFnStart) return makeError(ErrorCode::NotFound, "no function");
    std::vector<symbols::LineRange> out;
    for (uint64_t off = 0; off < kFnSize; off += kBytesPerLine)
      out.push_back({kFnStart + off, static_cast<uint32_t>(kBytesPerLine), m_file,
                     kFirstLine + static_cast<uint32_t>(off / kBytesPerLine) % 4096});
    return out;
  }
  Result<symbols::FunctionBounds> functionBounds(uint64_t rva) override {
    if (rva < kFnStart || rva >= kFnStart + kFnSize) return makeError(ErrorCode::NotFound, "gap");
    return symbols::FunctionBounds{kFnStart, kFnSize, m_name};
  }

 private:
  StringIdx m_name, m_file;
};

struct HotModel {
  model::TraceModel m;
  std::unique_ptr<symbols::SymbolService> svc;

  explicit HotModel(const std::string& file) {
    m.header.start_ts = 0;
    m.header.end_ts = kSamples * 100 + 1000;
    m.sampleIntervalTicks = 100;
    m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
    model::Module mod;
    mod.path = "/nonexistent/hot.bin";
    m.modules.push_back(mod);
    model::Thread th;
    th.tid = 1;
    m.threads.push_back(th);
    m.tidToThread[th.tid] = ThreadIdx(0);
    m.samplesByThread.resize(1);
    m.statesByThread.resize(1);
    m.scopesByThread.resize(1);
    model::ThreadStateInterval iv;
    iv.start = 0;
    iv.end = m.header.end_ts;
    iv.state = model::ThreadState::Running;
    m.statesByThread[0].push_back(iv);
    // One leaf node per distinct address (4 byte steps over the function).
    std::vector<CallstackIdx> leaves;
    for (uint64_t off = 0; off < kFnSize; off += 4) {
      m.frames.push_back(model::Frame{ModuleIdx(0), kFnStart + off});
      m.nodes.push_back(
          model::CallstackNode{CallstackIdx(0), FrameIdx(static_cast<uint32_t>(m.frames.size() - 1))});
      leaves.push_back(CallstackIdx(static_cast<uint32_t>(m.nodes.size() - 1)));
    }
    uint64_t x = 0x9E3779B97F4A7C15ull;
    auto& c = m.samplesByThread[0];
    for (size_t i = 0; i < kSamples; ++i) {
      x ^= x << 13;
      x ^= x >> 7;
      x ^= x << 17;
      c.ts.push_back(i * 100 + 50);
      c.callstack.push_back(leaves[x % leaves.size()]);
      c.cpu.push_back(0);
      c.flags.push_back(0);
    }
    svc = std::make_unique<symbols::SymbolService>(std::vector<std::string>{}, fs::path{});
    svc->setLocate([](const model::Module&) -> Result<fs::path> { return fs::path("hot.sym"); });
    symbols::SymbolStringTable& strings = svc->stringSink();
    svc->setProviderFactory(
        [&strings, file](const model::Module&, const fs::path&) -> std::unique_ptr<symbols::ISymbolProvider> {
          return std::make_unique<HotProvider>(strings, file);
        });
    svc->resolveAllAsync(m);
    svc->waitAll();
  }
};

}  // namespace

TEST(SourcePerf, OpeningAFiftyThousandLineFileOnTheWorker) {
  const fs::path file = writeBigSource();
  HotModel hot(file.string());
  source::BuildInputs inputs;
  inputs.key = symbols::FunctionKey{ModuleIdx(0), kFnStart};
  inputs.locator = std::make_shared<symbols::SourceLocator>();
  std::vector<double> times;
  std::shared_ptr<const source::SourceView> last;
  for (int i = 0; i < 7; ++i) {
    // A cold context each time: the query cache starts empty, only the OS file cache is warm.
    analysis::QueryContext ctx(hot.m, *hot.svc);
    inputs.locator->clearCache();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = source::buildSourceView(ctx, inputs, {});
    times.push_back(msSince(t0));
    ASSERT_TRUE(r.ok());
    last = *r;
  }
  ASSERT_NE(last, nullptr);
  ASSERT_EQ(last->kind, source::ViewKind::Source) << last->message << last->sourceProblem;
  ASSERT_NE(last->file, nullptr);
  EXPECT_EQ(last->file->lineCount(), kFileLines);
  EXPECT_NE(last->hottestLine, 0u);
  EXPECT_GE(last->hottestLine, kFirstLine);
  EXPECT_LT(last->hottestLine, kFirstLine + 4096);
  std::sort(times.begin(), times.end());
  std::printf(
      "[source perf] open %u-line file (%zu bytes), %zu samples: median %.2f ms, best %.2f ms (budget %.0f "
      "ms)\n",
      kFileLines, last->file->byteSize(), kSamples, times[times.size() / 2], times.front(), kWorkerBudgetMs);
  EXPECT_LT(times[times.size() / 2], kWorkerBudgetMs * 2.0);

  // The pieces: read + split alone.
  double bestLoad = 1e30;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    auto text = source::SourceText::load(file);
    bestLoad = std::min(bestLoad, msSince(t0));
    ASSERT_TRUE(text.ok());
  }
  std::printf("[source perf] read + split the file alone: %.2f ms\n", bestLoad);

  // Searching the whole file.
  double bestFind = 1e30;
  size_t hits = 0;
  for (int i = 0; i < 5; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    hits = 0;
    for (uint32_t l = 0; (l = last->file->findNext("items[4999", l + 1)) != 0;) ++hits;
    bestFind = std::min(bestFind, msSince(t0));
  }
  std::printf("[source perf] search \"items[4999\" over %u lines: %zu hits, %.2f ms\n", kFileLines, hits,
              bestFind);
  EXPECT_GT(hits, 0u);
  EXPECT_LT(bestFind, 20.0 * 2.0);
}

TEST(SourcePerf, PanelFrameWithAFiftyThousandLineFile) {
  const fs::path file = writeBigSource();
  HotModel hot(file.string());
  source::BuildInputs inputs;
  inputs.key = symbols::FunctionKey{ModuleIdx(0), kFnStart};
  inputs.locator = std::make_shared<symbols::SourceLocator>();
  analysis::QueryContext ctx(hot.m, *hot.svc);
  auto built = source::buildSourceView(ctx, inputs, {});
  ASSERT_TRUE(built.ok());
  ASSERT_EQ((*built)->kind, source::ViewKind::Source);

  AppOptions options;
  options.persist = false;
  options.pacing = false;
  options.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_source_perf_no_such_settings.json";
  App app(makeNullRenderer(), options);
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  auto* panel = dynamic_cast<SourcePanel*>(app.panels().find(SourcePanel::kName)->panel.get());
  ASSERT_NE(panel, nullptr);
  for (PanelEntry& e : app.panels().entries()) e.open = e.panel.get() == panel;

  app.openFile(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.tachyon");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  const auto settled = [&] {
    if (!app.document()) return false;
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    return p.total > 0 && p.done == p.total && !panel->pending() && panel->view() &&
           panel->requestedSpec().symbolGeneration == app.symbolGeneration();
  };
  // Any function of the fixture: the panel needs a selection to keep a view.
  while (!app.document()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  app.selection().setSelectedFunction(symbols::FunctionKey{ModuleIdx(0), 0x1200});
  while (!settled()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 0; i < 3; ++i) app.frame(0);

  panel->injectView(*built);
  ASSERT_EQ(panel->rows().size(), kFileLines);
  ImGui::SetWindowSize(SourcePanel::kName, ImVec2(1000, 700));
  for (int i = 0; i < 5; ++i) app.frame(0);  // warm up: fonts, layout, the scroll to the hottest line
  ASSERT_FALSE(panel->drawnRows().empty()) << "the panel must really draw rows";
  std::printf("[source perf] rows drawn per frame (clipped): %zu of %zu\n", panel->drawnRows().size(),
              panel->rows().size());
  EXPECT_LT(panel->drawnRows().size(), 200u);
  EXPECT_NE(panel->firstVisibleRow(), SIZE_MAX);

  double best = 1e30, sum = 0;
  constexpr int kFrames = 30;
  for (int i = 0; i < kFrames; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    app.frame(0);
    const double ms = msSince(t0);
    best = std::min(best, ms);
    sum += ms;
  }
  std::printf("[source perf] panel frame with %u lines: best %.2f ms, mean %.2f ms (budget %.0f ms)\n",
              kFileLines, best, sum / kFrames, kFrameBudgetMs);
  EXPECT_LT(best, kFrameBudgetMs * 2.0);
}

// A 1 MB line with a match in every cell, scrolled to its far right end: the search highlight must cost what
// is on screen, not the length of the line (it once re-walked the line from column 0 per match: seconds).
TEST(SourcePerf, SearchHighlightOnAOneMegabyteLineScrolledFarRight) {
  AppOptions options;
  options.persist = false;
  options.pacing = false;
  options.configPath = fs::path(TACHYON_BINARY_DIR) / "viewer_source_perf_no_such_settings.json";
  App app(makeNullRenderer(), options);
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  auto* panel = dynamic_cast<SourcePanel*>(app.panels().find(SourcePanel::kName)->panel.get());
  ASSERT_NE(panel, nullptr);
  for (PanelEntry& e : app.panels().entries()) e.open = e.panel.get() == panel;
  app.openFile(fs::path(TACHYON_GENERATED_FIXTURE_DIR) / "tiny.tachyon");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (!app.document()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  app.selection().setSelectedFunction(symbols::FunctionKey{ModuleIdx(0), 0x1200});
  const auto ready = [&] {
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    return panel->view() && !panel->pending() && p.total > 0 && p.done == p.total &&
           panel->requestedSpec().symbolGeneration == app.symbolGeneration();
  };
  while (!ready()) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    app.frame(0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::string text(1 << 20, 'a');
  text += "\nint x;\n";
  auto parsed = source::SourceText::parse(std::move(text));
  ASSERT_TRUE(parsed.ok());
  auto view = std::make_shared<source::SourceView>();
  view->kind = source::ViewKind::Source;
  view->key = symbols::FunctionKey{ModuleIdx(0), 0x1200};
  view->resolved = true;
  view->file = *parsed;
  view->lines.assign(static_cast<size_t>((*parsed)->lineCount()) + 1, source::LineTime{});
  panel->injectView(view);
  panel->setSearchText("a");
  ASSERT_GT(panel->searchMatches().size(), 0u);
  ImGui::SetWindowSize(SourcePanel::kName, ImVec2(1000, 600));
  for (int i = 0; i < 3; ++i) app.frame(0);
  ImGuiWindow* child = nullptr;
  for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    if (w->Name && std::string(w->Name).find("##sourcecode") != std::string::npos) child = w;
  ASSERT_NE(child, nullptr);
  ImGui::SetScrollX(child, 1e7f);
  for (int i = 0; i < 5; ++i) app.frame(0);
  ASSERT_GT(child->Scroll.x, 1e5f) << "the view must really be scrolled to the far right";
  double best = 1e30;
  for (int i = 0; i < 10; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    app.frame(0);
    best = std::min(best, msSince(t0));
  }
  std::printf(
      "[source perf] 1 MB line, dense matches, scrollX %.0f px: frame best %.3f ms (budget %.0f ms)\n",
      child->Scroll.x, best, kFrameBudgetMs);
  EXPECT_LT(best, kFrameBudgetMs * 2.0);
}
