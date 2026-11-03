// WP-45 on Windows: the Source panel on the real DIA provider. The testbed's hot_a is sampled in a small
// trace; the panel must render its source file (scenarios.cpp, found at the path recorded in the PDB, i.e.
// the checkout the test was built from), with line times that add up to the samples and with the code
// bytes of the testbed image disassembled for the mixed view.
#include "../../unit/viewer/source_test_util.h"

#include "etlconvert/pe_info.h"
#include "symbols/dia/dia_provider.h"
#include "viewer/document.h"
#include "viewer/paths.h"
#include "viewer/source/source_panel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::source_test;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

uint32_t lineContaining(const fs::path& file, const std::string& needle) {
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
  ModuleSpec module;
  uint32_t sizeOfImage = 0;
  bool ok = false;
  std::string why;
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
  t.module.idKind = trace::kModuleIdPdb;
  t.module.id.assign(pe.value().guid, pe.value().guid + 16);
  t.module.age = pe.value().age;
  t.ok = true;
  return t;
}

}  // namespace

TEST(SourceDia, HotARendersItsSourceFromTheCheckout) {
  const Testbed tb = loadTestbed();
  ASSERT_TRUE(tb.ok) << tb.why;

  // hot_a's start rva: walk the PDB's function extents like the DIA provider tests do.
  std::string err;
  auto opener = symbols::makeComDiaOpener(&err);
  ASSERT_NE(opener, nullptr) << err;
  symbols::SymbolStringTable strings;
  symbols::DiaSymbolProvider provider(strings, opener);
  model::Module m;
  m.path = tb.exe.string();
  m.id.kind = trace::kModuleIdPdb;
  m.id.len = 16;
  std::memcpy(m.id.bytes.data(), tb.module.id.data(), 16);
  m.id.age = tb.module.age;
  ASSERT_TRUE(provider.load(m, tb.pdb).ok());
  uint64_t hotA = 0, hotASize = 0;
  const uint32_t hi = std::min<uint32_t>(tb.sizeOfImage, 0x400000);
  for (uint32_t rva = 0x1000; rva < hi;) {
    auto b = provider.functionBounds(rva);
    if (!b.ok() || b.value().size == 0) {
      ++rva;
      continue;
    }
    if (strings.get(b.value().name) == "hot_a") {
      hotA = b.value().startRva;
      hotASize = b.value().size;
      break;
    }
    rva = static_cast<uint32_t>(std::max<uint64_t>(rva + 1, b.value().startRva + b.value().size));
  }
  ASSERT_NE(hotA, 0u) << "hot_a not found in the PDB";
  ASSERT_GT(hotASize, 16u);

  TempTree tree("dia");
  const fs::path tracePath = tree.root() / "hot_a.tachyon";
  const uint32_t count = 80;
  writeModuleTrace(tracePath, tb.module, {{{hotA + 4}, count}, {{hotA + hotASize / 2}, count / 2}});

  AppOptions options = headlessOptions("dia");
  const fs::path pdb = tb.pdb;
  options.configureSymbols = [pdb](symbols::SymbolService& service) {
    service.setLocate([pdb](const model::Module&) -> Result<fs::path> { return pdb; });
  };
  App app(makeNullRenderer(), options);
  std::string error;
  ASSERT_TRUE(app.init(error)) << error;
  ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  auto* panel = dynamic_cast<SourcePanel*>(app.panels().find(SourcePanel::kName)->panel.get());
  ASSERT_NE(panel, nullptr);
  panel->disableThrottle();
  for (PanelEntry& e : app.panels().entries()) e.open = e.panel.get() == panel;

  const auto until = [&](const std::function<bool()>& pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      app.frame(0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  };
  app.openFile(tracePath);
  ASSERT_TRUE(until([&] { return app.document() != nullptr && !app.loading(); }));
  ASSERT_TRUE(until([&] {
    const auto& svc = app.document()->symbols();
    if (!svc) return false;
    const symbols::SymbolProgress p = svc->progress();
    return p.total > 0 && p.done == p.total;
  }));
  const symbols::FunctionKey key{ModuleIdx(0), hotA};
  app.selection().setSelectedFunction(key);
  ASSERT_TRUE(until([&] {
    return panel->view() && !panel->pending() && panel->shownSpec().key == key &&
           panel->shownSpec() == panel->requestedSpec() &&
           panel->requestedSpec().symbolGeneration == app.symbolGeneration();
  }));

  const auto view = panel->view();
  ASSERT_NE(view, nullptr);
  ASSERT_EQ(view->kind, source::ViewKind::Source) << view->sourceProblem << view->message;
  EXPECT_EQ(lower(pathToUtf8(pathFromUtf8(view->localPath).filename())), "scenarios.cpp");
  ASSERT_NE(view->file, nullptr);
  const uint32_t definition = lineContaining(TACHYON_TESTBED_SCENARIOS_CPP, "uint64_t hot_a(");
  ASSERT_GT(definition, 0u);
  EXPECT_NE(view->hottestLine, 0u);
  EXPECT_GE(view->firstLine, definition);
  // Every sample is on a line of the file or in the "unknown" bucket; the total is the samples.
  EXPECT_EQ(view->total.ticks, (count + count / 2) * kSampleStepTicks);
  uint64_t sum = view->unknownLineTicks + view->pastEndTicks;
  for (uint32_t l = 1; l <= view->file->lineCount(); ++l) sum += view->lines[l].ticks;
  EXPECT_EQ(sum, view->total.ticks);
  // The code of the testbed image is disassembled, with the time on the sampled instructions.
  ASSERT_TRUE(view->haveInstructions) << view->instructionsProblem;
  EXPECT_EQ(view->instructionTicks + view->otherTicks, view->total.ticks);
  EXPECT_FALSE(view->instructions.empty());
  panel->setMode(SourcePanel::Mode::Mixed);
  panel->expandAll();
  for (int i = 0; i < 5; ++i) app.frame(0);
  EXPECT_EQ(panel->effectiveMode(), SourcePanel::Mode::Mixed);
  EXPECT_GT(panel->rows().size(), view->file->lineCount());
}
