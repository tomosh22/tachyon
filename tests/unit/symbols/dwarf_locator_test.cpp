// ELF symbol location (WP-72): .gnu_debuglink with CRC verification and the .build-id tree, followed through
// the DWARF provider. The debug/stripped pair is made with objcopy at test time (skipped when binutils is
// missing).
#include "dwarf_test_util.h"
#include "symbols/symbol_keys.h"
#include "symbols/symbol_locator.h"

#include <unistd.h>

#include <iterator>

namespace tachyon::symbols {
namespace {

using namespace dwarftest;
namespace fs = std::filesystem;

class DwarfLocator : public testing::Test {
 protected:
  void SetUp() override {
    if (!haveTool("objcopy")) GTEST_SKIP() << "objcopy (binutils) not available";
    m_root = fs::temp_directory_path() / ("tachyon-dwarf-locator-" + std::to_string(::getpid()));
    fs::remove_all(m_root);
    fs::create_directories(m_root / "bin");
    m_image = m_root / "bin" / "prog";
    m_debug = m_root / "symbols-of-prog.dbg";
    fs::copy_file(fixture("dwarf_fixture_nopie"), m_root / "full", fs::copy_options::overwrite_existing);
    ASSERT_EQ(
        run("objcopy --only-keep-debug '" + (m_root / "full").string() + "' '" + m_debug.string() + "'"), 0);
    ASSERT_EQ(run("objcopy --strip-debug --add-gnu-debuglink='" + m_debug.string() + "' '" +
                  (m_root / "full").string() + "' '" + m_image.string() + "'"),
              0);
    m_module = moduleFor(m_image);
    m_module.path = m_image.string();
    ASSERT_EQ(m_module.id.kind, 2) << "the stripped copy lost its build-id";
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(m_root, ec);
  }

  static int run(const std::string& cmd) { return std::system((cmd + " >/dev/null 2>&1").c_str()); }

  SymbolLocatorConfig config() const {
    SymbolLocatorConfig c;
    c.useSystemDebugDirs = false;
    c.debugDirs = {m_root / "dbgroot"};
    c.cacheDir = m_root / "cache";
    return c;
  }

  // The located file must be a usable DWARF source for hot_c, with a source line.
  void expectResolvesWithLines(const fs::path& located) {
    Loaded l(located);
    ASSERT_TRUE(l.status.ok()) << l.status.error().toString();
    EXPECT_TRUE(l.provider->hasDwarf());
    const auto rva = symbolRva(fixture("dwarf_fixture_nopie"), "hot_c");
    const ResolvedAddress r = l.provider->resolve(*rva);
    ASSERT_TRUE(r.resolved);
    EXPECT_EQ(l.name(r.function), "hot_c");
    EXPECT_EQ(r.line, markerLine("fixture_main.cpp", "hot_c"));
  }

  fs::path m_root, m_image, m_debug;
  model::Module m_module;
};

TEST_F(DwarfLocator, StrippedBinaryWithoutAnyDebugFileIsItsOwnSymbolSource) {
  SymbolLocator loc(config());
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_image);  // last resort: the symbol table still names functions
  Loaded l(r.value());
  ASSERT_TRUE(l.status.ok());
  EXPECT_FALSE(l.provider->hasDwarf());
  EXPECT_EQ(l.name(l.provider->resolve(*symbolRva(m_image, "hot_c")).function), "hot_c");
}

TEST_F(DwarfLocator, DebuglinkFileNextToTheBinary) {
  fs::copy_file(m_debug, m_root / "bin" / "symbols-of-prog.dbg");
  SymbolLocator loc(config());
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_root / "bin" / "symbols-of-prog.dbg");
  expectResolvesWithLines(r.value());
}

TEST_F(DwarfLocator, DebuglinkFileInTheDotDebugSubdirectory) {
  fs::create_directories(m_root / "bin" / ".debug");
  fs::copy_file(m_debug, m_root / "bin" / ".debug" / "symbols-of-prog.dbg");
  SymbolLocator loc(config());
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), m_root / "bin" / ".debug" / "symbols-of-prog.dbg");
  expectResolvesWithLines(r.value());
}

TEST_F(DwarfLocator, DebuglinkFileUnderTheDebugDirectoryMirroringTheBinaryPath) {
  const fs::path mirrored = m_root / "dbgroot" / fs::path(m_root / "bin").relative_path();
  fs::create_directories(mirrored);
  fs::copy_file(m_debug, mirrored / "symbols-of-prog.dbg");
  SymbolLocator loc(config());
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), mirrored / "symbols-of-prog.dbg");
  expectResolvesWithLines(r.value());
}

TEST_F(DwarfLocator, DebuglinkWithAWrongCrcIsRejected) {
  fs::copy_file(m_debug, m_root / "bin" / "symbols-of-prog.dbg");
  {  // flip one byte inside a debug section: the file stays a valid ELF, the CRC no longer matches
    std::fstream f(m_root / "bin" / "symbols-of-prog.dbg", std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(-20, std::ios::end);
    char c = 0;
    f.read(&c, 1);
    f.seekp(-20, std::ios::end);
    c = static_cast<char>(c ^ 0x5A);
    f.write(&c, 1);
  }
  SymbolLocator loc(config());
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), m_image) << "a CRC mismatch must not be used; the stripped image is the fallback";
}

TEST_F(DwarfLocator, DebuglinkNamesWithPathComponentsAreNotFollowed) {
  // Patch the link name in place (same length, so the section layout stays valid) to "../evilevilevilevi",
  // and put a file with the right CRC where that path would lead. A plain-file-name policy must ignore it.
  const std::string original = "symbols-of-prog.dbg";
  const std::string evil = "../evilevilevilevi";
  ASSERT_EQ(original.size(), evil.size() + 1);
  std::string bytes;
  {
    std::ifstream in(m_image, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  const size_t at = bytes.find(original);
  ASSERT_NE(at, std::string::npos);
  bytes.replace(at, original.size(), evil + '\0');
  {
    std::ofstream out(m_image, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  fs::copy_file(m_debug, m_root / "evilevilevilevi");
  auto link = ElfFile::open(m_image).value().debugLink();
  ASSERT_TRUE(link.has_value());
  ASSERT_EQ(link->name, evil);

  SymbolLocator loc(config());
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), m_image);
}

TEST_F(DwarfLocator, BuildIdTreeIsUsedWhenTheBinaryIsGone) {
  const std::string hex = buildIdHex(m_module.id);
  ASSERT_GT(hex.size(), 4u);
  const fs::path dest = m_root / "dbgroot" / ".build-id" / hex.substr(0, 2) / (hex.substr(2) + ".debug");
  fs::create_directories(dest.parent_path());
  fs::copy_file(m_debug, dest);
  model::Module gone = m_module;
  gone.path = (m_root / "elsewhere" / "prog").string();  // the binary does not exist on this machine

  SymbolLocator loc(config());
  auto r = loc.locate(gone);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), dest);
  expectResolvesWithLines(r.value());
}

TEST_F(DwarfLocator, BuildIdTreeBeatsAStrippedBinary) {
  const std::string hex = buildIdHex(m_module.id);
  const fs::path dest = m_root / "dbgroot" / ".build-id" / hex.substr(0, 2) / (hex.substr(2) + ".debug");
  fs::create_directories(dest.parent_path());
  fs::copy_file(m_debug, dest);
  SymbolLocator loc(config());
  auto r = loc.locate(m_module);  // the stripped image exists too, but has no debuglink target on disk
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), dest);
}

TEST_F(DwarfLocator, ABinaryThatCarriesItsDebugInfoIsUsedDirectly) {
  model::Module full = moduleFor(m_root / "full");
  full.path = (m_root / "full").string();
  SymbolLocator loc(config());
  auto r = loc.locate(full);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value(), m_root / "full");
}

TEST_F(DwarfLocator, ServiceResolvesThroughTheLocatedDebuglinkFile) {
  fs::copy_file(m_debug, m_root / "bin" / "symbols-of-prog.dbg");
  const uint64_t hot = *symbolRva(m_image, "hot_c");
  model::TraceModel m;
  m.modules.push_back(m_module);
  m.frames = {{ModuleIdx(0), hot + 2}};
  m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
  m.nodes.push_back(model::CallstackNode{CallstackIdx(0), FrameIdx(0)});
  SymbolService svc({}, m_root / "cache");
  svc.configureLocator(config());
  registerDwarfProvider(svc);
  svc.resolveAllAsync(m);
  svc.waitAll(m);
  ASSERT_EQ(m.modules[0].state, model::SymbolState::Resolved) << svc.moduleError(ModuleIdx(0));
  const ResolvedAddress& r = svc.lookup(FrameIdx(0));
  ASSERT_TRUE(r.resolved);
  EXPECT_EQ(svc.symbolStrings().get(r.function), "hot_c");
  EXPECT_NE(r.line, 0u);
}

}  // namespace
}  // namespace tachyon::symbols
