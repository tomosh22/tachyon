// WP-61: symbols for a trace captured on another machine. The viewer finds the symbol file of a module by its
// identity (GNU build-id here, PDB GUID+age on Windows), not by its path, so the module path the remote agent
// recorded ("/opt/game/bin/prog", which does not exist on the viewer's machine) must not matter. What the
// locator can and cannot do without the remote image:
//   * a `.build-id/xx/rest.debug` tree in a viewer search path (or a debug directory)      -> resolves
//   * a debug file named <image>.debug or <image> in a search path (the image name is known) -> resolves
//   * the .gnu_debuglink of the remote image                                                 -> cannot be
//   read
//     (the image is not here), so a debug file that only its debuglink name identifies is not found.
// The debug/stripped pair is made with objcopy (skipped when binutils is missing).
#include "dwarf_test_util.h"
#include "symbols/symbol_locator.h"

#include <unistd.h>

namespace tachyon::symbols {
namespace {

using namespace dwarftest;
namespace fs = std::filesystem;

class RemoteModule : public testing::Test {
 protected:
  void SetUp() override {
    if (!haveTool("objcopy")) GTEST_SKIP() << "objcopy (binutils) not available";
    m_root = fs::temp_directory_path() / ("tachyon-remote-syms-" + std::to_string(::getpid()));
    fs::remove_all(m_root);
    // The "remote machine": a stripped game binary and its debug file, linked by .gnu_debuglink.
    const fs::path remote = m_root / "remote";
    fs::create_directories(remote / "opt" / "game" / "bin");
    m_debug = remote / "symbols-of-prog.dbg";
    const fs::path full = remote / "full";
    fs::copy_file(fixture("dwarf_fixture_nopie"), full);
    ASSERT_EQ(run("objcopy --only-keep-debug '" + full.string() + "' '" + m_debug.string() + "'"), 0);
    m_remoteImage = remote / "opt" / "game" / "bin" / "prog";
    ASSERT_EQ(run("objcopy --strip-debug --add-gnu-debuglink='" + m_debug.string() + "' '" + full.string() +
                  "' '" + m_remoteImage.string() + "'"),
              0);
    // What the trace says about the module: its identity, and a path that only exists over there.
    m_module = moduleFor(m_remoteImage);
    ASSERT_EQ(m_module.id.kind, 2) << "no build-id";
    m_module.path = "/opt/game/bin/prog";
    ASSERT_FALSE(fs::exists(m_module.path));
    // The viewer's machine starts empty.
    m_viewer = m_root / "viewer";
    fs::create_directories(m_viewer / "cache");
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(m_root, ec);
  }

  static int run(const std::string& cmd) { return std::system((cmd + " >/dev/null 2>&1").c_str()); }

  SymbolLocatorConfig config(std::vector<fs::path> searchPaths = {}) const {
    SymbolLocatorConfig c;
    c.useSystemDebugDirs = false;
    c.cacheDir = m_viewer / "cache";
    c.searchPaths = std::move(searchPaths);
    return c;
  }

  std::string buildIdHex() const {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    for (uint8_t i = 0; i < m_module.id.len; ++i) {
      hex.push_back(kHex[m_module.id.bytes[i] >> 4]);
      hex.push_back(kHex[m_module.id.bytes[i] & 15]);
    }
    return hex;
  }

  void expectResolves(const fs::path& located) {
    Loaded l(located);
    ASSERT_TRUE(l.status.ok()) << l.status.error().toString();
    EXPECT_TRUE(l.provider->hasDwarf());
    const auto rva = symbolRva(fixture("dwarf_fixture_nopie"), "hot_c");
    ASSERT_TRUE(rva.has_value());
    const ResolvedAddress r = l.provider->resolve(*rva);
    ASSERT_TRUE(r.resolved);
    EXPECT_EQ(l.name(r.function), "hot_c");
    EXPECT_EQ(r.line, markerLine("fixture_main.cpp", "hot_c"));
  }

  fs::path m_root, m_viewer, m_remoteImage, m_debug;
  model::Module m_module;
};

TEST_F(RemoteModule, NothingIsFoundWithoutAnySymbolSource) {
  SymbolLocator loc(config());
  auto r = loc.locate(m_module);
  EXPECT_FALSE(r.ok()) << "the remote path means nothing here, and there is nothing else to search";
}

TEST_F(RemoteModule, BuildIdTreeInASearchPathResolvesIgnoringTheRemotePath) {
  const std::string hex = buildIdHex();
  const fs::path tree = m_viewer / "symbols" / ".build-id" / hex.substr(0, 2);
  fs::create_directories(tree);
  fs::copy_file(m_debug, tree / (hex.substr(2) + ".debug"));
  SymbolLocator loc(config({m_viewer / "symbols"}));
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), tree / (hex.substr(2) + ".debug"));
  expectResolves(r.value());
}

TEST_F(RemoteModule, BuildIdTreeInADebugDirectoryResolves) {
  const std::string hex = buildIdHex();
  const fs::path tree = m_viewer / "debugroot" / ".build-id" / hex.substr(0, 2);
  fs::create_directories(tree);
  fs::copy_file(m_debug, tree / (hex.substr(2) + ".debug"));
  SymbolLocatorConfig c = config();
  c.debugDirs = {m_viewer / "debugroot"};
  SymbolLocator loc(c);
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  expectResolves(r.value());
}

TEST_F(RemoteModule, DebugFileNamedAfterTheImageInASearchPathResolves) {
  fs::create_directories(m_viewer / "symbols");
  fs::copy_file(m_debug,
                m_viewer / "symbols" / "prog.debug");  // "<image>.debug": the name comes from the trace
  SymbolLocator loc(config({m_viewer / "symbols"}));
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value().filename(), "prog.debug");
  expectResolves(r.value());
}

TEST_F(RemoteModule, DebuglinkOnlyIdentifiesTheFileWhenTheImageIsHere) {
  // The debug file under the name the remote image's .gnu_debuglink gives it. Without the image there is no
  // way to know that name, so the locator cannot pick the file: this is the one thing that fails for a
  // remote trace (copy the binary next to it, or index the debug file by build-id).
  fs::create_directories(m_viewer / "symbols");
  fs::copy_file(m_debug, m_viewer / "symbols" / "symbols-of-prog.dbg");
  {
    SymbolLocator loc(config({m_viewer / "symbols"}));
    EXPECT_FALSE(loc.locate(m_module).ok());
  }
  // With the remote image copied to the same relative path on this machine, the debuglink works again.
  const fs::path mirrored = m_viewer / "mirror" / "opt" / "game" / "bin";
  fs::create_directories(mirrored);
  fs::copy_file(m_remoteImage, mirrored / "prog");
  fs::copy_file(m_debug, mirrored / "symbols-of-prog.dbg");
  model::Module copied = m_module;
  copied.path = (mirrored / "prog").string();
  SymbolLocator loc(config());
  auto r = loc.locate(copied);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r.value(), mirrored / "symbols-of-prog.dbg");
  expectResolves(r.value());
}

TEST_F(RemoteModule, LocalSymbolStoreByBuildIdResolves) {
  // A directory used as a symbol "server" (a local store): <store>/.build-id/xx/rest.debug. (The debuginfod
  // layout <server>/buildid/<id>/debuginfo is for HTTP servers and the download cache only.)
  const fs::path store = m_viewer / "store";
  const std::string hex = buildIdHex();
  fs::create_directories(store / ".build-id" / hex.substr(0, 2));
  fs::copy_file(m_debug, store / ".build-id" / hex.substr(0, 2) / (hex.substr(2) + ".debug"));
  SymbolLocatorConfig c = config();
  c.servers = {store.string()};
  SymbolLocator loc(c);
  auto r = loc.locate(m_module);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  expectResolves(r.value());
}

}  // namespace
}  // namespace tachyon::symbols
