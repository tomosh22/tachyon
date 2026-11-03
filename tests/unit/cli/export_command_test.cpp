// WP-63: `tachyon-cli export`. Offline: none of these tests starts an agent.
#include "../control/test_util.h"
#include "cli/cli.h"
#include "model/model.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using nlohmann::json;

struct CliResult {
  int code = -1;
  std::string out, err;
};

CliResult runCli(const std::vector<std::string>& args) {
  std::ostringstream out, err;
  CliResult r;
  r.code = cli::run(args, out, err);
  r.out = out.str();
  r.err = err.str();
  return r;
}

std::string fixture(const char* name) {
  return (fs::path(TACHYON_GENERATED_FIXTURE_DIR) / name).string();
}

json readJson(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return json::parse(in);
}

class ExportCommand : public ::testing::Test {
 protected:
  std::string tiny() const { return fixture("tiny.tachyon"); }
  std::string tinySymbols() const { return fixture("tiny.symbols.json"); }
  std::string out(const char* name) const { return (dir.path() / name).string(); }
  TempDir dir;
};

TEST_F(ExportCommand, UsageErrorsExitThree) {
  const std::vector<std::vector<std::string>> bad = {
      {"export"},
      {"export", tiny(), "--out", out("a.json")},                                   // no --format
      {"export", tiny(), "--format", "speedscope"},                                 // no --out
      {"export", tiny(), "--format", "pprof", "--out", out("a.json")},              // bad format
      {"export", tiny(), "extra", "--format", "chrome", "--out", out("a.json")},    // two positionals
      {"export", tiny(), "--format", "chrome", "--out", out("a.json"), "--bogus"},  // unknown option
      {"export", tiny(), "--format", "chrome", "--out", out("a.json"), "--threads", "x"},
      {"export", tiny(), "--format", "chrome", "--out", out("a.json"), "--threads", "1,,2"},
      {"export", tiny(), "--format", "chrome", "--out", out("a.json"), "--from-ms", "-1"},
      {"export", tiny(), "--format", "chrome", "--out", out("a.json"), "--from-ms", "5", "--to-ms", "5"},
      {"export", tiny(), "--format", "chrome", "--out", out("a.json"), "--to-ms"},
  };
  for (const auto& args : bad) {
    const CliResult r = runCli(args);
    EXPECT_EQ(r.code, kExitUsage) << args.size()
                                  << " args, first extra: " << (args.size() > 1 ? args[1] : "");
    EXPECT_NE(r.err.find("usage: tachyon-cli export"), std::string::npos);
  }
  EXPECT_FALSE(fs::exists(out("a.json")));
}

TEST_F(ExportCommand, UsageErrorWithJsonPrintsErrorObject) {
  const CliResult r = runCli({"--json", "export", tiny(), "--out", out("a.json")});
  EXPECT_EQ(r.code, kExitUsage);
  const json j = json::parse(r.out);
  EXPECT_EQ(j["ok"], false);
  EXPECT_EQ(j["error"]["code"], "EUSAGE");
}

TEST_F(ExportCommand, MissingAndCorruptTraceExitOne) {
  CliResult r =
      runCli({"--json", "export", out("nope.tachyon"), "--format", "chrome", "--out", out("a.json")});
  EXPECT_EQ(r.code, kExitProtocolError);
  EXPECT_EQ(json::parse(r.out)["error"]["code"], "ENOTFOUND");

  const std::string garbage = out("garbage.tachyon");
  { std::ofstream(garbage, std::ios::binary) << "this is not a trace file at all, just text"; }
  r = runCli({"--json", "export", garbage, "--format", "speedscope", "--out", out("a.json")});
  EXPECT_EQ(r.code, kExitProtocolError);
  EXPECT_EQ(json::parse(r.out)["error"]["code"], "EIO");
  EXPECT_FALSE(fs::exists(out("a.json")));
}

TEST_F(ExportCommand, UnwritableOutputExitsOne) {
  const CliResult r = runCli({"export", tiny(), "--format", "chrome", "--out", out("no/such/dir/a.json")});
  EXPECT_EQ(r.code, kExitProtocolError);
  EXPECT_NE(r.err.find("cannot open"), std::string::npos);
}

TEST_F(ExportCommand, UnknownThreadIdIsAUsageError) {
  const CliResult r =
      runCli({"export", tiny(), "--format", "chrome", "--out", out("a.json"), "--threads", "4000000"});
  EXPECT_EQ(r.code, kExitUsage);
  EXPECT_NE(r.err.find("no thread with id"), std::string::npos);
}

TEST_F(ExportCommand, SpeedscopeExport) {
  const CliResult r = runCli(
      {"export", tiny(), "--format", "speedscope", "--out", out("s.json"), "--symbols", tinySymbols()});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("speedscope"), std::string::npos);
  const json doc = readJson(out("s.json"));
  EXPECT_EQ(doc["$schema"], "https://www.speedscope.app/file-format-schema.json");
  EXPECT_GT(doc["profiles"].size(), 0u);
  EXPECT_GT(doc["shared"]["frames"].size(), 0u);
  // With the symbols no frame is a bare address.
  size_t named = 0;
  for (const json& f : doc["shared"]["frames"]) {
    const std::string n = f["name"].get<std::string>();
    if (n.find("0x") == std::string::npos) ++named;
  }
  EXPECT_GT(named, 0u);
  EXPECT_EQ(r.err.find("no symbols"), std::string::npos);
}

TEST_F(ExportCommand, SpeedscopeWithoutSymbolsWarnsAndNamesByAddress) {
  const CliResult r = runCli({"export", tiny(), "--format", "speedscope", "--out", out("s.json")});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.err.find("no symbols for"), std::string::npos);
  const json doc = readJson(out("s.json"));
  for (const json& f : doc["shared"]["frames"])
    EXPECT_NE(f["name"].get<std::string>().find("0x"), std::string::npos);
}

TEST_F(ExportCommand, MissingSymbolPathIsAWarning) {
  const CliResult r = runCli({"export", tiny(), "--format", "speedscope", "--out", out("s.json"), "--symbols",
                              out("missing.symbols.json")});
  EXPECT_EQ(r.code, kExitOk);
  EXPECT_NE(r.err.find("does not exist"), std::string::npos);
}

TEST_F(ExportCommand, SymbolDirectoryPicksUpSidecars) {
  fs::copy_file(tinySymbols(), dir.path() / "tiny.symbols.json");
  const CliResult r = runCli(
      {"export", tiny(), "--format", "speedscope", "--out", out("s.json"), "--symbols", dir.path().string()});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(r.err.find("no symbols"), std::string::npos) << r.err;
}

TEST_F(ExportCommand, ChromeExportWithFilters) {
  auto loaded = model::TraceModel::load(tiny());
  ASSERT_TRUE(loaded.ok());
  ASSERT_GE(loaded->threads.size(), 1u);
  const uint32_t tid = loaded->threads[0].tid;

  CliResult r = runCli({"export", tiny(), "--format", "chrome", "--out", out("c.json")});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  const json all = readJson(out("c.json"));
  EXPECT_EQ(all["displayTimeUnit"], "ns");
  EXPECT_TRUE(all["traceEvents"].is_array());

  r = runCli({"export", tiny(), "--format", "chrome", "--out", out("c2.json"), "--threads",
              std::to_string(tid), "--from-ms", "0", "--to-ms", "1"});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  const json some = readJson(out("c2.json"));
  EXPECT_LE(some["traceEvents"].size(), all["traceEvents"].size());
  for (const json& e : some["traceEvents"]) {
    if (e["ph"] == "M" || e["ph"] == "C") continue;
    if (e["cat"] != "cpu") EXPECT_EQ(e["tid"], tid);
    EXPECT_LE(e["ts"].get<double>(), 1000.0 + 1e-6);  // within the first millisecond (µs)
  }
}

TEST_F(ExportCommand, JsonSummary) {
  const CliResult r = runCli({"--json", "export", tiny(), "--format", "speedscope", "--out", out("s.json"),
                              "--symbols", tinySymbols()});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  const json j = json::parse(r.out);
  EXPECT_EQ(j["ok"], true);
  EXPECT_EQ(j["format"], "speedscope");
  EXPECT_EQ(j["output"], out("s.json"));
  EXPECT_GT(j["bytes"].get<uint64_t>(), 0u);
  EXPECT_EQ(j["bytes"].get<uint64_t>(), fs::file_size(out("s.json")));
  EXPECT_GT(j["samples"].get<uint64_t>(), 0u);
  const json c =
      json::parse(runCli({"--json", "export", tiny(), "--format", "chrome", "--out", out("c.json")}).out);
  EXPECT_EQ(c["format"], "chrome");
  EXPECT_GT(c["events"].get<uint64_t>(), 0u);
}

TEST_F(ExportCommand, DashWritesToStdout) {
  const CliResult r = runCli({"export", tiny(), "--format", "chrome", "--out", "-"});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  const json doc = json::parse(r.out);  // stdout is the export and nothing else
  EXPECT_TRUE(doc["traceEvents"].is_array());
}

TEST_F(ExportCommand, OutputIsDeterministic) {
  ASSERT_EQ(
      runCli({"export", tiny(), "--format", "speedscope", "--out", out("a.json"), "--symbols", tinySymbols()})
          .code,
      kExitOk);
  ASSERT_EQ(
      runCli({"export", tiny(), "--format", "speedscope", "--out", out("b.json"), "--symbols", tinySymbols()})
          .code,
      kExitOk);
  std::ifstream a(out("a.json"), std::ios::binary), b(out("b.json"), std::ios::binary);
  std::stringstream sa, sb;
  sa << a.rdbuf();
  sb << b.rdbuf();
  EXPECT_EQ(sa.str(), sb.str());
}

#ifdef TACHYON_CLI_EXE
TEST_F(ExportCommand, BinaryExitCodes) {
  const std::string exe = TACHYON_CLI_EXE;
  EXPECT_EQ(std::system(("\"" + exe + "\" export \"" + tiny() + "\" --format chrome --out \"" +
                         out("e.json") + "\" > /dev/null 2>&1")
                            .c_str()),
            0);
  EXPECT_TRUE(fs::exists(out("e.json")));
  const int usage = std::system(("\"" + exe + "\" export > /dev/null 2>&1").c_str());
  EXPECT_NE(usage, 0);
}
#endif

}  // namespace
}  // namespace tachyon::cli
