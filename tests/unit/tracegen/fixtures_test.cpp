// Checks the fixtures generated at build time (target tachyon_fixtures) against their own contract: the
// committed-to-disk expected.json must equal an independent brute-force recomputation from the .tachyon file,
// and each fixture must contain the edge cases it promises.
#include "reference.h"
#include "test_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace tachyon::tracegen {
namespace {

using nlohmann::json;

std::filesystem::path fixture(const std::string& name, const std::string& ext) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / (name + ext);
}

json expected(const std::string& name) {
  json j = testing_util::readJson(fixture(name, ".expected.json"));
  EXPECT_TRUE(j.is_object()) << name;
  return j;
}

const json* findThread(const json& e, uint32_t tid) {
  for (const json& t : e["threads"])
    if (t["tid"] == tid) return &t;
  return nullptr;
}

const json* findFunction(const json& e, const std::string& name) {
  for (const json& f : e["functions"])
    if (f["name"] == name) return &f;
  return nullptr;
}

class FixtureTest : public ::testing::TestWithParam<const char*> {};

TEST_P(FixtureTest, ExpectedJsonEqualsBruteForceReference) {
  const std::string name = GetParam();
  auto ref = computeReference(fixture(name, ".tachyon"), fixture(name, ".symbols.json"));
  ASSERT_TRUE(ref.ok()) << ref.error().toString();
  EXPECT_EQ(compareJson(expected(name), ref->toJson()), "");
}

TEST_P(FixtureTest, TraceOpensAndMatchesHeaderAndCounts) {
  const std::string name = GetParam();
  auto reader = trace::TraceReader::open(fixture(name, ".tachyon"));
  ASSERT_TRUE(reader.ok()) << reader.error().toString();
  const json e = expected(name);
  EXPECT_EQ(reader->header().clock_frequency_hz, e["clock_frequency_hz"].get<uint64_t>());
  EXPECT_EQ(reader->header().start_ts, e["trace"]["start_ts"].get<uint64_t>());
  EXPECT_EQ(reader->header().end_ts, e["trace"]["end_ts"].get<uint64_t>());
  EXPECT_EQ(reader->isPartial(), e["trace"]["partial"].get<bool>());
  auto samples = reader->readSamples();
  ASSERT_TRUE(samples.ok());
  EXPECT_EQ(samples->size(), e["counts"]["samples"].get<uint64_t>());
  auto info = reader->readCaptureInfo();
  ASSERT_TRUE(info.ok());
  const json ci = json::parse(*info);
  EXPECT_EQ(ci["sampling_hz"].get<uint64_t>(), e["sampling_hz"].get<uint64_t>());
  EXPECT_EQ(ci["events_lost"].get<uint64_t>(), 0u);
  EXPECT_EQ(ci["generator"].get<std::string>().rfind("tracegen ", 0), 0u);
  auto switches = reader->readContextSwitches();
  ASSERT_TRUE(switches.ok());
  EXPECT_EQ(switches->size(), e["counts"]["context_switches"].get<uint64_t>());
  // Samples are globally sorted by timestamp.
  for (size_t i = 1; i < samples->size(); ++i) ASSERT_LE(samples->ts[i - 1], samples->ts[i]) << "at " << i;
  for (size_t i = 1; i < switches->size(); ++i) ASSERT_LE((*switches)[i - 1].ts, (*switches)[i].ts);
}

INSTANTIATE_TEST_SUITE_P(Fixtures, FixtureTest,
                         ::testing::Values("tiny", "medium", "scopes", "fibers", "frames", "recursion",
                                           "partial"));

TEST(TinyFixture, ContainsTheDeliberateEdgeCases) {
  const json e = expected("tiny");
  const json* main = findThread(e, 101);
  const json* worker = findThread(e, 102);
  ASSERT_TRUE(main && worker);
  // Samples outside any Running interval: weight 0, still counted.
  EXPECT_GE((*main)["zero_weight_samples"].get<uint64_t>(), 1u);
  EXPECT_GE((*worker)["zero_weight_samples"].get<uint64_t>(), 2u);
  // Samples whose interval is clipped by the end of the Running interval.
  EXPECT_GE((*main)["clipped_samples"].get<uint64_t>(), 1u);
  // Running at trace start without a switch-in: the first interval is Running from the trace start.
  const json& first = (*main)["intervals"][0];
  EXPECT_EQ(first["state"], "running");
  EXPECT_EQ(first["start"].get<uint64_t>(), e["trace"]["start_ts"].get<uint64_t>());
  // Unknown before a thread's first event.
  EXPECT_EQ((*worker)["intervals"][0]["state"], "unknown");
  EXPECT_EQ((*worker)["state_ticks"]["unknown"].get<uint64_t>(), 1'500'000u);
  // A Ready-split wait: waiting [t, tr) then ready [tr, switch-in), both carrying waker info.
  bool split = false;
  const json& ivs = (*main)["intervals"];
  for (size_t i = 0; i + 1 < ivs.size(); ++i) {
    if (ivs[i]["state"] == "waiting" && ivs[i + 1]["state"] == "ready" &&
        ivs[i]["end"] == ivs[i + 1]["start"] && ivs[i]["woke_by_tid"] == 102 &&
        ivs[i + 1]["woke_by_tid"] == 102 && !ivs[i]["unblocking_stack"].empty())
      split = true;
  }
  EXPECT_TRUE(split);
  // Recursion counted once: fib appears several times in one stack but inclusive samples never exceed
  // samples.
  const json* fib = findFunction(e, "game.exe!Util::fib");
  ASSERT_NE(fib, nullptr);
  EXPECT_EQ((*fib)["inclusive_samples"], (*fib)["exclusive_samples"]);  // fib is always the leaf here
  EXPECT_LT((*fib)["inclusive_samples"].get<uint64_t>(), e["counts"]["samples"].get<uint64_t>());
  // Unmapped frame survives with the absolute address.
  bool unmapped = false;
  for (const json& f : e["functions"])
    if (f["kind"] == "unmapped" && f["start_rva"].get<uint64_t>() == 0x7ffe12345678ull) unmapped = true;
  EXPECT_TRUE(unmapped);
  // Thread 101 is Running overall for most of the second; state totals partition the trace.
  for (const json* t : {main, worker}) {
    uint64_t sum = 0;
    for (const char* k : {"running", "ready", "waiting", "unknown"})
      sum += (*t)["state_ticks"][k].get<uint64_t>();
    EXPECT_EQ(sum, 10'000'000u);
  }
}

TEST(TinyFixture, InclusiveAndExclusiveInvariants) {
  const json e = expected("tiny");
  // Every stack roots at RtlUserThreadStart: its inclusive time is the total weighted sample time.
  const json* root = findFunction(e, "ntdll.dll!RtlUserThreadStart");
  ASSERT_NE(root, nullptr);
  uint64_t totalTicks = 0, exclSum = 0, exclSamples = 0;
  for (const json& t : e["threads"]) totalTicks += t["sample_ticks"].get<uint64_t>();
  EXPECT_EQ((*root)["inclusive_ticks"].get<uint64_t>(), totalTicks);
  EXPECT_EQ((*root)["exclusive_ticks"].get<uint64_t>(), 0u);
  for (const json& f : e["functions"]) {
    exclSum += f["exclusive_ticks"].get<uint64_t>();
    exclSamples += f["exclusive_samples"].get<uint64_t>();
    EXPECT_LE(f["exclusive_ticks"].get<uint64_t>(), f["inclusive_ticks"].get<uint64_t>());
    uint64_t perThread = 0;
    for (const json& p : f["per_thread"]) perThread += p["inclusive_ticks"].get<uint64_t>();
    EXPECT_EQ(perThread, f["inclusive_ticks"].get<uint64_t>()) << f["name"];
  }
  EXPECT_EQ(exclSum, totalTicks);  // exclusive time partitions the weighted time
  EXPECT_EQ(exclSamples, e["counts"]["samples"].get<uint64_t>());
}

TEST(MediumFixture, HasTheAdvertisedShape) {
  const json e = expected("medium");
  EXPECT_EQ(e["threads"].size(), 16u);
  const uint64_t samples = e["counts"]["samples"].get<uint64_t>();
  EXPECT_GT(samples, 7'000'000u);  // 16 threads x 60 s x 8 kHz = 7.68 M at 100% duty
  EXPECT_LE(samples, 7'680'000u);
  EXPECT_GT(e["counts"]["context_switches"].get<uint64_t>(), 10'000u);
  EXPECT_GT(e["counts"]["ready_events"].get<uint64_t>(), 1'000u);
}

TEST(ScopesFixture, BlendedPlacementsCoverMatchedAndFallbackCases) {
  const json e = expected("scopes");
  ASSERT_TRUE(e.contains("blended"));
  bool matchedFrame = false, fallback = false, nestedUnderScope = false, unmappedCaller = false;
  for (const json& p : e["blended"]["placements"]) {
    const bool matched = p["matched"].get<bool>();
    if (p["scope"] == "Frame" && matched && p["parent"]["name"] == "game.exe!main") matchedFrame = true;
    if (p["scope"] == "Orphan" && !matched && p["parent"]["name"] == "scope:Frame") fallback = true;
    if (p["scope"] == "Physics" && !matched && p["parent"]["name"] == "scope:Update") nestedUnderScope = true;
    if (p["scope"] == "UnmappedCaller" && matched && p["parent"]["kind"] == "unmapped") unmappedCaller = true;
  }
  EXPECT_TRUE(matchedFrame);
  EXPECT_TRUE(fallback);
  EXPECT_TRUE(nestedUnderScope);
  EXPECT_TRUE(unmappedCaller);
  // Scope exclusive time moves out of the function it was spliced above (rule 3): blended main has less
  // exclusive time than raw main.
  uint64_t rawMain = 0, blendedMain = 0;
  for (const json& f : e["functions"])
    if (f["name"] == "game.exe!main") rawMain = f["exclusive_ticks"].get<uint64_t>();
  for (const json& f : e["blended"]["functions"])
    if (f["name"] == "game.exe!main") blendedMain = f["exclusive_ticks"].get<uint64_t>();
  EXPECT_LT(blendedMain, rawMain);
  // Frame scope stats: 99 instances with durations 18..19 ms.
  for (const json& s : e["scopes"])
    if (s["id"] == "Frame") {
      EXPECT_EQ(s["instances"], 99u);
      EXPECT_GE(s["min_ticks"].get<uint64_t>(), 180'000u);
      EXPECT_LE(s["max_ticks"].get<uint64_t>(), 190'000u);
      EXPECT_LE(s["min_ticks"], s["median_ticks"]);
      EXPECT_LE(s["median_ticks"], s["max_ticks"]);
    }
}

TEST(FibersFixture, ScopeIntervalsAreSplitAtFiberSwitches) {
  const json e = expected("fibers");
  const json* fiber1 = nullptr;
  for (const json& f : e["fibers"])
    if (f["fiber_id"] == 1) fiber1 = &f;
  ASSERT_NE(fiber1, nullptr);
  size_t outerSegments = 0;
  uint64_t outerTicks = 0;
  for (const json& s : (*fiber1)["segments"])
    if (s["scope"] == "Outer") {
      ++outerSegments;
      outerTicks += s["end"].get<uint64_t>() - s["begin"].get<uint64_t>();
    }
  EXPECT_EQ(outerSegments, 3u);
  // Outer spans 400 ms wall time but fiber 1 is switched out for ~2 x 100 ms (plus switch gaps).
  EXPECT_LT(outerTicks, 4'000'000u);
  EXPECT_NEAR(static_cast<double>(outerTicks), 2'000'000.0, 1000.0);
  // 5 registers + 1 unregister + (initial switch_end + 4 x 2) on thread 401 + (1 + 2 x 2) on thread 402.
  EXPECT_EQ(e["counts"]["fiber_events"], 20u);
}

TEST(FramesFixture, PerFrameTotalsPerThread) {
  const json e = expected("frames");
  EXPECT_EQ(e["frames"].size(), 118u + 58u);
  for (const json& f : e["frames"]) {
    const uint64_t len = f["end"].get<uint64_t>() - f["start"].get<uint64_t>();
    EXPECT_GT(len, 0u);
    for (const json& t : f["threads"]) {
      EXPECT_LE(t["running_ticks"].get<uint64_t>(), len);
      // Sample ticks and running ticks agree up to one sample interval per run boundary inside the frame.
      const int64_t d = static_cast<int64_t>(t["running_ticks"].get<uint64_t>()) -
                        static_cast<int64_t>(t["sample_ticks"].get<uint64_t>());
      EXPECT_LE(std::llabs(d), 20 * 1250) << "frame " << f["number"];
    }
  }
}

TEST(RecursionFixture, InclusiveCountedOncePerSample) {
  const json e = expected("recursion");
  const json* fib = findFunction(e, "recursion!fib");
  const json* main = findFunction(e, "recursion!main");
  ASSERT_TRUE(fib && main);
  // Samples from stacks with 4-5 fib frames count once each.
  EXPECT_LT((*fib)["inclusive_samples"].get<uint64_t>(), (*main)["inclusive_samples"].get<uint64_t>());
  EXPECT_EQ((*main)["inclusive_samples"].get<uint64_t>(), e["counts"]["samples"].get<uint64_t>());
  EXPECT_GT((*fib)["inclusive_samples"].get<uint64_t>(), 0u);
  EXPECT_LE((*fib)["inclusive_ticks"].get<uint64_t>(), (*main)["inclusive_ticks"].get<uint64_t>());
  const json* pong = findFunction(e, "recursion!pong");
  const json* ping = findFunction(e, "recursion!ping");
  ASSERT_TRUE(pong && ping);
  EXPECT_EQ((*pong)["exclusive_samples"].get<uint64_t>(), 0u);  // pong is never the leaf
  EXPECT_GT((*ping)["exclusive_samples"].get<uint64_t>(), 0u);
  EXPECT_EQ((*ping)["inclusive_samples"], (*pong)["inclusive_samples"]);  // same stacks contain both
  EXPECT_EQ(e["clock_frequency_hz"].get<uint64_t>(), 1'000'000'000u);
}

TEST(PartialFixture, IsUnfinalizedAndRecoverable) {
  auto reader = trace::TraceReader::open(fixture("partial", ".tachyon"));
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(reader->isPartial());
  EXPECT_EQ(reader->header().footer_offset, 0u);
  const json e = expected("partial");
  EXPECT_TRUE(e["trace"]["partial"].get<bool>());
  // Recovery returns everything that was flushed: the sample count in the expected JSON.
  auto samples = reader->readSamples();
  ASSERT_TRUE(samples.ok());
  EXPECT_EQ(samples->size(), e["counts"]["samples"].get<uint64_t>());
  EXPECT_GT(reader->chunks().size(), 10u);
  // Nothing after the crash time (1.3 s of a 3 s scenario).
  EXPECT_LT(samples->ts.back(), e["trace"]["end_ts"].get<uint64_t>());
  EXPECT_EQ(e["trace"]["end_ts"].get<uint64_t>() - e["trace"]["start_ts"].get<uint64_t>(), 13'000'000u);
}

}  // namespace
}  // namespace tachyon::tracegen
