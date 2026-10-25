// WP-52: `tachyon-cli merge TRACE INST [--out PATH]`. Offline: none of these tests starts an agent.
#include "../control/test_util.h"
#include "../inst/trace_builder.h"
#include "../inst/wire_builder.h"
#include "cli/cli.h"
#include "inst/merge.h"
#include "inst/sidecar_paths.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iterator>
#include <sstream>

namespace tachyon::cli {
namespace {

namespace fs = std::filesystem;
using control::testing_util::TempDir;
using inst::testing_util::StreamBuilder;
using inst::testing_util::writeBaseTrace;

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

StreamBuilder oneThread(uint32_t tid, uint64_t t0, const std::string& name, bool close = true) {
  StreamBuilder b;
  b.header(77, 1'000'000'000, t0);
  const uint64_t k = b.string(name);
  b.threadName(t0, tid, "thread-" + std::to_string(tid));
  b.scopeBegin(t0 + 10, tid, k);
  if (close) b.scopeEnd(t0 + 20, tid);
  return b;
}

std::vector<char> bytesOf(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

size_t countFiles(const fs::path& dir) {
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    (void)e;
    ++n;
  }
  return n;
}

size_t scopeCount(const fs::path& trace) {
  auto reader = trace::TraceReader::open(trace);
  EXPECT_TRUE(reader.ok());
  return reader.ok() ? reader->readScopeEvents().value().size() : 0;
}

TEST(CliMerge, WithOutKeepsTheInputAndReportsWhatWasMerged) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const auto before = bytesOf(base.path);
  const fs::path inst = oneThread(5, 2000, "work").write(dir.path() / "app.tachyon-inst");
  const fs::path out = dir.path() / "out.tachyon";
  const CliResult r = runCli({"merge", base.path.string(), inst.string(), "--out", out.string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("merged 1 instrumentation file(s) into " + out.string()), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("2 scope events"), std::string::npos) << r.out;
  EXPECT_EQ(bytesOf(base.path), before);
  EXPECT_EQ(scopeCount(out), 2u);
}

TEST(CliMerge, WithoutOutReplacesTheTraceThroughATemporaryFile) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path inst = oneThread(5, 2000, "work").write(dir.path() / "app.tachyon-inst");
  const CliResult r = runCli({"merge", base.path.string(), inst.string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(scopeCount(base.path), 2u);
  EXPECT_EQ(countFiles(dir.path()), 2u);  // no temporary left behind
}

TEST(CliMerge, NumberedSiblingsAreMergedUnlessDisabled) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path stem = dir.path() / "game-9.tachyon-inst";
  oneThread(5, 2000, "phase0").write(stem);
  oneThread(6, 3000, "phase1").write(inst::numberedSidecarPath(stem, 1));
  oneThread(7, 4000, "phase2").write(inst::numberedSidecarPath(stem, 2));

  const fs::path all = dir.path() / "all.tachyon";
  CliResult r = runCli({"merge", base.path.string(), stem.string(), "--out", all.string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("merged 3 instrumentation file(s)"), std::string::npos) << r.out;
  EXPECT_EQ(scopeCount(all), 6u);

  const fs::path only = dir.path() / "only.tachyon";
  r = runCli({"merge", base.path.string(), stem.string(), "--no-siblings", "--out", only.string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("merged 1 instrumentation file(s)"), std::string::npos) << r.out;
  EXPECT_EQ(scopeCount(only), 2u);
}

TEST(CliMerge, WorksWithoutAnAgentEvenWithGlobalOptionsBeforeTheCommand) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path inst = oneThread(5, 2000, "work").write(dir.path() / "app.tachyon-inst");
  const CliResult r =
      runCli({"--socket", (dir.path() / "no-agent.sock").string(), "--timeout", "1", "merge",
              base.path.string(), inst.string(), "--out", (dir.path() / "o.tachyon").string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
}

TEST(CliMerge, JsonOutput) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path inst = oneThread(5, 2000, "work").write(dir.path() / "app.tachyon-inst");
  const fs::path out = dir.path() / "o.tachyon";
  const CliResult r = runCli({"--json", "merge", base.path.string(), inst.string(), "--out", out.string()});
  ASSERT_EQ(r.code, kExitOk) << r.err;
  const auto j = nlohmann::json::parse(r.out);
  EXPECT_TRUE(j["ok"].get<bool>());
  EXPECT_EQ(j["output"], out.string());
  EXPECT_EQ(j["scope_events"], 2);
  EXPECT_EQ(j["thread_names"], 1);
  EXPECT_EQ(j["inputs"].size(), 1u);
  EXPECT_TRUE(j["warnings"].is_array());
  EXPECT_EQ(r.err, "");
}

TEST(CliMerge, UnbalancedScopesAndTruncationAreWarningsNotFailures) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path inst = oneThread(5, 2000, "work", /*close=*/false).write(dir.path() / "app.tachyon-inst");
  const CliResult r =
      runCli({"merge", base.path.string(), inst.string(), "--out", (dir.path() / "o.tachyon").string()});
  EXPECT_EQ(r.code, kExitOk);
  EXPECT_NE(r.err.find("warning:"), std::string::npos) << r.err;
  EXPECT_NE(r.err.find("still open"), std::string::npos) << r.err;

  const CliResult j = runCli(
      {"--json", "merge", base.path.string(), inst.string(), "--out", (dir.path() / "o2.tachyon").string()});
  EXPECT_EQ(j.code, kExitOk);
  const auto doc = nlohmann::json::parse(j.out);
  EXPECT_EQ(doc["unclosed_scopes"], 1);
  EXPECT_GE(doc["warnings"].size(), 1u);
}

TEST(CliMerge, ClipDropsRecordsOutsideTheTraceWindow) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon", 1000, 3000);
  StreamBuilder b = oneThread(5, 500, "early");  // 510..520, before the window
  const uint64_t k = b.string("late");
  b.scopeBegin(2500, 5, k);
  b.scopeEnd(2600, 5);
  const fs::path inst = b.write(dir.path() / "app.tachyon-inst");
  const fs::path out = dir.path() / "clipped.tachyon";
  const CliResult r = runCli({"merge", base.path.string(), inst.string(), "--clip", "--out", out.string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_EQ(scopeCount(out), 2u);
  EXPECT_NE(r.err.find("outside the trace window"), std::string::npos) << r.err;
}

TEST(CliMerge, NothingToMergeInPlaceSaysSoAndKeepsTheFile) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const auto before = bytesOf(base.path);
  StreamBuilder empty;
  empty.header();
  const fs::path inst = empty.write(dir.path() / "empty.tachyon-inst");
  const CliResult r = runCli({"merge", base.path.string(), inst.string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("nothing merged"), std::string::npos) << r.out;
  EXPECT_EQ(bytesOf(base.path), before);
}

TEST(CliMerge, UsageErrors) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const fs::path inst = oneThread(5, 2000, "work").write(dir.path() / "app.tachyon-inst");
  EXPECT_EQ(runCli({"merge"}).code, kExitUsage);
  EXPECT_EQ(runCli({"merge", base.path.string()}).code, kExitUsage);
  EXPECT_EQ(runCli({"merge", base.path.string(), inst.string(), "extra"}).code, kExitUsage);
  EXPECT_EQ(runCli({"merge", base.path.string(), inst.string(), "--bogus"}).code, kExitUsage);
  EXPECT_EQ(runCli({"merge", base.path.string(), inst.string(), "--out"}).code, kExitUsage);
  const CliResult json = runCli({"--json", "merge"});
  EXPECT_EQ(json.code, kExitUsage);
  EXPECT_NE(json.out.find("EUSAGE"), std::string::npos) << json.out;
}

TEST(CliMerge, MissingOrBrokenInputsFailWithoutDamage) {
  TempDir dir;
  const auto base = writeBaseTrace(dir.path() / "base.tachyon");
  const auto before = bytesOf(base.path);
  const fs::path inst = oneThread(5, 2000, "work").write(dir.path() / "app.tachyon-inst");

  CliResult r = runCli({"merge", (dir.path() / "gone.tachyon").string(), inst.string()});
  EXPECT_EQ(r.code, kExitProtocolError);
  EXPECT_NE(r.err.find("trace not found"), std::string::npos) << r.err;

  r = runCli({"merge", base.path.string(), (dir.path() / "gone.tachyon-inst").string()});
  EXPECT_EQ(r.code, kExitProtocolError);
  EXPECT_NE(r.err.find("instrumentation file not found"), std::string::npos) << r.err;

  const fs::path junk = dir.path() / "junk.tachyon-inst";
  std::ofstream(junk) << "not a stream not a stream not a stream";
  r = runCli({"merge", base.path.string(), junk.string()});
  EXPECT_EQ(r.code, kExitProtocolError);
  EXPECT_NE(r.err.find("merge failed"), std::string::npos) << r.err;
  EXPECT_EQ(bytesOf(base.path), before);
}

}  // namespace
}  // namespace tachyon::cli
