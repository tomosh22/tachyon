// WP-62: the tachyon-report command line: outputs, baseline diff exit codes, usage and I/O errors,
// determinism. Runs report::run in-process; one test also runs the real executable.
#include "report/report_cli.h"

#include "report_test_util.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>
#include <sstream>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

namespace tachyon::report {
namespace {

using nlohmann::json;
using testutil::generated;
using testutil::readJson;
using testutil::readText;
using testutil::tempDir;
using testutil::writeText;

struct RunResult {
  int code = -1;
  std::string out, err;
};

RunResult runCli(const std::vector<std::string>& args) {
  std::ostringstream out, err;
  RunResult r;
  r.code = run(args, out, err);
  r.out = out.str();
  r.err = err.str();
  return r;
}

std::string trace(const std::string& name) {
  return generated(name + ".tachyon").string();
}

// Writes the report of `fixture` to <dir>/<file> and returns its parsed JSON.
json makeReport(const std::filesystem::path& dir, const std::string& fixture, const std::string& file,
                std::vector<std::string> extra = {}) {
  std::vector<std::string> args = {trace(fixture), "--out", (dir / file).string()};
  args.insert(args.end(), extra.begin(), extra.end());
  const RunResult r = runCli(args);
  EXPECT_EQ(r.code, 0) << r.err;
  return readJson(dir / file);
}

// Doctors a report JSON and writes it as <dir>/<file>.
std::string doctor(const std::filesystem::path& dir, const std::string& file, json doc,
                   const std::function<void(json&)>& edit) {
  edit(doc);
  writeText(dir / file, doc.dump(2));
  return (dir / file).string();
}

bool hasFinding(const json& list, const std::string& category, const std::string& subject = "") {
  for (const json& f : list)
    if (f["category"] == category && (subject.empty() || f["subject"] == subject)) return true;
  return false;
}

TEST(ReportCli, WritesJsonAndMarkdownByExtension) {
  const auto dir = tempDir("outputs");
  const RunResult r = runCli({trace("frames"), "--out", (dir / "a.json").string(), "--out",
                              (dir / "b.md").string(), "--out", (dir / "c.markdown").string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  const json doc = readJson(dir / "a.json");
  ASSERT_TRUE(doc.is_object());
  EXPECT_EQ(doc["schema_version"], 1);
  EXPECT_TRUE(testutil::reportSchema().validate(doc).empty());
  EXPECT_EQ(readText(dir / "b.md").rfind("## Tachyon performance report", 0), 0u);
  EXPECT_EQ(readText(dir / "b.md"), readText(dir / "c.markdown"));
  EXPECT_NE(r.out.find("report written"), std::string::npos);
}

TEST(ReportCli, StdoutFormatDefaultsToJson) {
  const RunResult js = runCli({trace("frames")});
  EXPECT_EQ(js.code, 0);
  EXPECT_TRUE(json::parse(js.out, nullptr, false).is_object());
  const RunResult md = runCli({trace("frames"), "--format", "md"});
  EXPECT_EQ(md.code, 0);
  EXPECT_EQ(md.out.rfind("## Tachyon performance report", 0), 0u);
  EXPECT_EQ(runCli({trace("frames"), "--format=md"}).out, md.out);
}

TEST(ReportCli, TopNAndNoBlendAndBudgetAreApplied) {
  const RunResult r = runCli({trace("scopes"), "--top", "2", "--no-blend", "--frame-budget-ms", "10"});
  ASSERT_EQ(r.code, 0) << r.err;
  const json doc = json::parse(r.out);
  EXPECT_EQ(doc["settings"]["top_n"], 2);
  EXPECT_EQ(doc["settings"]["blend_scopes"], false);
  EXPECT_EQ(doc["settings"]["frame_budget_ms"], 10.0);
  EXPECT_EQ(doc["functions"]["top"].size(), 2u);
  EXPECT_EQ(doc["scopes"]["top"].size(), 2u);
  const json blended = json::parse(runCli({trace("scopes")}).out);
  EXPECT_EQ(blended["settings"]["blend_scopes"], true);
}

TEST(ReportCli, FrameSeriesOption) {
  const json doc = json::parse(runCli({trace("frames"), "--frame-series", "RenderFrame"}).out);
  ASSERT_EQ(doc["frames"]["series"].size(), 1u);
  EXPECT_EQ(doc["frames"]["series"][0]["name"], "RenderFrame");
  const RunResult bad = runCli({trace("frames"), "--frame-series", "Nope"});
  EXPECT_EQ(bad.code, kExitUsage);
  EXPECT_NE(bad.err.find("Nope"), std::string::npos);
}

TEST(ReportCli, SameReportAsBaselinePasses) {
  const auto dir = tempDir("same");
  makeReport(dir, "frames", "base.json");
  const RunResult r = runCli({trace("frames"), "--baseline", (dir / "base.json").string(), "--out",
                              (dir / "cur.json").string(), "--out", (dir / "cur.md").string()});
  EXPECT_EQ(r.code, kExitOk) << r.err;
  EXPECT_NE(r.out.find("PASS"), std::string::npos);
  const json cur = readJson(dir / "cur.json");
  EXPECT_EQ(cur["baseline_diff"]["verdict"], "pass");
  EXPECT_EQ(cur["baseline_diff"]["regression_count"], 0);
  EXPECT_TRUE(testutil::reportSchema().validate(cur).empty());
  EXPECT_NE(readText(dir / "cur.md").find("**PASS**"), std::string::npos);
  // A report that carries a baseline_diff is itself a valid baseline.
  const RunResult chained = runCli({trace("frames"), "--baseline", (dir / "cur.json").string()});
  EXPECT_EQ(chained.code, kExitOk) << chained.err;
}

TEST(ReportCli, FrameP95RegressionGivesExit1) {
  const auto dir = tempDir("p95");
  const json base = makeReport(dir, "frames", "base.json");
  // Series 1 (Render): the baseline's p95 was 30 % lower, i.e. the current run is 43 % slower.
  const std::string slow = doctor(dir, "slow.json", base, [](json& d) {
    d["frames"]["series"][1]["p95_ms"] = d["frames"]["series"][1]["p95_ms"].get<double>() * 0.7;
  });
  const RunResult r = runCli({trace("frames"), "--baseline", slow, "--out", (dir / "cur.json").string(),
                              "--out", (dir / "cur.md").string()});
  EXPECT_EQ(r.code, kExitRegression) << r.err;
  const json cur = readJson(dir / "cur.json");
  EXPECT_EQ(cur["baseline_diff"]["verdict"], "regression");
  EXPECT_TRUE(hasFinding(cur["baseline_diff"]["regressions"], "frame_p95", "RenderFrame@Render"));
  EXPECT_FALSE(hasFinding(cur["baseline_diff"]["regressions"], "frame_p95", "Frame@Main"));
  EXPECT_NE(readText(dir / "cur.md").find("**FAIL**"), std::string::npos);
  EXPECT_NE(r.out.find("FAIL"), std::string::npos);
}

TEST(ReportCli, FunctionShareRegressionGivesExit1) {
  const auto dir = tempDir("fn");
  const json base = makeReport(dir, "frames", "base.json");
  const std::string lowered = doctor(dir, "lowered.json", base, [](json& d) {
    d["functions"]["top"][0]["exclusive_pct"] = d["functions"]["top"][0]["exclusive_pct"].get<double>() - 6.0;
    // The baseline function set (WP-69) is what the diff compares; its first row is the same function.
    d["functions"]["significant"][0]["exclusive_pct"] =
        d["functions"]["significant"][0]["exclusive_pct"].get<double>() - 6.0;
  });
  const RunResult r = runCli({trace("frames"), "--baseline", lowered, "--out", (dir / "cur.json").string()});
  EXPECT_EQ(r.code, kExitRegression) << r.err;
  const json cur = readJson(dir / "cur.json");
  const json& top = base["functions"]["top"][0];
  EXPECT_TRUE(hasFinding(cur["baseline_diff"]["regressions"], "function_exclusive",
                         top["module"].get<std::string>() + "!" + top["name"].get<std::string>()));
}

// WP-69: a baseline written by the previous version (no function set, no total CPU) still loads and compares.
TEST(ReportCli, OldFormatBaselineStillCompares) {
  const auto dir = tempDir("oldfmt");
  const json base = makeReport(dir, "frames", "base.json");
  const std::string old = doctor(dir, "old.json", base, [](json& d) {
    d["functions"].erase("significant");
    d["functions"].erase("significant_floor_pct");
    d["trace"].erase("total_cpu_ms");
    d["functions"]["top"][0]["exclusive_pct"] = d["functions"]["top"][0]["exclusive_pct"].get<double>() - 6.0;
  });
  const RunResult r = runCli({trace("frames"), "--baseline", old, "--out", (dir / "cur.json").string()});
  EXPECT_EQ(r.code, kExitRegression) << r.err;
  const json cur = readJson(dir / "cur.json");
  const json& top = base["functions"]["top"][0];
  EXPECT_TRUE(hasFinding(cur["baseline_diff"]["regressions"], "function_exclusive",
                         top["module"].get<std::string>() + "!" + top["name"].get<std::string>()));
  EXPECT_FALSE(hasFinding(cur["baseline_diff"]["regressions"], "total_cpu", "total CPU"));
  // The unchanged old baseline passes.
  const std::string same = doctor(dir, "same.json", base, [](json& d) {
    d["functions"].erase("significant");
    d["functions"].erase("significant_floor_pct");
    d["trace"].erase("total_cpu_ms");
  });
  EXPECT_EQ(runCli({trace("frames"), "--baseline", same}).code, kExitOk);
}

TEST(ReportCli, ScopeMedianRegressionGivesExit1) {
  const auto dir = tempDir("scope");
  const json base = makeReport(dir, "scopes", "base.json");
  const std::string halved = doctor(dir, "halved.json", base, [](json& d) {
    for (json& s : d["scopes"]["top"])
      if (s["name"] == "Update") s["median_ms"] = s["median_ms"].get<double>() / 2;
  });
  const RunResult r = runCli({trace("scopes"), "--baseline", halved, "--out", (dir / "cur.json").string()});
  EXPECT_EQ(r.code, kExitRegression) << r.err;
  const json cur = readJson(dir / "cur.json");
  EXPECT_TRUE(hasFinding(cur["baseline_diff"]["regressions"], "scope_median", "scope:Update"));
  EXPECT_EQ(cur["baseline_diff"]["regressions"].size(), 1u);
}

TEST(ReportCli, ChangesWithinThresholdsExit0AndTightThresholdsFlagThem) {
  const auto dir = tempDir("within");
  const json base = makeReport(dir, "frames", "base.json");
  // The baseline is 5 % faster / smaller than now: inside the default 10 % / 15 % / 2 pp / 25 % limits.
  const std::string near = doctor(dir, "near.json", base, [](json& d) {
    for (json& s : d["frames"]["series"]) {
      s["p95_ms"] = s["p95_ms"].get<double>() * 0.95;
      s["p99_ms"] = s["p99_ms"].get<double>() * 0.95;
    }
    d["functions"]["top"][0]["exclusive_pct"] = d["functions"]["top"][0]["exclusive_pct"].get<double>() - 1.0;
  });
  EXPECT_EQ(runCli({trace("frames"), "--baseline", near}).code, kExitOk);

  writeText(
      dir / "tight.json",
      R"({"frame_p95_rel_pct": 1, "frame_p99_rel_pct": 1, "frame_abs_floor_ms": 0.01, "function_exclusive_pp": 0.5})");
  const RunResult tight =
      runCli({trace("frames"), "--baseline", near, "--thresholds", (dir / "tight.json").string()});
  EXPECT_EQ(tight.code, kExitRegression);

  writeText(dir / "loose.json",
            R"({"frame_p95_rel_pct": 500, "frame_p99_rel_pct": 500, "function_exclusive_pp": 50})");
  const std::string slow = doctor(dir, "slow.json", base, [](json& d) {
    d["frames"]["series"][1]["p95_ms"] = d["frames"]["series"][1]["p95_ms"].get<double>() * 0.5;
  });
  EXPECT_EQ(runCli({trace("frames"), "--baseline", slow}).code, kExitRegression);
  EXPECT_EQ(runCli({trace("frames"), "--baseline", slow, "--thresholds", (dir / "loose.json").string()}).code,
            kExitOk);
}

TEST(ReportCli, LostEventsRegression) {
#if defined(TACHYON_TRACEGEN_EXE) && !defined(_WIN32)
  const auto dir = tempDir("lost");
  json scenario = readJson(std::filesystem::path(TACHYON_FIXTURE_DIR) / "tracegen" / "tiny.json");
  ASSERT_TRUE(scenario.is_object());
  scenario["name"] = "lost";
  scenario["events_lost"] = 42;
  writeText(dir / "lost_scenario.json", scenario.dump());
  const std::string cmd = std::string("\"") + TACHYON_TRACEGEN_EXE + "\" --out-dir \"" + dir.string() +
                          "\" \"" + (dir / "lost_scenario.json").string() + "\" > /dev/null 2>&1";
  ASSERT_EQ(std::system(cmd.c_str()), 0);
  const std::string lostTrace = (dir / "lost.tachyon").string();

  // A baseline taken from the same trace has the same lost events: fine. One taken before the loss: not.
  EXPECT_EQ(runCli({lostTrace, "--out", (dir / "lost.json").string()}).code, kExitOk);
  const json lostDoc = readJson(dir / "lost.json");
  EXPECT_EQ(lostDoc["trace"]["events_lost"], 42);
  EXPECT_EQ(runCli({lostTrace, "--baseline", (dir / "lost.json").string()}).code, kExitOk);
  const std::string clean =
      doctor(dir, "clean.json", lostDoc, [](json& d) { d["trace"]["events_lost"] = 0; });
  const RunResult r = runCli({lostTrace, "--baseline", clean, "--out", (dir / "cur.json").string()});
  EXPECT_EQ(r.code, kExitRegression);
  EXPECT_TRUE(hasFinding(readJson(dir / "cur.json")["baseline_diff"]["regressions"], "lost_events"));
  EXPECT_NE(r.err.find("42 events were lost"), std::string::npos);

  writeText(dir / "allow.json", R"({"lost_events_allowed": 100})");
  EXPECT_EQ(runCli({lostTrace, "--baseline", clean, "--thresholds", (dir / "allow.json").string()}).code,
            kExitOk);
  // --fail-on-lost
  EXPECT_EQ(runCli({lostTrace, "--baseline", clean, "--thresholds", (dir / "allow.json").string(),
                    "--fail-on-lost"})
                .code,
            kExitLostEvents);
  EXPECT_EQ(runCli({trace("frames"), "--fail-on-lost"}).code, kExitOk);
#else
  GTEST_SKIP() << "needs the tracegen executable";
#endif
}

TEST(ReportCli, FailOnLostFlagsPartialTraces) {
  EXPECT_EQ(runCli({trace("partial_torn")}).code, kExitOk);
  const RunResult r = runCli({trace("partial_torn"), "--fail-on-lost"});
  EXPECT_EQ(r.code, kExitLostEvents);
  EXPECT_NE(r.err.find("partial"), std::string::npos);
}

TEST(ReportCli, UsageErrorsGiveExit2) {
  const std::string t = trace("frames");
  const std::vector<std::vector<std::string>> bad = {
      {},
      {"--top", "5"},
      {t, "extra.tachyon"},
      {t, "--bogus"},
      {t, "--top", "0"},
      {t, "--top", "abc"},
      {t, "--top"},
      {t, "--format", "xml"},
      {t, "--out", "report.txt"},
      {t, "--out"},
      {t, "--frame-budget-ms", "-1"},
      {t, "--frame-budget-ms", "x"},
      {t, "--baseline"},
  };
  for (const auto& args : bad) {
    const RunResult r = runCli(args);
    EXPECT_EQ(r.code, kExitUsage) << (args.empty() ? "<no args>" : args.back()) << " | " << r.err;
    EXPECT_TRUE(r.out.empty());
    EXPECT_NE(r.err.find("usage"), std::string::npos);
  }
  EXPECT_EQ(runCli({"--help"}).code, kExitOk);
  const RunResult missingSymbols = runCli({t, "--symbols", "/nonexistent/path/to/symbols"});
  EXPECT_EQ(missingSymbols.code, kExitUsage);
  EXPECT_NE(missingSymbols.err.find("symbol path"), std::string::npos);
}

TEST(ReportCli, InvalidThresholdsGiveExit2AndMissingFileExit3) {
  const auto dir = tempDir("thr");
  writeText(dir / "bad_key.json", R"({"frame_p95": 1})");
  writeText(dir / "bad_type.json", R"({"frame_p95_rel_pct": "x"})");
  writeText(dir / "garbage.json", "not json");
  for (const char* f : {"bad_key.json", "bad_type.json", "garbage.json"})
    EXPECT_EQ(runCli({trace("frames"), "--thresholds", (dir / f).string()}).code, kExitUsage) << f;
  EXPECT_EQ(runCli({trace("frames"), "--thresholds", (dir / "missing.json").string()}).code, kExitIoError);
}

TEST(ReportCli, TraceAndFileErrorsGiveExit3) {
  const auto dir = tempDir("io");
  EXPECT_EQ(runCli({(dir / "missing.tachyon").string()}).code, kExitIoError);

  writeText(dir / "corrupt.tachyon", std::string(4096, 'x'));
  const RunResult corrupt = runCli({(dir / "corrupt.tachyon").string()});
  EXPECT_EQ(corrupt.code, kExitIoError);
  EXPECT_NE(corrupt.err.find("tachyon-report"), std::string::npos);

  writeText(dir / "empty.tachyon", "");
  EXPECT_EQ(runCli({(dir / "empty.tachyon").string()}).code, kExitIoError);

  // A truncated header.
  const std::string good = readText(generated("frames.tachyon"));
  writeText(dir / "short.tachyon", good.substr(0, 20));
  EXPECT_EQ(runCli({(dir / "short.tachyon").string()}).code, kExitIoError);

  // Bad baselines.
  writeText(dir / "garbage.json", "{ nope");
  EXPECT_EQ(runCli({trace("frames"), "--baseline", (dir / "garbage.json").string()}).code, kExitIoError);
  EXPECT_EQ(runCli({trace("frames"), "--baseline", (dir / "missing.json").string()}).code, kExitIoError);
  const json base = makeReport(dir, "frames", "base.json");
  const std::string v2 = doctor(dir, "v2.json", base, [](json& d) { d["schema_version"] = 2; });
  const RunResult wrongVersion = runCli({trace("frames"), "--baseline", v2});
  EXPECT_EQ(wrongVersion.code, kExitIoError);
  EXPECT_NE(wrongVersion.err.find("schema_version"), std::string::npos);
  const std::string incomplete = doctor(dir, "incomplete.json", base, [](json& d) { d.erase("functions"); });
  EXPECT_EQ(runCli({trace("frames"), "--baseline", incomplete}).code, kExitIoError);

  // Output that cannot be created.
  EXPECT_EQ(runCli({trace("frames"), "--out", (dir / "no_such_dir" / "r.json").string()}).code, kExitIoError);
}

TEST(ReportCli, SymbolsOptionResolvesFunctionNames) {
  const auto dir = tempDir("symbols");
  std::filesystem::copy_file(generated("frames.tachyon"), dir / "nosym.tachyon");  // no sidecar next to it
  const json plain = json::parse(runCli({(dir / "nosym.tachyon").string()}).out);
  ASSERT_FALSE(plain["functions"]["top"].empty());
  EXPECT_NE(plain["functions"]["top"][0]["name"].get<std::string>().rfind("0x", 0), std::string::npos)
      << "unresolved frames are named by address";

  const std::string sym = generated("frames.symbols.json").string();
  // Several values after one --symbols, then more options.
  const RunResult r = runCli({(dir / "nosym.tachyon").string(), "--symbols", sym, sym, "--top", "3"});
  ASSERT_EQ(r.code, 0) << r.err;
  const json doc = json::parse(r.out);
  EXPECT_EQ(doc["settings"]["top_n"], 3);
  EXPECT_EQ(doc["functions"]["top"][0]["name"], "Render::submit");
  // Identical to the run that found the sidecar next to the trace.
  EXPECT_EQ(runCli({(dir / "nosym.tachyon").string(), "--symbols=" + sym, "--top", "3"}).out, r.out);
  const json sidecar = json::parse(runCli({trace("frames"), "--top", "3"}).out);
  EXPECT_EQ(sidecar["functions"], doc["functions"]);
}

TEST(ReportCli, OutputsAreByteIdenticalAcrossRuns) {
  const auto dir = tempDir("determinism");
  const json base = makeReport(dir, "scopes", "base.json");
  const std::string older = doctor(dir, "older.json", base, [](json& d) {
    d["scopes"]["top"][0]["median_ms"] = d["scopes"]["top"][0]["median_ms"].get<double>() / 2;
    d["functions"]["top"][0]["exclusive_pct"] = 1.0;
  });
  for (const char* fixture : {"scopes", "frames", "medium"}) {
    const std::string baseline = std::string(fixture) == "scopes" ? older : std::string();
    std::string first[2];
    for (int round = 0; round < 2; ++round) {
      const auto j = dir / ("r" + std::to_string(round) + ".json");
      const auto m = dir / ("r" + std::to_string(round) + ".md");
      std::vector<std::string> args = {trace(fixture), "--out", j.string(), "--out", m.string()};
      if (!baseline.empty()) {
        args.push_back("--baseline");
        args.push_back(baseline);
      }
      runCli(args);
      first[round] = readText(j) + "\n----\n" + readText(m);
    }
    EXPECT_FALSE(first[0].empty());
    EXPECT_EQ(first[0], first[1]) << fixture;
  }
}

TEST(ReportCli, MarkdownHasNoAbsolutePaths) {
  const auto dir = tempDir("paths");
  const RunResult r = runCli({trace("scopes"), "--format", "md"});
  EXPECT_EQ(r.out.find(std::filesystem::path(TACHYON_BINARY_DIR).string()), std::string::npos);
  EXPECT_EQ(r.out.find(std::filesystem::path(TACHYON_FIXTURE_DIR).string()), std::string::npos);
  (void)dir;
}

TEST(ReportCli, RealExecutableReturnsTheDocumentedExitCodes) {
#if defined(TACHYON_REPORT_EXE) && !defined(_WIN32)
  auto exitCode = [](const std::string& args) {
    const std::string cmd = std::string("\"") + TACHYON_REPORT_EXE + "\" " + args + " > /dev/null 2>&1";
    const int status = std::system(cmd.c_str());
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  };
  const auto dir = tempDir("exe");
  EXPECT_EQ(exitCode("\"" + trace("frames") + "\" --out \"" + (dir / "base.json").string() + "\""), 0);
  EXPECT_EQ(exitCode("\"" + trace("frames") + "\" --baseline \"" + (dir / "base.json").string() + "\""), 0);
  EXPECT_EQ(exitCode("--nonsense"), 2);
  EXPECT_EQ(exitCode("\"" + (dir / "nope.tachyon").string() + "\""), 3);
  const json base = readJson(dir / "base.json");
  const std::string slow = doctor(dir, "slow.json", base, [](json& d) {
    d["frames"]["series"][1]["p95_ms"] = d["frames"]["series"][1]["p95_ms"].get<double>() * 0.5;
  });
  EXPECT_EQ(exitCode("\"" + trace("frames") + "\" --baseline \"" + slow + "\""), 1);
  EXPECT_EQ(exitCode("\"" + trace("partial_torn") + "\" --fail-on-lost"), 4);
#else
  GTEST_SKIP() << "needs the tachyon-report executable";
#endif
}

}  // namespace
}  // namespace tachyon::report
