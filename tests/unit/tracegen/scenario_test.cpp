#include "scenario.h"
#include "tally.h"
#include "test_util.h"

#include <gtest/gtest.h>

namespace tachyon::tracegen {
namespace {

using nlohmann::json;
using testing_util::miniScenario;

void expectRejected(const json& doc, const std::string& needle) {
  auto r = parseScenario(doc, "x");
  ASSERT_FALSE(r.ok()) << "scenario unexpectedly accepted; expected error containing: " << needle;
  EXPECT_NE(r.error().message.find(needle), std::string::npos) << r.error().message;
}

TEST(Scenario, MinimalScenarioParses) {
  auto r = parseScenario(miniScenario(), "x");
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->name, "mini");
  EXPECT_EQ(r->sampleInterval, 1250u);
  EXPECT_EQ(r->duration, 1'000'000u);  // 100 ms at 10 MHz
  ASSERT_EQ(r->threads.size(), 2u);
  EXPECT_EQ(r->threads[0].intervals.size(), 3u);
  EXPECT_EQ(r->threads[0].intervals[0].duration, 300'100u);
  EXPECT_EQ(r->stacks.size(), 3u);
  // "+0x10" offset lands at rva 0x1110.
  EXPECT_EQ(r->stacks[0].rootToLeaf[1].value, 0x1110u);
}

TEST(Scenario, FallbackNameAndClockOneGigahertz) {
  json d = miniScenario();
  d.erase("name");
  d["clock_frequency_hz"] = 1'000'000'000;
  auto r = parseScenario(d, "fallback");
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->name, "fallback");
  EXPECT_EQ(r->sampleInterval, 125'000u);
  EXPECT_EQ(r->duration, 100'000'000u);
}

TEST(Scenario, GeneratedTablesAreDeterministic) {
  json d = miniScenario();
  d["modules"][0]["functions_generated"] = {{"count", 50}, {"inline_pct", 100}};
  d["stack_gen"] = json::array({{{"prefix", "g"}, {"count", 5}, {"min_depth", 3}, {"max_depth", 6}}});
  auto a = parseScenario(d, "x");
  auto b = parseScenario(d, "x");
  ASSERT_TRUE(a.ok() && b.ok());
  EXPECT_EQ(a->modules[0].functions.size(), 52u);
  EXPECT_EQ(a->stacks.size(), 8u);
  for (size_t i = 0; i < a->modules[0].functions.size(); ++i) {
    EXPECT_EQ(a->modules[0].functions[i].rva, b->modules[0].functions[i].rva);
    EXPECT_EQ(a->modules[0].functions[i].size, b->modules[0].functions[i].size);
    EXPECT_FALSE(a->modules[0].functions[i].lines.empty());
  }
  d["seed"] = 12;
  auto c = parseScenario(d, "x");
  ASSERT_TRUE(c.ok());
  EXPECT_NE(a->stacks[4].rootToLeaf[1].value, c->stacks[4].rootToLeaf[1].value);
}

TEST(Scenario, IntervalGroupsAndFrameHitchesExpand) {
  json d = miniScenario();
  d["threads"][1]["intervals"] = json::array(
      {{{"group",
         json::array({{{"state", "running"}, {"dur_us", 1000}}, {{"state", "waiting"}, {"dur_us", 500}}})},
        {"repeat", 4}},
       {{"state", "running"}}});
  d["frame_marks"] = json::array({{{"tid", 1},
                                   {"period_us", 1000},
                                   {"count", 10},
                                   {"hitches", json::array({{{"frame", 3}, {"extra_us", 5000}}})}}});
  auto r = parseScenario(d, "x");
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->threads[1].intervals.size(), 9u);
  ASSERT_EQ(r->frameMarks.size(), 10u);
  EXPECT_EQ(r->frameMarks[3].ts, 30'000u);
  EXPECT_EQ(r->frameMarks[4].ts - r->frameMarks[3].ts, 60'000u);  // frame 3 is lengthened by 5 ms
  EXPECT_EQ(r->frameMarks[5].ts - r->frameMarks[4].ts, 10'000u);
}

TEST(Scenario, RejectsBadInput) {
  {
    json d = miniScenario();
    d["partial"] = {{"crash_at_ms", 50}, {"torn_tail", true}};
    d["blend"] = true;
    expectRejected(d, "torn_tail");
  }
  {
    json d = miniScenario();
    d.erase("duration_ms");
    expectRejected(d, "duration");
  }
  {
    json d = miniScenario();
    d["sampling_hz"] = 7777;
    expectRejected(d, "multiple of sampling_hz");
  }
  {
    json d = miniScenario();
    d["threads"][0]["mix"][0]["stack"] = "nope";
    expectRejected(d, "unknown stack");
  }
  {
    json d = miniScenario();
    d["stacks"][0]["frames"][1] = "m.exe!missing";
    expectRejected(d, "unknown function");
  }
  {
    json d = miniScenario();
    d["stacks"][0]["frames"][1] = "m.exe!work+0x1000";
    expectRejected(d, "offset beyond function size");
  }
  {
    json d = miniScenario();
    d["modules"][0]["functions"][1]["rva"] = "0x1010";
    expectRejected(d, "overlap");
  }
  {
    json d = miniScenario();
    d["threads"][1]["tid"] = 1;
    expectRejected(d, "duplicate tid");
  }
  {
    json d = miniScenario();  // two running intervals in a row
    d["threads"][0]["intervals"][2]["state"] = "waiting";
    expectRejected(d, "alternate");
  }
  {
    json d = miniScenario();
    d["threads"][0]["intervals"][1]["ready_after_us"] = 10'000;
    expectRejected(d, "ready_after_us must be < dur_us");
  }
  {
    json d = miniScenario();
    d["threads"][0]["extra_samples"] = json::array({{{"at_us", 200'000}, {"stack", "a"}}});
    expectRejected(d, "extra sample beyond trace end");
  }
  {
    json d = miniScenario();
    d["scopes"] = json::array({{{"tid", 1}, {"id", "S"}, {"begin_us", 99'000}, {"dur_us", 5000}}});
    expectRejected(d, "ends at or after the trace end");
  }
  {
    json d = miniScenario();
    d["scopes"] = json::array({{{"tid", 9}, {"id", "S"}, {"begin_us", 1000}, {"dur_us", 500}}});
    expectRejected(d, "unknown tid");
  }
  {
    json d = miniScenario();
    d["scopes"] =
        json::array({{{"tid", 1},
                      {"id", "P"},
                      {"begin_us", 1000},
                      {"dur_us", 500},
                      {"children", json::array({{{"id", "C"}, {"begin_us", 400}, {"dur_us", 500}}})}}});
    expectRejected(d, "extends beyond its parent");
  }
  {
    json d = miniScenario();
    d["threads"][0]["fiber"] = {{"initial", 1}};
    d["scopes"] = json::array({{{"tid", 1}, {"fiber", 2}, {"id", "S"}, {"begin_us", 1000}, {"dur_us", 500}}});
    expectRejected(d, "while that fiber is running");
  }
  {
    json d = miniScenario();
    d["partial"] = true;
    d["scopes"] = json::array({{{"tid", 1}, {"id", "S"}, {"begin_us", 1000}, {"dur_us", 500}}});
    expectRejected(d, "partial scenarios cannot contain scopes");
  }
  EXPECT_FALSE(parseScenario(json::array(), "x").ok());
  EXPECT_FALSE(loadScenario("/definitely/not/here.json").ok());
}

TEST(Scenario, CommittedScenariosAllParse) {
  for (const char* name :
       {"tiny", "medium", "large", "scopes", "fibers", "frames", "recursion", "partial", "partial_torn"}) {
    auto r = loadScenario((testing_util::scenarioDir() / (std::string(name) + ".json")).string());
    ASSERT_TRUE(r.ok()) << name << ": " << r.error().toString();
    EXPECT_EQ(r->name, name);
  }
}

TEST(CompareJson, ToleranceAndExactness) {
  EXPECT_TRUE(compareJson(json{{"a", 1.0}}, json{{"a", 1.0 + 1e-12}}).empty());
  EXPECT_FALSE(compareJson(json{{"a", 1.0}}, json{{"a", 1.0001}}).empty());
  EXPECT_FALSE(compareJson(json{{"a", 5}}, json{{"a", 6}}).empty());
  EXPECT_FALSE(compareJson(json{{"a", 5}}, json{{"b", 5}}).empty());
  EXPECT_NE(
      compareJson(json{{"a", json::array({1, 2, 3})}}, json{{"a", json::array({1, 2, 4})}}).find("/a[2]"),
      std::string::npos);
  EXPECT_FALSE(compareJson(json::array({1}), json::array({1, 2})).empty());
  EXPECT_FALSE(compareJson(json("x"), json(1)).empty());
}

}  // namespace
}  // namespace tachyon::tracegen
