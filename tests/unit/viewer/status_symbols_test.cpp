// WP-48: the Symbols panel and "Add symbol path". A trace copied next to no sidecar leaves every module
// NotFound; the panel lists them with the reason; adding the directory that holds the matching
// `.symbols.json` (or the file itself) resolves them again, persists the path in Settings and toasts.
#include "viewer/paths.h"
#include "viewer/status/symbol_paths.h"
#include "viewer/status/symbols_panel.h"

#include "status_test_util.h"

#include <fstream>

using namespace tachyon;
using namespace tachyon::viewer;
using namespace tachyon::viewer::status_test;

namespace {

// <root>/trace/tiny.tachyon without a sidecar, <root>/syms/tiny.symbols.json.
struct Layout {
  explicit Layout(const std::string& name) : root(name) {
    std::error_code ec;
    fs::create_directories(root.path() / "trace", ec);
    fs::create_directories(root.path() / "syms", ec);
    fs::copy_file(fixture("tiny.tachyon"), trace(), ec);
    EXPECT_FALSE(ec);
    fs::copy_file(fixture("tiny.symbols.json"), symsFile(), ec);
    EXPECT_FALSE(ec);
  }
  fs::path trace() const { return root.path() / "trace" / "tiny.tachyon"; }
  fs::path symsDir() const { return root.path() / "syms"; }
  fs::path symsFile() const { return symsDir() / "tiny.symbols.json"; }
  TempDir root;
};

SymbolsPanel* symbolsPanel(App& app) {
  PanelEntry* e = app.panels().find(SymbolsPanel::kName);
  return e ? dynamic_cast<SymbolsPanel*>(e->panel.get()) : nullptr;
}

}  // namespace

TEST(StatusSymbols, ListsUnresolvedModulesWithReason) {
  Layout layout("symbols_list");
  Harness h;
  ASSERT_TRUE(h.open(layout.trace()));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  SymbolsPanel* panel = symbolsPanel(h.app);
  ASSERT_NE(panel, nullptr);
  h.app.panels().find(SymbolsPanel::kName)->open = true;
  h.frames(3);
  ASSERT_EQ(panel->rows().size(), 2u);
  for (const ModuleSymbolRow& r : panel->rows()) {
    EXPECT_EQ(r.state, symbols::SymbolState::NotFound);
    EXPECT_FALSE(r.detail.empty());
  }
  EXPECT_EQ(panel->rows()[0].name, "game.exe");
  EXPECT_EQ(panel->rows()[1].name, "ntdll.dll");
  EXPECT_EQ(panel->summary().failed, 2u);
  EXPECT_EQ(panel->summary().resolved, 0u);
  EXPECT_NE(h.app.statusBar().symbolsText().find("(2 failed)"), std::string::npos);
}

TEST(StatusSymbols, AddingTheDirectoryResolvesTheModules) {
  Layout layout("symbols_add_dir");
  AppOptions options = headlessOptions("symbols_add_dir");
  options.persist = true;
  options.configPath = layout.root.path() / "config" / "settings.json";
  std::string persistedTo;
  {
    Harness h(std::move(options));
    ASSERT_TRUE(h.open(layout.trace()));
    ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
    SymbolsPanel* panel = symbolsPanel(h.app);
    h.app.panels().find(SymbolsPanel::kName)->open = true;
    h.frames(3);
    ASSERT_EQ(panel->rows().size(), 2u);
    const uint64_t generation = h.app.symbolGeneration();
    const symbols::SymbolService* before = h.app.document()->symbols().get();

    std::string error;
    ASSERT_TRUE(h.app.addSymbolPath(layout.symsDir(), error)) << error;
    EXPECT_TRUE(error.empty());
    EXPECT_GT(h.app.symbolGeneration(), generation);
    EXPECT_NE(h.app.document()->symbols().get(), before) << "resolution runs again with a fresh service";
    ASSERT_EQ(h.app.settings().symbolSearchPaths.size(), 1u);
    EXPECT_EQ(fs::path(h.app.settings().symbolSearchPaths[0]), fs::absolute(layout.symsDir()));

    ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
    h.frames(3);
    EXPECT_TRUE(panel->rows().empty());
    EXPECT_EQ(panel->summary().resolved, 2u);
    EXPECT_EQ(h.app.statusBar().symbolsText(), "Symbols: 2/2");
    const symbols::SymbolService& service = *h.app.document()->symbols();
    EXPECT_EQ(service.moduleState(ModuleIdx(0)), symbols::SymbolState::Resolved);
    EXPECT_EQ(service.moduleState(ModuleIdx(1)), symbols::SymbolState::Resolved);

    // Info toast about the added path.
    ASSERT_FALSE(h.app.toasts().empty());
    EXPECT_EQ(h.app.toasts().toasts().back().level, ToastLevel::Info);
    persistedTo = pathToUtf8(h.app.settingsPath());
  }
  // Persisted in settings.json: a new App sees the path and resolves on open.
  ASSERT_FALSE(persistedTo.empty());
  const Settings loaded = Settings::load(persistedTo);
  ASSERT_EQ(loaded.symbolSearchPaths.size(), 1u);
  AppOptions again = headlessOptions("symbols_add_dir2");
  again.configPath = persistedTo;
  Harness h2(std::move(again));
  ASSERT_TRUE(h2.open(layout.trace()));
  ASSERT_TRUE(h2.until([&] { return h2.symbolsDone(); }));
  EXPECT_EQ(h2.app.document()->symbols()->moduleState(ModuleIdx(0)), symbols::SymbolState::Resolved);
}

TEST(StatusSymbols, AddingTheSymbolsFileResolvesTheModules) {
  Layout layout("symbols_add_file");
  Harness h;
  ASSERT_TRUE(h.open(layout.trace()));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  h.frames(2);
  EXPECT_EQ(h.app.statusBar().summary().failed, 2u);
  std::string error;
  ASSERT_TRUE(h.app.addSymbolPath(layout.symsFile(), error)) << error;
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  h.frames(2);
  EXPECT_EQ(h.app.statusBar().summary().resolved, 2u);
}

TEST(StatusSymbols, BadPathIsRejectedWithAToast) {
  Layout layout("symbols_bad");
  Harness h;
  ASSERT_TRUE(h.open(layout.trace()));
  std::string error;
  EXPECT_FALSE(h.app.addSymbolPath(layout.root.path() / "no-such-dir", error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(h.app.settings().symbolSearchPaths.empty());
  ASSERT_FALSE(h.app.toasts().empty());
  EXPECT_EQ(h.app.toasts().toasts().back().level, ToastLevel::Error);

  // A .json file that is not a symbols file.
  const fs::path junk = layout.root.path() / "junk.json";
  std::ofstream(junk) << "{ \"hello\": 1 }";
  error.clear();
  EXPECT_FALSE(h.app.addSymbolPath(junk, error));
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(h.app.settings().symbolSearchPaths.empty());
}

TEST(StatusSymbols, ButtonOpensThePickerAndCopyInfoUsesTheClipboard) {
  Layout layout("symbols_ui");
  Harness h;
  ASSERT_TRUE(h.open(layout.trace()));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  SymbolsPanel* panel = symbolsPanel(h.app);
  h.app.panels().find(SymbolsPanel::kName)->open = true;
  h.frames(4);
  const SymbolsPanel::Rect b = panel->addButton();
  ASSERT_GT(b.x1, b.x0);
  EXPECT_FALSE(h.app.symbolPathPicker().isOpen());
  h.click((b.x0 + b.x1) * 0.5f, (b.y0 + b.y1) * 0.5f);
  EXPECT_TRUE(h.app.symbolPathPicker().isOpen());

  PanelContext context;
  context.document = h.app.document().get();
  ASSERT_FALSE(panel->rows().empty());
  panel->copyModuleInfo(context, panel->rows()[0]);
  const std::string clip = ImGui::GetClipboardText();
  EXPECT_NE(clip.find("game.exe"), std::string::npos);
  EXPECT_NE(clip.find("NotFound"), std::string::npos);
  EXPECT_NE(clip.find("Id kind:"), std::string::npos);
  EXPECT_NE(clip.find("Base: 0x"), std::string::npos);
}

TEST(StatusSymbols, RetryRunsTheResolutionAgain) {
  Layout layout("symbols_retry");
  Harness h;
  ASSERT_TRUE(h.open(layout.trace()));
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  h.frames(2);
  EXPECT_EQ(h.app.statusBar().summary().failed, 2u);
  // The sidecar appears after the first attempt (e.g. a build finished): Retry picks it up.
  std::error_code ec;
  fs::copy_file(layout.symsFile(), layout.trace().replace_extension(".symbols.json"), ec);
  ASSERT_FALSE(ec);
  const symbols::SymbolService* before = h.app.document()->symbols().get();
  h.app.commands().run("symbols.retry");
  EXPECT_NE(h.app.document()->symbols().get(), before);
  ASSERT_TRUE(h.until([&] { return h.symbolsDone(); }));
  h.frames(2);
  EXPECT_EQ(h.app.statusBar().summary().resolved, 2u);
}

TEST(StatusSymbolPaths, ApplyClassifiesDirectoriesAndFiles) {
  Layout layout("symbols_apply");
  symbols::SymbolService service({}, layout.root.path() / "cache");
  const fs::path binary = layout.root.path() / "some.pdb";
  std::ofstream(binary) << "x";
  const SymbolPathReport report = applySymbolSearchPaths(
      service, {pathToUtf8(layout.symsDir()), pathToUtf8(binary), pathToUtf8(layout.root.path() / "missing")},
      {}, layout.root.path() / "cache");
  EXPECT_EQ(report.overrides, 1u);    // tiny.symbols.json found by the directory scan
  EXPECT_EQ(report.searchPaths, 2u);  // the directory and the .pdb file
  ASSERT_EQ(report.problems.size(), 1u);
  EXPECT_NE(report.problems[0].find("missing"), std::string::npos);
  EXPECT_TRUE(validateSymbolPath(layout.symsDir()).ok());
  EXPECT_TRUE(validateSymbolPath(layout.symsFile()).ok());
  EXPECT_FALSE(validateSymbolPath(layout.root.path() / "missing").ok());
}
