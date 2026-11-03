// WP-63: Chrome trace event exporter. Hand-built models pin the exact events; tracegen fixtures check
// scopes against pairScopes, CPU tracks against the thread states, ordering and determinism.
#include "aggregation_test_util.h"
#include "analysis/scope_pairing.h"
#include "export_test_util.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <tuple>

namespace tachyon::exporters {
namespace {

using analysis::Filter;
using analysis::testutil::HandModel;
using exporttest::LoadedFixture;
using model::ThreadState;
using nlohmann::json;

int64_t toNs(const json& micros) {
  return std::llround(micros.get<double>() * 1000.0);
}

std::vector<json> eventsWhere(const json& doc, const std::function<bool(const json&)>& pred) {
  std::vector<json> out;
  for (const json& e : doc["traceEvents"])
    if (pred(e)) out.push_back(e);
  return out;
}

void addScope(HandModel& h, uint64_t begin, uint64_t end, const char* id, uint32_t tid,
              const char* data = "") {
  trace::ScopeEventRec b{};
  b.ts = begin;
  b.tid = tid;
  b.kind = trace::kScopeBegin;
  b.id = h.m.strings.intern(id);
  b.data = h.m.strings.intern(data);
  b.color_rgba = 0xFFFFFFFFu;
  h.m.scopeEvents.push_back(b);
  if (end != 0) {
    trace::ScopeEventRec e{};
    e.ts = end;
    e.tid = tid;
    e.kind = trace::kScopeEnd;
    h.m.scopeEvents.push_back(e);
  }
}

// Two threads of process 10 on CPUs 0 and 1; trace window [500, 100000).
HandModel makeModel() {
  HandModel h(2);
  h.m.header.clock_frequency_hz = 1'000'000'000ull;
  h.m.header.start_ts = 500;
  h.m.header.end_ts = 100000;
  h.m.threads[0].name = "Main";
  h.m.threads[0].pid = 10;
  h.m.threads[0].is_target = true;
  h.m.threads[1].name = "Worker \"1\"";
  h.m.threads[1].pid = 10;
  h.m.threads[1].is_target = true;
  model::Process p;
  p.pid = 10;
  p.name = "game";
  h.m.processes.push_back(p);

  auto state = [&](uint32_t t, uint64_t s, uint64_t e, ThreadState st, uint32_t cpu) {
    h.state(t, s, e, st);
    h.m.statesByThread[t].back().cpu = cpu;
  };
  state(0, 500, 5000, ThreadState::Running, 0);
  state(0, 5000, 8000, ThreadState::Waiting, 0);
  state(0, 8000, 100000, ThreadState::Running, 1);
  state(1, 500, 8000, ThreadState::Running, 1);
  state(1, 8000, 100000, ThreadState::Waiting, 1);

  addScope(h, 1000, 4000, "Update", 100);
  addScope(h, 1500, 2000, "Physics", 100, "dt=1");
  addScope(h, 9000, 0, "Open", 100);  // never ended: unterminated
  addScope(h, 1200, 1300, "Job", 101);
  std::stable_sort(h.m.scopeEvents.begin(), h.m.scopeEvents.end(),
                   [](const trace::ScopeEventRec& a, const trace::ScopeEventRec& b) { return a.ts < b.ts; });

  h.m.frameMarks.push_back({3000, ThreadIdx(0), h.m.strings.intern("Frame"), 7});
  h.m.markers.push_back(
      {1500, ThreadIdx(0), h.m.strings.intern("Hit"), h.m.strings.intern("x"), 0xFF0000FFu});
  h.m.markers.push_back({2000, ThreadIdx::invalid(), h.m.strings.intern("Global"), StringIdx(0), 0});
  model::PlotSeries fps;
  fps.name = h.m.strings.intern("fps");
  fps.ts = {1000, 2000};
  fps.value = {60.0, 59.5};
  h.m.plots.push_back(fps);
  h.finish();
  return h;
}

TEST(ChromeTrace, HandModelEvents) {
  HandModel h = makeModel();
  exporters::ExportStats stats;
  const std::string text = exporttest::chromeText(h.m, *h.svc, {}, &stats);
  const json doc = exporttest::parseOrFail(text);
  EXPECT_EQ(doc["displayTimeUnit"], "ns");
  ASSERT_TRUE(doc["traceEvents"].is_array());
  EXPECT_EQ(stats.events, doc["traceEvents"].size());
  EXPECT_EQ(stats.threads, 2u);

  // Metadata.
  auto meta = eventsWhere(doc, [](const json& e) { return e["ph"] == "M"; });
  std::map<std::tuple<std::string, int, int>, std::string> names;
  for (const json& e : meta)
    names[{e["name"].get<std::string>(), e["pid"].get<int>(), e["tid"].get<int>()}] = e["args"]["name"];
  EXPECT_EQ((names[{"process_name", 10, 0}]), "game (10)");
  EXPECT_EQ((names[{"thread_name", 10, 100}]), "Main");
  EXPECT_EQ((names[{"thread_name", 10, 101}]), "Worker \"1\"");
  int cpuPid = -1;
  for (const auto& [key, value] : names)
    if (std::get<0>(key) == "process_name" && value == "CPU") cpuPid = std::get<1>(key);
  ASSERT_GE(cpuPid, 0);
  EXPECT_NE(cpuPid, 10);
  EXPECT_EQ((names[{"thread_name", cpuPid, 0}]), "CPU 0");
  EXPECT_EQ((names[{"thread_name", cpuPid, 1}]), "CPU 1");

  // Thread 100: Update, Physics (scope before the marker at the same ts), Hit, Frame, Open.
  auto main =
      eventsWhere(doc, [](const json& e) { return e["ph"] != "M" && e["pid"] == 10 && e["tid"] == 100; });
  ASSERT_EQ(main.size(), 5u);
  EXPECT_EQ(main[0]["name"], "Update");
  EXPECT_EQ(main[0]["ph"], "X");
  EXPECT_EQ(toNs(main[0]["ts"]), 500);  // 1000 - trace start 500
  EXPECT_EQ(toNs(main[0]["dur"]), 3000);
  EXPECT_FALSE(main[0].contains("args"));
  EXPECT_EQ(main[1]["name"], "Physics");
  EXPECT_EQ(main[1]["args"]["data"], "dt=1");
  EXPECT_EQ(main[2]["name"], "Hit");
  EXPECT_EQ(main[2]["ph"], "i");
  EXPECT_EQ(main[2]["s"], "t");
  EXPECT_EQ(main[2]["args"]["data"], "x");
  EXPECT_EQ(main[2]["args"]["color"], "#FF0000FF");
  EXPECT_EQ(main[3]["name"], "Frame");
  EXPECT_EQ(main[3]["ph"], "i");
  EXPECT_EQ(main[3]["args"]["frame_number"], 7);
  EXPECT_EQ(main[4]["name"], "Open");
  EXPECT_EQ(toNs(main[4]["dur"]), 100000 - 9000);  // ends at the trace end
  EXPECT_EQ(main[4]["args"]["unterminated"], true);

  auto worker =
      eventsWhere(doc, [](const json& e) { return e["ph"] != "M" && e["pid"] == 10 && e["tid"] == 101; });
  ASSERT_EQ(worker.size(), 1u);
  EXPECT_EQ(worker[0]["name"], "Job");
  EXPECT_EQ(toNs(worker[0]["ts"]), 700);
  EXPECT_EQ(toNs(worker[0]["dur"]), 100);

  // CPU tracks: Running intervals, named after the running thread.
  auto cpu = eventsWhere(doc, [&](const json& e) { return e["ph"] == "X" && e["pid"] == cpuPid; });
  ASSERT_EQ(cpu.size(), 3u);
  // Sorted by (cpu, start): CPU 0: Main [500,5000); CPU 1: Worker [500,8000), Main [8000,100000).
  EXPECT_EQ(cpu[0]["tid"], 0);
  EXPECT_EQ(cpu[0]["name"], "Main");
  EXPECT_EQ(toNs(cpu[0]["ts"]), 0);
  EXPECT_EQ(toNs(cpu[0]["dur"]), 4500);
  EXPECT_EQ(cpu[0]["args"]["tid"], 100);
  EXPECT_EQ(cpu[1]["tid"], 1);
  EXPECT_EQ(cpu[1]["name"], "Worker \"1\"");
  EXPECT_EQ(toNs(cpu[1]["dur"]), 7500);
  EXPECT_EQ(cpu[2]["tid"], 1);
  EXPECT_EQ(cpu[2]["name"], "Main");
  EXPECT_EQ(toNs(cpu[2]["ts"]), 7500);

  // Global marker and counters.
  auto global = eventsWhere(doc, [](const json& e) { return e["ph"] == "i" && e["s"] == "g"; });
  ASSERT_EQ(global.size(), 1u);
  EXPECT_EQ(global[0]["name"], "Global");
  EXPECT_EQ(toNs(global[0]["ts"]), 1500);
  auto counters = eventsWhere(doc, [](const json& e) { return e["ph"] == "C"; });
  ASSERT_EQ(counters.size(), 2u);
  EXPECT_EQ(counters[0]["name"], "fps");
  EXPECT_DOUBLE_EQ(counters[0]["args"]["value"].get<double>(), 60.0);
  EXPECT_DOUBLE_EQ(counters[1]["args"]["value"].get<double>(), 59.5);
  EXPECT_EQ(toNs(counters[1]["ts"]), 1500);
}

TEST(ChromeTrace, ThreadAndTimeFilters) {
  HandModel h = makeModel();
  Filter byThread;
  byThread.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  json doc = exporttest::parseOrFail(exporttest::chromeText(h.m, *h.svc, byThread));
  auto scopes = eventsWhere(doc, [](const json& e) { return e["ph"] == "X" && e["cat"] == "scope"; });
  ASSERT_EQ(scopes.size(), 1u);
  EXPECT_EQ(scopes[0]["name"], "Job");
  for (const json& e : doc["traceEvents"]) {
    if (e["ph"] == "X" && e["cat"] == "cpu") EXPECT_EQ(e["args"]["tid"], 101);
    if (e["ph"] == "M" && e["name"] == "thread_name" && e["pid"] == 10) EXPECT_EQ(e["tid"], 101);
  }
  // Global markers and counters stay.
  EXPECT_EQ(eventsWhere(doc, [](const json& e) { return e["ph"] == "C"; }).size(), 2u);

  Filter range;
  range.t0 = 1450;
  range.t1 = 3500;  // selects Physics (1500), Hit (1500), Frame (3000); not Update (1000), nor Open
  doc = exporttest::parseOrFail(exporttest::chromeText(h.m, *h.svc, range));
  auto main =
      eventsWhere(doc, [](const json& e) { return e["ph"] != "M" && e["pid"] == 10 && e["tid"] == 100; });
  ASSERT_EQ(main.size(), 3u);
  EXPECT_EQ(main[0]["name"], "Physics");
  EXPECT_EQ(main[1]["name"], "Hit");
  EXPECT_EQ(main[2]["name"], "Frame");
  // CPU slices are clipped to the range: Main on CPU 0 [1450, 3500), Worker on CPU 1 [1450, 3500).
  auto cpu = eventsWhere(doc, [](const json& e) { return e["ph"] == "X" && e["cat"] == "cpu"; });
  ASSERT_EQ(cpu.size(), 2u);
  for (const json& e : cpu) {
    EXPECT_EQ(toNs(e["ts"]), 950);
    EXPECT_EQ(toNs(e["dur"]), 2050);
  }
  EXPECT_EQ(eventsWhere(doc, [](const json& e) { return e["ph"] == "C"; }).size(),
            1u);  // only ts 1000 < t0? no: 2000
}

TEST(ChromeTrace, ErrorsAreReported) {
  HandModel h = makeModel();
  std::ostringstream os;
  Filter badThread;
  badThread.threads = std::vector<ThreadIdx>{ThreadIdx(9)};
  auto r = exportChromeTrace(h.m, *h.svc, badThread, os);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);

  Filter function;
  function.function = FunctionIdx(0);
  r = exportChromeTrace(h.m, *h.svc, function, os);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Unsupported);

  std::ostringstream bad;
  bad.setstate(std::ios::badbit);
  r = exportChromeTrace(h.m, *h.svc, {}, bad);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::IoError);
}

TEST(ChromeTrace, CpuPseudoProcessAvoidsRealPids) {
  HandModel h = makeModel();
  h.m.threads[1].pid = 0x40000000u;  // the default pseudo pid
  const json doc = exporttest::parseOrFail(exporttest::chromeText(h.m, *h.svc));
  for (const json& e : doc["traceEvents"]) {
    if (e["ph"] == "M" && e["name"] == "process_name" && e["args"]["name"] == "CPU")
      EXPECT_NE(e["pid"], 0x40000000);
  }
}

class ChromeFixture : public ::testing::TestWithParam<const char*> {};

TEST_P(ChromeFixture, MatchesPairedScopesAndThreadStates) {
  LoadedFixture f(GetParam());
  const model::TraceModel& m = f.m;
  const std::string text = exporttest::chromeText(m, *f.svc);
  EXPECT_EQ(text, exporttest::chromeText(m, *f.svc)) << "export is not deterministic";
  const json doc = exporttest::parseOrFail(text);
  ASSERT_TRUE(doc["traceEvents"].is_array());
  const uint64_t hz = m.header.clock_frequency_hz;

  // Events are sorted by timestamp within every (pid, tid) track.
  std::map<std::pair<int64_t, int64_t>, double> lastTs;
  for (const json& e : doc["traceEvents"]) {
    if (e["ph"] == "M") continue;
    const auto key = std::make_pair(e["pid"].get<int64_t>(), e["tid"].get<int64_t>());
    const double ts = e["ts"].get<double>();
    const auto it = lastTs.find(key);
    if (it != lastTs.end()) EXPECT_GE(ts, it->second) << "track " << key.first << "/" << key.second;
    lastTs[key] = ts;
    EXPECT_GE(ts, 0.0);
    if (e["ph"] == "X") EXPECT_GE(e["dur"].get<double>(), 0.0);
  }

  // Scopes equal the pairScopes instances: count and durations, per thread.
  auto paired = analysis::pairScopes(m);
  ASSERT_TRUE(paired.ok());
  for (uint32_t ti = 0; ti < m.threads.size(); ++ti) {
    const model::Thread& th = m.threads[ti];
    auto events = eventsWhere(doc, [&](const json& e) {
      return e["ph"] == "X" && e["cat"] == "scope" && e["tid"] == th.tid && e["pid"] == th.pid;
    });
    const auto& expected = paired->byThread[ti];
    ASSERT_EQ(events.size(), expected.size()) << "thread " << th.tid;
    std::multiset<std::pair<int64_t, int64_t>> got, want;
    for (const json& e : events) got.insert({toNs(e["ts"]), toNs(e["dur"])});
    for (const analysis::ScopeInstance& s : expected) {
      const auto ns = [&](uint64_t ticks) {
        return static_cast<int64_t>(std::llround(static_cast<double>(ticks) * 1e9 / static_cast<double>(hz)));
      };
      want.insert({static_cast<int64_t>(std::llround(static_cast<double>(s.begin - m.header.start_ts) * 1e9 /
                                                     static_cast<double>(hz))),
                   ns(s.end - s.begin)});
    }
    EXPECT_EQ(got, want) << "thread " << th.tid;
    size_t unterminated = 0;
    for (const analysis::ScopeInstance& s : expected) unterminated += s.unterminated ? 1 : 0;
    size_t flagged = 0;
    for (const json& e : events) flagged += e.contains("args") && e["args"].contains("unterminated") ? 1 : 0;
    EXPECT_EQ(flagged, unterminated);
  }

  // CPU tracks equal the Running intervals of the thread states.
  int64_t cpuPid = -1;
  for (const json& e : doc["traceEvents"])
    if (e["ph"] == "M" && e["name"] == "process_name" && e["args"]["name"] == "CPU") cpuPid = e["pid"];
  std::multiset<std::tuple<int64_t, int64_t, int64_t, int64_t>> got, want;  // cpu, ts, dur, tid
  for (const json& e : doc["traceEvents"])
    if (e["ph"] == "X" && e["pid"] == cpuPid)
      got.insert({e["tid"].get<int64_t>(), toNs(e["ts"]), toNs(e["dur"]), e["args"]["tid"].get<int64_t>()});
  for (uint32_t ti = 0; ti < m.threads.size(); ++ti) {
    for (const model::ThreadStateInterval& iv : m.statesByThread[ti]) {
      if (iv.state != ThreadState::Running || iv.end <= iv.start) continue;
      const auto ns = [&](uint64_t ticks) {
        return static_cast<int64_t>(std::llround(static_cast<double>(ticks) * 1e9 / static_cast<double>(hz)));
      };
      want.insert({static_cast<int64_t>(iv.cpu), ns(iv.start - m.header.start_ts), ns(iv.end - iv.start),
                   static_cast<int64_t>(m.threads[ti].tid)});
    }
  }
  EXPECT_EQ(got, want);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, ChromeFixture,
                         ::testing::Values("tiny", "scopes", "frames", "fibers", "partial"));

TEST(ChromeTrace, FixtureFramesAndMarkersAreExported) {
  LoadedFixture f("frames");
  const json doc = exporttest::parseOrFail(exporttest::chromeText(f.m, *f.svc));
  const auto marks = eventsWhere(doc, [](const json& e) { return e["cat"] == "framemark"; });
  EXPECT_EQ(marks.size(), f.m.frameMarks.size());
  const auto markers = eventsWhere(doc, [](const json& e) { return e["cat"] == "marker"; });
  EXPECT_EQ(markers.size(), f.m.markers.size());
  size_t plotPoints = 0;
  for (const auto& p : f.m.plots) plotPoints += p.ts.size();
  EXPECT_EQ(eventsWhere(doc, [](const json& e) { return e["ph"] == "C"; }).size(), plotPoints);
}

// Optional: the output loads with python's json module (skipped where python3 is missing).
TEST(ChromeTrace, PythonJsonSmoke) {
  if (std::system("python3 --version > /dev/null 2>&1") != 0) GTEST_SKIP() << "python3 not available";
  LoadedFixture f("scopes");
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "tachyon_wp63_chrome_smoke.json";
  {
    std::ofstream os(path, std::ios::binary);
    ASSERT_TRUE(exportChromeTrace(f.m, *f.svc, {}, os).ok());
  }
  const std::string cmd =
      "python3 -I -c \"import json,sys; d=json.load(open(sys.argv[1])); "
      "assert d['displayTimeUnit']=='ns' and isinstance(d['traceEvents'], list)\" \"" +
      path.string() + "\"";
  EXPECT_EQ(std::system(cmd.c_str()), 0);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace
}  // namespace tachyon::exporters
