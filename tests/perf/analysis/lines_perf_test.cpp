// Perf budgets (WP-34): linesForFunction for the hottest function of `medium` < 100 ms cold (asserted at 2x),
// instructionsForFunction likewise, and disassembling a 64 KB function < 20 ms (asserted at 2x).
#include "analysis/query.h"
#include "analysis/thread_states.h"
#include "disasm/disassembler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

namespace tachyon::analysis {
namespace {

double seconds(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

TEST(LinesPerf, MediumLinesAndInstructionsForTheTopFunction) {
  const std::filesystem::path dir(TACHYON_GENERATED_FIXTURE_DIR);
  if (!std::filesystem::exists(dir / "medium.tachyon")) GTEST_SKIP() << "medium fixture not generated";
  auto loaded = model::TraceModel::load(dir / "medium.tachyon");
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  model::TraceModel m = std::move(*loaded);
  ASSERT_TRUE(buildThreadStates(m).ok());
  symbols::SymbolService svc{std::vector<std::string>{}, std::filesystem::path{}};
  ASSERT_TRUE(svc.addSymbolFileOverride(dir / "medium.symbols.json").ok());
  svc.resolveAllAsync(m);
  svc.waitAll();

  FunctionKey top;
  {
    QueryContext ctx(m, svc);
    auto rows = bottomUpFunctions(ctx, {});
    ASSERT_TRUE(rows.ok());
    ASSERT_FALSE(rows->empty());
    top = (*rows)[0].key;
  }
  double bestLines = 1e30, bestIns = 1e30;
  size_t lineRows = 0, insRows = 0;
  for (int i = 0; i < 5; ++i) {
    QueryContext ctx(m, svc);  // cold: no cached weights, no cached results
    auto t0 = std::chrono::steady_clock::now();
    auto lines = linesForFunction(ctx, {}, top);
    const double l = seconds(t0);
    ASSERT_TRUE(lines.ok()) << lines.error().toString();
    lineRows = lines->size();
    bestLines = std::min(bestLines, l);

    QueryContext ctx2(m, svc);
    t0 = std::chrono::steady_clock::now();
    auto ins = instructionsForFunction(ctx2, {}, top);
    const double s = seconds(t0);
    ASSERT_TRUE(ins.ok());
    insRows = ins->size();
    bestIns = std::min(bestIns, s);

    // Warm weights (shared with the aggregation queries): a second function is a cheap scan of the table.
    t0 = std::chrono::steady_clock::now();
    auto again = linesForFunction(ctx, {}, top);
    const double cached = seconds(t0);
    ASSERT_TRUE(again.ok());
    EXPECT_LT(cached, 0.01) << "memoized repeat should be near-instant";
  }
  std::printf(
      "[lines perf] medium linesForFunction (cold) best %.4f s (target 0.10 s, assert 0.20 s, %u hw "
      "threads); %zu rows\n",
      bestLines, std::thread::hardware_concurrency(), lineRows);
  std::printf("[lines perf] medium instructionsForFunction (cold) best %.4f s; %zu rows\n", bestIns, insRows);
  EXPECT_LT(bestLines, 0.2);
  EXPECT_LT(bestIns, 0.2);
}

TEST(LinesPerf, Disassemble64KbFunction) {
  // A plausible mix: prologue, rip-relative load, direct call, conditional jump, nop, ret.
  const std::vector<uint8_t> unit = {0x55, 0x48, 0x89, 0xE5, 0x48, 0x8B, 0x05, 0x10, 0x00,
                                     0x00, 0x00, 0xE8, 0x00, 0x01, 0x00, 0x00, 0x74, 0x05,
                                     0x90, 0x48, 0x83, 0xC4, 0x20, 0x5D, 0xC3};
  std::vector<uint8_t> code;
  while (code.size() < 64 * 1024) code.insert(code.end(), unit.begin(), unit.end());
  code.resize(64 * 1024);
  double best = 1e30;
  size_t count = 0;
  for (int i = 0; i < 10; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto ins =
        disasm::disassemble(code, 0x10000, 64, [](uint64_t rva) { return rva == 0x10105 ? "callee" : ""; });
    best = std::min(best, seconds(t0));
    count = ins.size();
  }
  std::printf(
      "[lines perf] disassemble 64 KB: best %.4f s (target 0.02 s, assert 0.04 s); %zu instructions\n", best,
      count);
  EXPECT_GT(count, 10000u);
  EXPECT_LT(best, 0.04);
}

}  // namespace
}  // namespace tachyon::analysis
