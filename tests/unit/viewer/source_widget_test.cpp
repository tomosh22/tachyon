// WP-45: the Source panel driven headless (Null renderer, real ImGui frames, injected input) on traces whose
// only module is the profiler testbed, so symbols (DWARF through libdw), source lines and code bytes are all
// real: per-line totals equal linesForFunction, the hottest-line jump, search, mixed-mode instruction rows
// equal instructionsForFunction, the disassembly fallback when the file is missing, "Locate file..." deriving
// and persisting a remap rule, newest-wins under rapid selection and the lifetime of in-flight work when the
// document closes. Linux only (the DWARF provider); the Windows twin lives in tests/windows/viewer.
#include "source_test_util.h"

#include "analysis/query.h"
#include "disasm/disassembler.h"
#include "symbols/dwarf/dwarf_provider.h"
#include "viewer/builtin_panels.h"
#include "viewer/document.h"
#include "viewer/paths.h"
#include "viewer/source/source_panel.h"

#if defined(TACHYON_TESTBED_EXE) && TACHYON_HAVE_LIBDW

#include "symbols/dwarf/elf_reader.h"

#include <algorithm>
#include <map>
#include <set>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::source_test;
using symbols::FunctionKey;
using Mode = SourcePanel::Mode;

namespace {

// ---- the testbed binary --------------------------------------------------------------------------------

struct Fn {
  uint64_t rva = 0, size = 0;
  std::vector<disasm::Instruction> code;
};

struct Testbed {
  fs::path exe;
  fs::path scenarios = fs::path(TACHYON_TESTBED_SOURCE_DIR) / "scenarios.cpp";
  ModuleSpec module;
  uint64_t bias = 0;
  std::map<std::string, std::pair<uint64_t, uint64_t>> symbols;  // raw name -> {rva, size}
  bool ok = false;

  explicit Testbed(fs::path image = TACHYON_TESTBED_EXE) : exe(std::move(image)) {
    auto elf = symbols::ElfFile::open(exe);
    if (!elf.ok()) return;
    bias = elf->loadBias();
    module.path = exe.string();
    if (auto id = elf->buildId()) {
      module.idKind = trace::kModuleIdGnuBuildId;
      module.id = *id;
    }
    auto syms = elf->readCodeSymbols();
    if (!syms.ok()) return;
    for (const auto& s : *syms) symbols[s.name] = {s.value - bias, s.size};
    ok = true;
  }

  Fn function(const std::string& name) const {
    Fn f;
    const auto it = symbols.find(name);
    if (it == symbols.end()) return f;
    f.rva = it->second.first;
    f.size = it->second.second;
    auto bytes = disasm::readImageBytes(exe, f.rva, f.size);
    if (bytes.ok()) f.code = disasm::disassemble(*bytes, f.rva);
    return f;
  }
};

const Testbed& testbed() {
  static const Testbed t;
  return t;
}

// A function with a direct call in the middle and a known source file, found through a standalone provider.
struct CallFn {
  Fn fn;
  size_t callIndex = 0;  // code[callIndex] is a call; code[callIndex + 1] is the return address
  bool found = false;
  bool lineDiffers = false;  // the instruction after the call is on another source line
  int score = -1;
  bool calleeNamed = false;  // the call is direct and its target is a function with a symbol
};

CallFn findCallFunction() {
  CallFn out;
  const Testbed& tb = testbed();
  symbols::SymbolStringTable strings;
  symbols::DwarfSymbolProvider provider(strings, symbols::makeLibdwOpener());
  model::Module m;
  m.path = tb.exe.string();
  m.id.kind = tb.module.idKind;
  m.id.len = static_cast<uint8_t>(tb.module.id.size());
  std::copy(tb.module.id.begin(), tb.module.id.end(), m.id.bytes.begin());
  if (!provider.load(m, tb.exe).ok()) return out;
  std::vector<std::pair<uint64_t, std::string>> byRva;
  for (const auto& [name, v] : tb.symbols) byRva.push_back({v.first, name});
  std::sort(byRva.begin(), byRva.end());
  for (const auto& [rva, name] : byRva) {
    const auto size = tb.symbols.at(name).second;
    if (size < 40 || size > 3000) continue;
    const symbols::ResolvedAddress ra = provider.resolve(rva);
    if (!ra.resolved || ra.functionStartRva != rva || !ra.file.valid()) continue;
    Fn f = tb.function(name);
    for (size_t i = 0; i + 2 < f.code.size(); ++i) {
      if (f.code[i].isCall && f.code[i].valid && f.code[i + 1].valid) {
        // Prefer a direct call to a function the symbols know (its text must carry the callee's name) and a
        // call whose next instruction is on another source line (the verifier's AfterCallLineMismatch shape).
        const bool named = f.code[i].hasTarget && provider.resolve(f.code[i].targetRva).resolved;
        const symbols::ResolvedAddress a = provider.resolve(f.code[i].rva);
        const symbols::ResolvedAddress b = provider.resolve(f.code[i + 1].rva);
        const bool differs = a.resolved && b.resolved && a.inlines.empty() && b.inlines.empty() &&
                             a.file == b.file && a.line != 0 && b.line != 0 && a.line != b.line;
        const int score = (named ? 1 : 0) + (differs ? 2 : 0);
        if (out.found && score <= out.score) continue;
        out.fn = f;
        out.callIndex = i;
        out.found = true;
        out.calleeNamed = named;
        out.lineDiffers = differs;
        out.score = score;
        if (score == 3) return out;
      }
    }
  }
  return out;
}

// ---- providers ---------------------------------------------------------------------------------------

// Wraps a provider and moves every file it reports below `prefix`: the debug info then names files that do
// not exist on this machine (an engine built elsewhere).
class RewritingProvider final : public symbols::ISymbolProvider {
 public:
  RewritingProvider(std::unique_ptr<symbols::ISymbolProvider> inner, symbols::SymbolStringTable& strings,
                    std::string prefix)
      : m_inner(std::move(inner)), m_strings(strings), m_prefix(std::move(prefix)) {}
  Result<void> load(const model::Module& m, const fs::path& file) override { return m_inner->load(m, file); }
  symbols::ResolvedAddress resolve(uint64_t rva) override {
    symbols::ResolvedAddress r = m_inner->resolve(rva);
    r.file = rewrite(r.file);
    r.callSiteFile = rewrite(r.callSiteFile);
    for (symbols::InlineFrame& f : r.inlines) f.file = rewrite(f.file);
    return r;
  }
  Result<std::vector<symbols::LineRange>> linesForFunction(uint64_t start) override {
    auto lines = m_inner->linesForFunction(start);
    if (!lines.ok()) return lines;
    for (symbols::LineRange& l : *lines) l.file = rewrite(l.file);
    return lines;
  }
  Result<symbols::FunctionBounds> functionBounds(uint64_t rva) override {
    return m_inner->functionBounds(rva);
  }

 private:
  StringIdx rewrite(StringIdx f) {
    if (!f.valid()) return f;
    const std::string_view s = m_strings.get(f);
    if (s.empty()) return f;
    return m_strings.intern(m_prefix + (s.front() == '/' ? "" : "/") + std::string(s));
  }
  std::unique_ptr<symbols::ISymbolProvider> m_inner;
  symbols::SymbolStringTable& m_strings;
  std::string m_prefix;
};

constexpr const char* kMissingRoot = "/nonexistent-build-root";

struct SymbolMode {
  bool rewriteFiles = false;  // the debug info names files below kMissingRoot
  bool noSymbols = false;     // the symbol locator finds nothing: every key stays {module, rva}
  fs::path exe;               // the module image; empty = the testbed
};

// ---- harness -----------------------------------------------------------------------------------------

class Harness {
 public:
  explicit Harness(SymbolMode mode = {}, bool persist = false, fs::path config = {})
      : app(makeNullRenderer(), options(mode, persist, std::move(config))) {
    std::string error;
    EXPECT_TRUE(app.init(error)) << error;
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.ConfigInputTrickleEventQueue = false;
    PanelEntry* entry = app.panels().find(SourcePanel::kName);
    EXPECT_NE(entry, nullptr);
    panel = entry ? dynamic_cast<SourcePanel*>(entry->panel.get()) : nullptr;
    EXPECT_NE(panel, nullptr);
    if (panel) panel->disableThrottle();
    for (PanelEntry& e : app.panels().entries())
      e.open = e.panel.get() == panel || std::string(e.panel->name()) == "Trace Info";
  }

  static AppOptions options(SymbolMode mode, bool persist, fs::path config) {
    AppOptions o = headlessOptions();
    if (persist) {
      o.persist = true;
      o.configPath = std::move(config);
    }
    const fs::path exe = mode.exe.empty() ? testbed().exe : mode.exe;
    o.configureSymbols = [mode, exe](symbols::SymbolService& service) {
      service.setLocate([mode, exe](const model::Module&) -> Result<fs::path> {
        if (mode.noSymbols) return makeError(ErrorCode::NotFound, "no symbols (test)");
        return exe;
      });
      if (mode.rewriteFiles) {
        symbols::SymbolStringTable* strings = &service.stringSink();
        service.setProviderFactory(
            [strings](const model::Module&, const fs::path&) -> std::unique_ptr<symbols::ISymbolProvider> {
              return std::make_unique<RewritingProvider>(
                  std::make_unique<symbols::DwarfSymbolProvider>(*strings, symbols::makeLibdwOpener()),
                  *strings, kMissingRoot);
            });
      }
    };
    return o;
  }

  bool until(const std::function<bool()>& pred, int seconds = 60) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
  void frames(int n = 1) {
    for (int i = 0; i < n; ++i) app.frame(0);
  }

  bool symbolsDone() {
    if (!app.document() || !app.document()->symbols()) return false;
    const symbols::SymbolProgress p = app.document()->symbols()->progress();
    return p.total > 0 && p.done == p.total;
  }
  // The panel has delivered the view for the bus's current selection and symbol generation.
  bool settled() {
    const auto& key = app.selection().state().selectedFunction;
    if (!key) return !panel->pending();
    return panel->view() && !panel->pending() && panel->shownSpec().key == key &&
           panel->shownSpec() == panel->requestedSpec() &&
           panel->requestedSpec().symbolGeneration == app.symbolGeneration();
  }
  // Symbols done and a symbol generation that stays put for 100 ms, then the panel.
  void stable() {
    for (;;) {
      ASSERT_TRUE(until([&] { return symbolsDone(); }));
      const uint64_t generation = app.symbolGeneration();
      const auto until100 = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
      while (std::chrono::steady_clock::now() < until100) {
        frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
      if (generation == app.symbolGeneration()) break;
    }
    ASSERT_TRUE(until([&] { return settled(); }));
  }
  void settle() {
    frames(2);
    ASSERT_TRUE(until([&] { return settled(); }));
    frames(2);
  }
  void open(const fs::path& trace) {
    app.openFile(trace);
    ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
    stable();
  }
  void select(const FunctionKey& key) {
    app.selection().setSelectedFunction(key);
    settle();
  }

  const model::TraceModel& model() { return app.document()->model(); }
  std::unique_ptr<analysis::QueryContext> refContext() {
    return std::make_unique<analysis::QueryContext>(model(), *app.document()->symbols());
  }
  void key(ImGuiKey k, bool shift = false, bool ctrl = false) {
    ImGuiIO& io = ImGui::GetIO();
    if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(k, true);
    frames(1);
    io.AddKeyEvent(k, false);
    if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
    frames(2);
  }
  void click(float x, float y) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    frames(1);
    io.AddMouseButtonEvent(0, true);
    frames(1);
    io.AddMouseButtonEvent(0, false);
    frames(2);
  }
  // Window layout for deterministic clicks.
  void layout() {
    frames(2);
    ImGui::SetWindowPos(SourcePanel::kName, ImVec2(20, 40));
    ImGui::SetWindowSize(SourcePanel::kName, ImVec2(900, 500));
    frames(3);
  }

  App app;
  SourcePanel* panel = nullptr;
};

// A trace with samples spread over the hot functions of the spin scenario.
struct SpinTrace {
  TempTree tree{"spin"};
  fs::path path;
  Fn hotA, hotB, hotC;
  std::vector<size_t> hotAIndices;  // indices into hotA.code that carry samples

  SpinTrace() {
    const Testbed& tb = testbed();
    hotA = tb.function("_Z5hot_amm");
    hotB = tb.function("_Z5hot_bmm");
    hotC = tb.function("_Z5hot_cmm");
    if (hotA.code.empty()) {
      // Names are mangled by the compiler: find them by their demangled prefix.
      for (const auto& [name, v] : tb.symbols) {
        Fn* target = name.find("hot_a") != std::string::npos   ? &hotA
                     : name.find("hot_b") != std::string::npos ? &hotB
                     : name.find("hot_c") != std::string::npos ? &hotC
                                                               : nullptr;
        if (target != nullptr && target->code.empty()) *target = tb.function(name);
      }
    }
    path = tree.root() / "spin.tachyon";
    if (hotA.code.size() < 12) return;
    const size_t n = hotA.code.size();
    hotAIndices = {1, 3, n / 3, n / 3 + 1, n / 2, (2 * n) / 3, n - 3};
    std::vector<StackSpec> stacks;
    uint32_t count = 90;
    for (const size_t i : hotAIndices) {
      stacks.push_back({{hotA.code[i].rva}, count});
      count = std::max<uint32_t>(10, count - 12);
    }
    stacks.push_back({{hotB.code[2].rva}, 25});
    stacks.push_back({{hotC.code[2].rva}, 15});
    // hot_c running below hot_a: hot_a is a non-leaf frame at its return address.
    stacks.push_back({{hotC.code[3].rva, hotA.code[hotA.code.size() / 2].rva}, 20});
    writeModuleTrace(path, tb.module, stacks);
  }
  bool ok() const { return !hotAIndices.empty() && !hotB.code.empty() && !hotC.code.empty(); }
  static FunctionKey key(const Fn& f) { return FunctionKey{ModuleIdx(0), f.rva}; }
};

std::string readFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------

TEST(SourceWidget, TestbedIsAvailable) {
  ASSERT_TRUE(testbed().ok);
  SpinTrace t;
  ASSERT_TRUE(t.ok()) << "hot_a / hot_b / hot_c not found in the testbed's symbol table";
}

TEST(SourceWidget, SourceModeLineTotalsEqualLinesForFunction) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->kind, source::ViewKind::Source) << view->sourceProblem << view->message;
  EXPECT_TRUE(view->resolved);
  EXPECT_EQ(fs::weakly_canonical(pathFromUtf8(view->localPath)), fs::weakly_canonical(testbed().scenarios));
  ASSERT_NE(view->file, nullptr);
  EXPECT_EQ(view->file->byteSize(), readFile(testbed().scenarios).size());

  auto ref = h.refContext();
  auto aggs = analysis::linesForFunction(*ref, {}, SpinTrace::key(t.hotA));
  ASSERT_TRUE(aggs.ok()) << aggs.error().toString();
  ASSERT_FALSE(aggs->empty());
  uint64_t total = 0, best = 0;
  uint32_t bestLine = 0;
  std::set<uint32_t> expectedLines;
  for (const analysis::LineAgg& a : *aggs) {
    total += a.ticks;
    if (a.line == 0 || a.line > view->file->lineCount()) continue;
    expectedLines.insert(a.line);
    EXPECT_EQ(view->lines[a.line].ticks, a.ticks) << "line " << a.line;
    EXPECT_EQ(view->lines[a.line].running, a.runningTicks);
    EXPECT_EQ(view->lines[a.line].ready, a.readyTicks);
    EXPECT_EQ(view->lines[a.line].waiting, a.waitingTicks);
    if (a.ticks > best) {
      best = a.ticks;
      bestLine = a.line;
    }
  }
  EXPECT_EQ(view->total.ticks, total);
  EXPECT_EQ(view->hottestLine, bestLine);
  uint64_t sum = view->unknownLineTicks + view->pastEndTicks;
  for (uint32_t l = 1; l <= view->file->lineCount(); ++l) {
    sum += view->lines[l].ticks;
    if (view->lines[l].ticks != 0) EXPECT_TRUE(expectedLines.count(l)) << "unexpected time on line " << l;
  }
  EXPECT_EQ(sum, total);
  // The function's own lines are known: first <= hottest <= last.
  EXPECT_LE(view->firstLine, view->hottestLine);
  EXPECT_GE(view->lastLine, view->hottestLine);

  // The panel starts at the hottest line.
  EXPECT_EQ(h.panel->effectiveMode(), Mode::Source);
  EXPECT_EQ(h.panel->selectedLine(), view->hottestLine);
  // Rows are the file's lines.
  EXPECT_EQ(h.panel->rows().size(), view->file->lineCount());
}

TEST(SourceWidget, JumpToHottestLineSelectsAndScrollsToIt) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  h.layout();
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_NE(view->hottestLine, 0u);
  // Scroll away (to the first line), then jump back.
  h.panel->selectRow(0);
  h.panel->jumpToHottest();
  h.frames(4);
  EXPECT_EQ(h.panel->selectedLine(), view->hottestLine);
  const size_t row = h.panel->selectedRow();
  ASSERT_NE(row, SIZE_MAX);
  ASSERT_NE(h.panel->firstVisibleRow(), SIZE_MAX);
  EXPECT_LE(h.panel->firstVisibleRow(), row);
  EXPECT_GE(h.panel->lastVisibleRow(), row);
  // Virtualized: far fewer rows drawn than the file has.
  EXPECT_LT(h.panel->drawnRows().size(), h.panel->rows().size());
  EXPECT_GT(h.panel->drawnRows().size(), 5u);
}

TEST(SourceWidget, MixedModeInstructionRowsEqualInstructionsForFunction) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->kind, source::ViewKind::Source);
  ASSERT_TRUE(view->haveInstructions) << view->instructionsProblem;
  EXPECT_EQ(view->instructions.size(), t.hotA.code.size());

  h.panel->setMode(Mode::Mixed);
  h.panel->expandAll();
  h.frames(3);
  EXPECT_EQ(h.panel->effectiveMode(), Mode::Mixed);

  auto ref = h.refContext();
  auto aggs = analysis::instructionsForFunction(*ref, {}, SpinTrace::key(t.hotA));
  ASSERT_TRUE(aggs.ok());
  ASSERT_FALSE(aggs->empty());
  std::map<uint64_t, uint64_t> expected;
  for (const analysis::InstructionAgg& a : *aggs) expected[a.rva] = a.ticks;

  // Every instruction row of the expanded view, with its time, keyed by address.
  std::map<uint64_t, uint64_t> shown;
  size_t instrRows = 0;
  uint32_t previousLine = 0;
  for (const SourcePanel::Row& row : h.panel->rows()) {
    EXPECT_GE(row.line, previousLine);  // instruction rows sit below their own line, in line order
    previousLine = row.line;
    if (row.instr < 0) continue;
    const source::InstructionRow& in = view->instructions[static_cast<size_t>(row.instr)];
    EXPECT_EQ(in.groupLine, row.line);
    ++instrRows;
    if (in.ticks != 0) shown[in.rva] += in.ticks;
  }
  // Instructions whose line is outside the file are not listed under a line; here all are inside.
  size_t mapped = 0;
  for (const source::InstructionRow& in : view->instructions)
    if (in.groupLine >= 1 && in.groupLine <= view->file->lineCount()) ++mapped;
  EXPECT_EQ(instrRows, mapped);
  EXPECT_GT(instrRows, 0u);
  for (const auto& [rva, ticks] : expected) {
    // The sample addresses are instruction starts, so each aggregate lands on exactly one row.
    auto it = shown.find(rva);
    ASSERT_NE(it, shown.end()) << std::hex << rva;
    EXPECT_EQ(it->second, ticks) << std::hex << rva;
  }
  EXPECT_EQ(shown.size(), expected.size());

  // The decoded text equals a direct decode of the same bytes.
  for (size_t i = 0; i < view->instructions.size() && i < t.hotA.code.size(); ++i) {
    EXPECT_EQ(view->instructions[i].rva, t.hotA.code[i].rva);
    EXPECT_EQ(view->instructions[i].length, t.hotA.code[i].length);
    EXPECT_EQ(view->instructions[i].text.substr(0, 3), t.hotA.code[i].text.substr(0, 3)) << i;
  }

  // Collapsing hides them again.
  h.panel->collapseAll();
  EXPECT_EQ(h.panel->rows().size(), view->file->lineCount());
}

TEST(SourceWidget, ExpandingOneLineInsertsItsInstructionsBelowIt) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_TRUE(view->haveInstructions);
  h.panel->setMode(Mode::Mixed);
  h.layout();
  const uint32_t line = view->hottestLine;
  ASSERT_NE(line, 0u);
  const size_t instrCount = view->instructionCountOfLine(line);
  ASSERT_GT(instrCount, 0u);
  const size_t before = h.panel->rows().size();
  // Click the expander of the hottest line.
  h.panel->jumpToHottest();
  h.frames(4);
  const SourcePanel::DrawnRow* drawn = nullptr;
  for (const auto& d : h.panel->drawnRows())
    if (h.panel->rows()[d.row].line == line && h.panel->rows()[d.row].instr < 0) drawn = &d;
  ASSERT_NE(drawn, nullptr);
  h.click(drawn->fixedX + h.panel->charWidth() * 0.6f, (drawn->y0 + drawn->y1) * 0.5f);
  EXPECT_TRUE(h.panel->lineExpanded(line));
  EXPECT_EQ(h.panel->rows().size(), before + instrCount);
  const size_t at = h.panel->selectedRow();
  ASSERT_LT(at, h.panel->rows().size());
  EXPECT_EQ(h.panel->rows()[at].line, line);
  // The instruction rows follow the line row directly, in address order.
  uint64_t lastRva = 0;
  for (size_t r = at + 1; r < h.panel->rows().size() && h.panel->rows()[r].line == line; ++r) {
    ASSERT_GE(h.panel->rows()[r].instr, 0);
    const uint64_t rva = view->instructions[static_cast<size_t>(h.panel->rows()[r].instr)].rva;
    EXPECT_GT(rva, lastRva);
    lastRva = rva;
  }
  h.panel->toggleLine(line);
  EXPECT_EQ(h.panel->rows().size(), before);
}

TEST(SourceWidget, SamplesAfterACallAreLabelled) {
  const CallFn cf = findCallFunction();
  if (!cf.found) GTEST_SKIP() << "no function with a call found in the testbed";
  const Testbed& tb = testbed();
  // Any other function with code serves as the leaf below the call.
  Fn hot;
  for (const auto& [name, v] : tb.symbols) {
    if (v.first == cf.fn.rva || v.second < 8) continue;
    hot = tb.function(name);
    if (!hot.code.empty()) break;
  }
  ASSERT_FALSE(hot.code.empty());
  TempTree tree("aftercall");
  const uint64_t ret = cf.fn.code[cf.callIndex + 1].rva;
  std::vector<StackSpec> stacks;
  stacks.push_back({{hot.code[0].rva, ret}, 50});  // the call is on the stack: ret is a return address
  writeModuleTrace(tree.root() / "t.tachyon", tb.module, stacks);
  Harness h;
  h.open(tree.root() / "t.tachyon");
  h.select(FunctionKey{ModuleIdx(0), cf.fn.rva});
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_TRUE(view->haveInstructions) << view->instructionsProblem << view->message;
  const source::InstructionRow* row = nullptr;
  for (const source::InstructionRow& in : view->instructions)
    if (in.rva == ret) row = &in;
  ASSERT_NE(row, nullptr);
  EXPECT_TRUE(row->afterCall);
  EXPECT_EQ(row->ticks, 50 * kSampleStepTicks);
  // The call itself carries no time: the sample is attributed to the return address, not the call.
  EXPECT_EQ(view->instructions[cf.callIndex].ticks, 0u);
  EXPECT_TRUE(view->instructions[cf.callIndex].isCall);
  // Call targets are symbolized through the module's symbols: "call 0x1230 <callee>".
  if (cf.calleeNamed) {
    const std::string& text = view->instructions[cf.callIndex].text;
    std::printf("[source test] symbolized call: %s\n", text.c_str());
    EXPECT_NE(text.find(" <"), std::string::npos) << text;
    EXPECT_EQ(text.back(), '>') << text;
  }
  // And it equals the query.
  auto ref = h.refContext();
  auto aggs = analysis::instructionsForFunction(*ref, {}, FunctionKey{ModuleIdx(0), cf.fn.rva});
  ASSERT_TRUE(aggs.ok());
  uint64_t atRet = 0;
  for (const auto& a : *aggs)
    if (a.rva == ret) atRet = a.ticks;
  EXPECT_EQ(atRet, row->ticks);
  // Mixed mode lists the row under the CALL's line, where linesForFunction put the samples, so the per-line
  // sum of the listed rows equals the line total (the verifier's AfterCallLineMismatch case).
  const source::InstructionRow& call = view->instructions[cf.callIndex];
  std::printf("[source test] call line %u, next instruction line %u (differs: %d)\n", call.line, row->line,
              cf.lineDiffers ? 1 : 0);
  EXPECT_EQ(row->groupLine, call.line);
  if (cf.lineDiffers) EXPECT_NE(row->line, call.line);
  if (view->file && call.line >= 1 && call.line <= view->file->lineCount()) {
    uint64_t listed = 0;
    for (const uint32_t idx : view->instructionsOfLine(call.line)) listed += view->instructions[idx].ticks;
    EXPECT_EQ(listed, view->lines[call.line].ticks);
    EXPECT_EQ(view->lines[call.line].ticks, 50 * kSampleStepTicks);
    if (cf.lineDiffers) EXPECT_EQ(view->lines[row->line].ticks, 0u);
    const auto inLine = view->instructionsOfLine(call.line);
    EXPECT_NE(std::find(inLine.begin(), inLine.end(), static_cast<uint32_t>(cf.callIndex + 1)), inLine.end());
  }
  // Draw it in the disassembly view (fully, with the after-call tag path).
  h.panel->setMode(Mode::Disassembly);
  h.layout();
  h.frames(3);
  EXPECT_EQ(h.panel->effectiveMode(), Mode::Disassembly);
}

TEST(SourceWidget, SearchFindsLinesAndStepsForwardAndBackWithWrap) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_NE(view->file, nullptr);
  const std::string needle = "ITERATIONS";  // case-insensitive: the source says "iterations"
  std::vector<uint32_t> expected;
  for (uint32_t l = 1; l <= view->file->lineCount(); ++l) {
    std::string low(view->file->line(l));
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low.find("iterations") != std::string::npos) expected.push_back(l);
  }
  ASSERT_GE(expected.size(), 3u);

  h.panel->openSearch();
  EXPECT_TRUE(h.panel->searchOpen());
  h.panel->selectRow(0);
  h.panel->setSearchText(needle);
  EXPECT_EQ(h.panel->searchMatches(), expected);
  EXPECT_EQ(h.panel->selectedLine(), expected.front());  // the first match at or after the selection
  h.panel->searchNext();
  EXPECT_EQ(h.panel->selectedLine(), expected[1]);
  h.panel->searchNext();
  EXPECT_EQ(h.panel->selectedLine(), expected[2]);
  h.panel->searchPrevious();
  EXPECT_EQ(h.panel->selectedLine(), expected[1]);
  h.panel->searchPrevious();
  h.panel->searchPrevious();  // wraps to the last
  EXPECT_EQ(h.panel->selectedLine(), expected.back());
  h.panel->searchNext();  // wraps to the first
  EXPECT_EQ(h.panel->selectedLine(), expected.front());
  h.frames(4);
  // The matching line is on screen after the jump.
  h.layout();
  h.panel->searchNext();
  h.frames(4);
  ASSERT_NE(h.panel->selectedRow(), SIZE_MAX);
  EXPECT_LE(h.panel->firstVisibleRow(), h.panel->selectedRow());
  EXPECT_GE(h.panel->lastVisibleRow(), h.panel->selectedRow());
  // No match: nothing moves.
  const size_t row = h.panel->selectedRow();
  h.panel->setSearchText("qqqqnotthere");
  EXPECT_TRUE(h.panel->searchMatches().empty());
  h.panel->searchNext();
  EXPECT_EQ(h.panel->selectedRow(), row);
  h.panel->closeSearch();
  EXPECT_FALSE(h.panel->searchOpen());
}

TEST(SourceWidget, SearchInTheDisassemblySearchesInstructionText) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_TRUE(view->haveInstructions);
  h.panel->setMode(Mode::Disassembly);
  EXPECT_EQ(h.panel->effectiveMode(), Mode::Disassembly);
  EXPECT_EQ(h.panel->rows().size(), view->instructions.size());
  std::vector<uint32_t> expected;
  for (uint32_t i = 0; i < view->instructions.size(); ++i)
    if (view->instructions[i].text.find("mov") != std::string::npos ||
        view->instructions[i].text.find("MOV") != std::string::npos)
      expected.push_back(i);
  ASSERT_FALSE(expected.empty());
  h.panel->setSearchText("MOV");
  // Matches include instructions whose address text contains "mov": none ("mov" has non-hex letters).
  EXPECT_EQ(h.panel->searchMatches(), expected);
  ASSERT_LT(h.panel->selectedRow(), h.panel->rows().size());
  const uint32_t selected = static_cast<uint32_t>(h.panel->rows()[h.panel->selectedRow()].instr);
  EXPECT_TRUE(std::find(expected.begin(), expected.end(), selected) != expected.end());
  h.panel->searchNext();
  const uint32_t next = static_cast<uint32_t>(h.panel->rows()[h.panel->selectedRow()].instr);
  EXPECT_TRUE(std::find(expected.begin(), expected.end(), next) != expected.end());
  EXPECT_NE(next, selected);
}

TEST(SourceWidget, CtrlFSearchesInsideTheFocusedPanelAndStillOpensStatisticsElsewhere) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  h.layout();
  // The panel takes the keyboard focus when it is requested.
  h.app.selection().requestPanel(SourcePanel::kName);
  h.frames(4);
  EXPECT_GE(h.panel->focusRequests(), 1u);
  EXPECT_TRUE(h.panel->wantsFindShortcut());
  const uint64_t serialBefore = h.app.selection().state().panelRequest.serial;
  h.key(ImGuiKey_F, false, true);
  EXPECT_TRUE(h.panel->searchOpen());
  // Function Statistics was NOT requested by the focused Source panel's Ctrl+F.
  EXPECT_EQ(h.app.selection().state().panelRequest.serial, serialBefore);
  EXPECT_EQ(h.app.selection().state().panelRequest.name, std::string(SourcePanel::kName));
  // Esc closes the search.
  h.key(ImGuiKey_Escape);
  EXPECT_FALSE(h.panel->searchOpen());

  // Another window has the focus: Ctrl+F is the global shortcut again.
  ImGui::SetWindowFocus("Trace Info");
  h.frames(3);
  EXPECT_FALSE(h.panel->wantsFindShortcut());
  h.key(ImGuiKey_F, false, true);
  EXPECT_FALSE(h.panel->searchOpen());
  EXPECT_EQ(h.app.selection().state().panelRequest.name, std::string(kFunctionStatsPanelName));
  EXPECT_GT(h.app.selection().state().panelRequest.serial, serialBefore);
}

TEST(SourceWidget, MissingFileFallsBackToDisassemblyAndShowsTheReason) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  SymbolMode mode;
  mode.rewriteFiles = true;
  Harness h(mode);
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->kind, source::ViewKind::Disassembly);
  EXPECT_TRUE(view->resolved);
  EXPECT_NE(view->sourceProblem.find("not found"), std::string::npos) << view->sourceProblem;
  EXPECT_NE(view->debugPath.find(kMissingRoot), std::string::npos) << view->debugPath;
  EXPECT_EQ(view->file, nullptr);
  ASSERT_TRUE(view->haveInstructions);
  EXPECT_EQ(h.panel->effectiveMode(), Mode::Disassembly);
  EXPECT_EQ(h.panel->rows().size(), view->instructions.size());
  // The instructions are the function's real code with their times.
  auto ref = h.refContext();
  auto aggs = analysis::instructionsForFunction(*ref, {}, SpinTrace::key(t.hotA));
  ASSERT_TRUE(aggs.ok());
  std::map<uint64_t, uint64_t> expected, shown;
  for (const auto& a : *aggs) expected[a.rva] = a.ticks;
  for (const auto& in : view->instructions)
    if (in.ticks) shown[in.rva] = in.ticks;
  EXPECT_EQ(shown, expected);
  ASSERT_EQ(view->instructions.size(), t.hotA.code.size());
  EXPECT_EQ(view->instructions.front().rva, t.hotA.rva);
  // Starts at the hottest instruction.
  ASSERT_NE(view->hottestInstruction, UINT32_MAX);
  EXPECT_EQ(h.panel->selectedRow(), view->hottestInstruction);
  // Source and Mixed are not available: asking for them keeps the disassembly.
  h.panel->setMode(Mode::Source);
  EXPECT_EQ(h.panel->effectiveMode(), Mode::Disassembly);
  // The Locate button is shown once the view is drawn.
  h.layout();
  EXPECT_GT(h.panel->locateButton().x1, h.panel->locateButton().x0);
  // Clicking it opens the file picker.
  h.click((h.panel->locateButton().x0 + h.panel->locateButton().x1) * 0.5f,
          (h.panel->locateButton().y0 + h.panel->locateButton().y1) * 0.5f);
  EXPECT_TRUE(h.panel->picker().isOpen());
}

TEST(SourceWidget, LocateFileDerivesAndPersistsARemapRuleThenShowsTheSource) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  TempTree cfg("locate_cfg");
  SymbolMode mode;
  mode.rewriteFiles = true;
  const fs::path settingsPath = cfg.root() / "settings.json";
  std::vector<std::string> toasts;
  {
    Harness h(mode, /*persist=*/true, settingsPath);
    h.open(t.path);
    h.select(SpinTrace::key(t.hotA));
    ASSERT_EQ(h.panel->view()->kind, source::ViewKind::Disassembly);
    EXPECT_TRUE(h.app.settings().sourcePathMappings.empty());

    // The user picks the file in the checkout.
    h.panel->beginLocate(PanelContext{});  // no settings / document needed to open the picker
    EXPECT_TRUE(h.panel->picker().isOpen());
    h.panel->locateFile(testbed().scenarios, false);
    ASSERT_EQ(h.app.settings().sourcePathMappings.size(), 1u);
    const SourcePathMapping rule = h.app.settings().sourcePathMappings[0];
    EXPECT_EQ(rule.from, kMissingRoot);
    EXPECT_EQ(rule.to, "/");
    h.settle();
    h.frames(3);
    ASSERT_TRUE(
        h.until([&] { return h.panel->view() && h.panel->view()->kind == source::ViewKind::Source; }));
    const auto view = h.panel->view();
    EXPECT_EQ(fs::weakly_canonical(pathFromUtf8(view->localPath)), fs::weakly_canonical(testbed().scenarios));
    EXPECT_NE(view->hottestLine, 0u);
    EXPECT_EQ(h.panel->effectiveMode(), Mode::Source);
  }
  // Persisted: a fresh settings load has the rule.
  const Settings loaded = Settings::load(settingsPath);
  ASSERT_EQ(loaded.sourcePathMappings.size(), 1u);
  EXPECT_EQ(loaded.sourcePathMappings[0].from, kMissingRoot);

  // A new session with those settings shows the source straight away.
  {
    Harness h(mode, /*persist=*/true, settingsPath);
    h.open(t.path);
    h.select(SpinTrace::key(t.hotA));
    ASSERT_NE(h.panel->view(), nullptr);
    EXPECT_EQ(h.panel->view()->kind, source::ViewKind::Source);
  }
}

TEST(SourceWidget, LocateFileWithADirectoryAddsASearchRoot) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  SymbolMode mode;
  mode.rewriteFiles = true;
  Harness h(mode);
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  ASSERT_EQ(h.panel->view()->kind, source::ViewKind::Disassembly);
  // The debug path is /nonexistent-build-root/<abs path of scenarios.cpp>: a root "/" resolves its tail.
  h.panel->locateFile(fs::path("/"), true);
  ASSERT_EQ(h.app.settings().sourceRoots.size(), 1u);
  h.settle();
  ASSERT_TRUE(h.until([&] { return h.panel->view() && h.panel->view()->kind == source::ViewKind::Source; }));
}

TEST(SourceWidget, UnresolvedKeyGoesStraightToDisassembly) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  SymbolMode mode;
  mode.noSymbols = true;
  Harness h(mode);
  h.open(t.path);
  const uint64_t rva = t.hotA.code[3].rva;
  h.select(FunctionKey{ModuleIdx(0), rva});
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  EXPECT_FALSE(view->resolved);
  EXPECT_EQ(view->kind, source::ViewKind::Disassembly);
  EXPECT_NE(view->note.find("No symbols"), std::string::npos) << view->note;
  ASSERT_TRUE(view->haveInstructions) << view->instructionsProblem;
  EXPECT_EQ(view->startRva, rva);
  ASSERT_FALSE(view->instructions.empty());
  EXPECT_EQ(view->instructions.front().rva, rva);
  EXPECT_LE(view->sizeBytes, source::kUnresolvedWindowBytes);
  // The time at that exact address is on the first row.
  EXPECT_GT(view->instructions.front().ticks, 0u);
  EXPECT_EQ(h.panel->effectiveMode(), Mode::Disassembly);
  // Nothing to locate without symbols.
  h.layout();
  EXPECT_EQ(h.panel->locateButton().x1, 0.0f);
}

TEST(SourceWidget, KeysWithoutCodeShowAnExplanation) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(FunctionKey::scope(StringIdx(1)));
  ASSERT_NE(h.panel->view(), nullptr);
  EXPECT_EQ(h.panel->view()->kind, source::ViewKind::Message);
  EXPECT_NE(h.panel->view()->message.find("instrumentation scope"), std::string::npos);
  h.frames(3);
  h.select(analysis::kRootKey);
  EXPECT_EQ(h.panel->view()->kind, source::ViewKind::Message);
  h.select(analysis::kKernelKey);
  EXPECT_EQ(h.panel->view()->kind, source::ViewKind::Message);
  h.select(FunctionKey{ModuleIdx::invalid(), 0x7ffe12345678ull});
  EXPECT_EQ(h.panel->view()->kind, source::ViewKind::Message);
  EXPECT_NE(h.panel->view()->message.find("not inside any loaded module"), std::string::npos);
  EXPECT_TRUE(h.panel->rows().empty());
  // Back to a function.
  h.select(SpinTrace::key(t.hotB));
  EXPECT_EQ(h.panel->view()->kind, source::ViewKind::Source);
  // Deselecting forgets the view.
  h.app.selection().setSelectedFunction(std::nullopt);
  h.frames(3);
  EXPECT_EQ(h.panel->view(), nullptr);
}

TEST(SourceWidget, RapidSelectionOnlyEverShowsTheNewestRequest) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  const FunctionKey keys[] = {SpinTrace::key(t.hotA), SpinTrace::key(t.hotB), SpinTrace::key(t.hotC)};
  uint64_t delivered = h.panel->deliveredViews();
  for (int i = 0; i < 60; ++i) {
    h.app.selection().setSelectedFunction(keys[i % 3]);
    h.frames(1);
    if (h.panel->deliveredViews() != delivered) {
      delivered = h.panel->deliveredViews();
      // A delivered view always belongs to the request it is booked under: a superseded task's result is
      // never shown as the answer to a newer one.
      ASSERT_NE(h.panel->view(), nullptr);
      EXPECT_EQ(h.panel->view()->key, *h.panel->shownSpec().key);
    }
  }
  h.app.selection().setSelectedFunction(keys[2]);
  h.settle();
  ASSERT_NE(h.panel->view(), nullptr);
  EXPECT_EQ(h.panel->view()->key, keys[2]);
  EXPECT_EQ(h.panel->shownSpec().key, keys[2]);
  EXPECT_LE(h.panel->deliveredViews(), h.panel->issuedQueries());
  EXPECT_GE(h.panel->issuedQueries(), 3u);
  // A symbol re-resolution while a request may be in flight: the final view belongs to the new generation.
  h.app.selection().setSelectedFunction(keys[0]);
  h.frames(1);
  h.app.rerunSymbolResolution();
  h.stable();
  EXPECT_EQ(h.panel->view()->key, keys[0]);
  EXPECT_EQ(h.panel->shownSpec().symbolGeneration, h.app.symbolGeneration());
}

TEST(SourceWidget, ClosingTheDocumentWithWorkInFlightIsSafe) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  for (int round = 0; round < 3; ++round) {
    h.app.selection().setSelectedFunction(SpinTrace::key(round % 2 ? t.hotB : t.hotA));
    h.frames(1);  // the task is submitted, possibly still running
    h.app.commands().run("file.close");
    h.frames(3);
    EXPECT_EQ(h.panel->view(), nullptr);
    EXPECT_FALSE(h.panel->pending());
    h.open(t.path);
  }
  h.select(SpinTrace::key(t.hotA));
  EXPECT_NE(h.panel->view(), nullptr);
  // And a request for an old document generation is not delivered into a new document.
  h.app.selection().setSelectedFunction(SpinTrace::key(t.hotC));
  h.frames(1);
  h.app.commands().run("file.close");
  h.open(t.path);
  EXPECT_EQ(h.panel->view(),
            nullptr);  // nothing selected after the new document opened (the bus was cleared)
}

TEST(SourceWidget, TimeFilterRestrictsTheLineTimes) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  Harness h;
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  const uint64_t all = h.panel->view()->total.ticks;
  ASSERT_GT(all, 0u);
  const model::TraceModel& m = h.model();
  ViewFilter f;
  f.t0 = m.header.start_ts;
  f.t1 = m.header.start_ts + (m.header.end_ts - m.header.start_ts) / 3;
  h.app.selection().setTimeFilter(f);
  h.settle();
  const uint64_t part = h.panel->view()->total.ticks;
  EXPECT_LT(part, all);
  auto ref = h.refContext();
  analysis::Filter qf;
  qf.t0 = f.t0;
  qf.t1 = f.t1;
  auto aggs = analysis::linesForFunction(*ref, qf, SpinTrace::key(t.hotA));
  ASSERT_TRUE(aggs.ok());
  uint64_t sum = 0;
  for (const auto& a : *aggs) sum += a.ticks;
  EXPECT_EQ(part, sum);
}

TEST(SourceWidget, HorizontalScrollKeepsFixedColumnsAndDrawsOnlyVisibleColumns) {
  // A file with one very long line: drawing must not depend on its length and the window must scroll.
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  TempTree tree("wide");
  std::string wide;
  for (int i = 0; i < 40; ++i) wide += "int v" + std::to_string(i) + " = " + std::string(300, 'x') + ";\n";
  tree.write("scenarios.cpp", wide);
  SymbolMode mode;
  mode.rewriteFiles = true;
  Harness h(mode);
  h.app.settings().sourceRoots.push_back(pathToUtf8(tree.root()));
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  h.layout();
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->kind, source::ViewKind::Source) << view->sourceProblem;
  EXPECT_GE(view->file->maxColumns(), 300u);
  ASSERT_FALSE(h.panel->drawnRows().empty());
  const float fixedX = h.panel->drawnRows().front().fixedX;
  // Scroll the code child to the right and check the fixed columns stay where they are.
  ImGuiWindow* child = nullptr;
  for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    if (w->Name && std::string(w->Name).find("##sourcecode") != std::string::npos) child = w;
  ASSERT_NE(child, nullptr);
  ImGui::SetScrollX(child, 500.0f);
  h.frames(3);
  ASSERT_FALSE(h.panel->drawnRows().empty());
  EXPECT_NEAR(h.panel->drawnRows().front().fixedX, fixedX, 1.0f);  // the fixed columns do not scroll
  EXPECT_GT(child->Scroll.x, 100.0f);
}

TEST(SourceWidget, BinaryAndOversizedFilesFallBackToDisassemblyWithTheReason) {
  SpinTrace t;
  ASSERT_TRUE(t.ok());
  SymbolMode mode;
  mode.rewriteFiles = true;  // the debug info names kMissingRoot/<checkout>/scenarios.cpp
  TempTree tree("unusable");
  // The search root holds a file of that name which is not source code.
  std::string binary(2000, 'x');
  binary[1000] = '\0';
  const fs::path target = tree.write(pathToUtf8(testbed().scenarios).substr(1), binary);
  Harness h(mode);
  h.app.settings().sourceRoots.push_back(pathToUtf8(tree.root()));
  h.open(t.path);
  h.select(SpinTrace::key(t.hotA));
  auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->kind, source::ViewKind::Disassembly);
  EXPECT_NE(view->sourceProblem.find("binary"), std::string::npos) << view->sourceProblem;
  EXPECT_EQ(view->file, nullptr);
  EXPECT_TRUE(view->haveInstructions);

  // Too large: 17 MiB of text.
  {
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    const std::string chunk(1 << 20, 'a');
    for (int i = 0; i < 17; ++i) out << chunk << '\n';
  }
  h.panel->reload();
  h.settle();
  view = h.panel->view();
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->kind, source::ViewKind::Disassembly);
  EXPECT_NE(view->sourceProblem.find("16 MiB"), std::string::npos) << view->sourceProblem;

  // A usable file after the reload: the panel recovers.
  tree.write(pathToUtf8(testbed().scenarios).substr(1), "int a;\nint b;\n");
  h.panel->reload();
  h.settle();
  view = h.panel->view();
  ASSERT_NE(view, nullptr);
  EXPECT_EQ(view->kind, source::ViewKind::Source);
  ASSERT_NE(view->file, nullptr);
  EXPECT_EQ(view->file->lineCount(), 2u);
  // Every sample is on a line past the end of this (wrong) file: reported, not drawn.
  EXPECT_GT(view->pastEndTicks, 0u);
}

#ifdef TACHYON_SPLIT_FIXTURE
// A function split into a hot and a cold range (g++ -freorder-blocks-and-partition): the view shows the piece
// that holds the entry, says so, and keeps the cold piece's samples out of the rows.
TEST(SourceWidget, SplitFunctionShowsTheEntryPieceWithANote) {
  const fs::path image(TACHYON_SPLIT_FIXTURE);
  if (!fs::exists(image)) GTEST_SKIP() << "g++ not available: no split fixture";
  const Testbed tb(image);
  ASSERT_TRUE(tb.ok);
  const auto hot = tb.symbols.find("split_fn");
  const auto cold = tb.symbols.find("split_fn.cold");
  if (hot == tb.symbols.end() || cold == tb.symbols.end())
    GTEST_SKIP() << "the compiler did not split split_fn";
  const Fn hotFn = tb.function("split_fn");
  const Fn coldFn = tb.function("split_fn.cold");
  ASSERT_GE(hotFn.code.size(), 2u);
  ASSERT_GE(coldFn.code.size(), 1u);
  TempTree tree("split");
  writeModuleTrace(tree.root() / "t.tachyon", tb.module,
                   {{{hotFn.code[1].rva}, 30}, {{coldFn.code[0].rva}, 12}});
  SymbolMode mode;
  mode.exe = image;
  Harness h(mode);
  h.open(tree.root() / "t.tachyon");
  h.select(FunctionKey{ModuleIdx(0), hotFn.rva});
  const auto view = h.panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_TRUE(view->resolved) << view->message;
  EXPECT_NE(view->note.find("Split function"), std::string::npos) << view->note;
  ASSERT_TRUE(view->haveInstructions) << view->instructionsProblem;
  EXPECT_EQ(view->startRva, hotFn.rva);
  EXPECT_EQ(view->sizeBytes, hot->second.second);
  EXPECT_EQ(view->instructions.size(), hotFn.code.size());
  EXPECT_EQ(view->instructionTicks, 30 * kSampleStepTicks);
  EXPECT_EQ(view->otherTicks, 12 * kSampleStepTicks);  // the cold piece is not listed, its time is reported
  // The line times still cover both pieces.
  EXPECT_EQ(view->total.ticks, 42 * kSampleStepTicks);
}
#endif

#else

#include <gtest/gtest.h>
TEST(SourceWidget, NeedsTheTestbedAndLibdw) {
  GTEST_SKIP() << "built without the testbed or libdw";
}

#endif  // TACHYON_TESTBED_EXE && TACHYON_HAVE_LIBDW
