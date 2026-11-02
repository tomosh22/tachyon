// WP-34 (Windows): disassemble hot_a of the testbed from its PE file with the bounds DIA reports, then check
// that synthetic sample hits on its instructions land inside those bounds in instructionsForFunction and
// linesForFunction. Runs against the real msdia140 and the testbed's own PDB (see tests/windows/symbols).
#include "aggregation_test_util.h"
#include "analysis/query.h"
#include "disasm/disassembler.h"
#include "etlconvert/pe_info.h"
#include "symbols/dia/dia_provider.h"
#include "trace/trace_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace tachyon {
namespace {

namespace fs = std::filesystem;
using analysis::FunctionKey;
using analysis::QueryContext;

uint32_t lineOf(const fs::path& file, const std::string& needle) {
  std::ifstream in(file);
  std::string text;
  uint32_t n = 0;
  while (std::getline(in, text)) {
    ++n;
    if (text.find(needle) != std::string::npos) return n;
  }
  return 0;
}

struct Testbed {
  fs::path exe{TACHYON_TESTBED_EXE};
  fs::path pdb;
  model::Module module;
  uint32_t sizeOfImage = 0;
  std::string why;
  bool ok = false;
};

Testbed loadTestbed() {
  Testbed t;
  auto pe = etlconvert::parsePeFile(t.exe);
  if (!pe.ok() || !pe.value().hasCodeView) {
    t.why = "testbed has no CodeView record";
    return t;
  }
  t.sizeOfImage = pe.value().sizeOfImage;
  t.pdb = pe.value().pdbPath;
  if (!fs::exists(t.pdb)) t.pdb = fs::path(t.exe).replace_extension(".pdb");
  if (!fs::exists(t.pdb)) {
    t.why = "no PDB for the testbed";
    return t;
  }
  t.module.path = t.exe.string();
  t.module.id.kind = trace::kModuleIdPdb;
  t.module.id.len = 16;
  std::memcpy(t.module.id.bytes.data(), pe.value().guid, 16);
  t.module.id.age = pe.value().age;
  t.ok = true;
  return t;
}

TEST(DisasmWindows, HotAInstructionsAndSampleHitsLandInsideItsBounds) {
  const Testbed tb = loadTestbed();
  ASSERT_TRUE(tb.ok) << tb.why;
  std::string err;
  auto opener = symbols::makeComDiaOpener(&err);
  ASSERT_NE(opener, nullptr) << err;

  // 1. The function's bounds from DIA (leaf functions have no .pdata entry: walk the PDB from .text).
  symbols::SymbolStringTable strings;
  symbols::DiaSymbolProvider provider(strings, opener);
  auto loaded = provider.load(tb.module, tb.pdb);
  ASSERT_TRUE(loaded.ok()) << loaded.error().toString();
  uint64_t start = 0, size = 0;
  for (uint32_t rva = 0x1000; rva < tb.sizeOfImage && size == 0;) {
    auto b = provider.functionBounds(rva);
    if (!b.ok() || b.value().size == 0) {
      ++rva;
      continue;
    }
    if (strings.get(b.value().name) == "hot_a") {
      start = b.value().startRva;
      size = b.value().size;
    }
    rva = static_cast<uint32_t>(std::max<uint64_t>(rva + 1, b.value().startRva + b.value().size));
  }
  ASSERT_GT(size, 0u) << "hot_a not found in the PDB";

  // 2. Its bytes from the PE file, disassembled with symbolized call targets.
  auto bytes = disasm::readModuleBytes(tb.module, start, static_cast<size_t>(size));
  ASSERT_TRUE(bytes.ok()) << bytes.error().toString();
  ASSERT_EQ(bytes->size(), size);
  const auto ins = disasm::disassemble(*bytes, start, 64, [&](uint64_t target) -> std::string {
    auto b = provider.functionBounds(target);
    return b.ok() ? std::string(strings.get(b.value().name)) : std::string();
  });
  ASSERT_FALSE(ins.empty());
  EXPECT_EQ(ins.front().rva, start);
  EXPECT_EQ(ins.back().rva + ins.back().length, start + size);
  std::set<uint64_t> boundaries;
  bool sawRet = false;
  for (const disasm::Instruction& i : ins) {
    EXPECT_TRUE(i.valid) << i.text << " at 0x" << std::hex << i.rva;
    EXPECT_GE(i.rva, start);
    EXPECT_LT(i.rva, start + size);
    boundaries.insert(i.rva);
    sawRet = sawRet || i.text == "ret";
  }
  EXPECT_TRUE(sawRet);

  // 3. A synthetic trace with one sample per instruction of hot_a (leaf), 100 ticks each.
  testutil::HandModel h;
  h.m.modules[0] = tb.module;
  h.state(0, 0, 10'000'000, model::ThreadState::Running);
  uint64_t ts = 0;
  for (const disasm::Instruction& i : ins) {
    h.sample(0, ts, h.stack({i.rva}));
    ts += 200;
  }
  h.svc = std::make_unique<symbols::SymbolService>(std::vector<std::string>{}, fs::path{});
  h.svc->setLocate([&](const model::Module&) -> Result<fs::path> { return tb.pdb; });
  symbols::registerDiaProvider(*h.svc, opener);
  h.svc->resolveAllAsync(h.m);
  h.svc->waitAll();
  ASSERT_EQ(h.svc->moduleState(ModuleIdx(0)), model::SymbolState::Resolved);

  QueryContext ctx(h.m, *h.svc);
  const FunctionKey key{ModuleIdx(0), start};
  auto hits = analysis::instructionsForFunction(ctx, {}, key);
  ASSERT_TRUE(hits.ok()) << hits.error().toString();
  ASSERT_EQ(hits->size(), ins.size());
  for (const analysis::InstructionAgg& a : *hits) {
    EXPECT_GE(a.rva, start);
    EXPECT_LT(a.rva, start + size);
    EXPECT_EQ(boundaries.count(a.rva), 1u) << std::hex << a.rva;
    EXPECT_EQ(a.ticks, 100u);
  }

  auto lines = analysis::linesForFunction(ctx, {}, key);
  ASSERT_TRUE(lines.ok()) << lines.error().toString();
  ASSERT_FALSE(lines->empty());
  const uint32_t def = lineOf(fs::path(TACHYON_TESTBED_SCENARIOS_CPP), "uint64_t hot_a(");
  ASSERT_GT(def, 0u);
  uint64_t total = 0;
  for (const analysis::LineAgg& l : *lines) {
    EXPECT_GE(l.line, def);  // the function's own file: the definition and its closing lines
    EXPECT_LE(l.line, def + 3);
    total += l.runningTicks;
  }
  EXPECT_EQ(total, 100u * ins.size());
}

}  // namespace
}  // namespace tachyon
