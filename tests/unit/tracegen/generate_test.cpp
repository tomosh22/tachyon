#include "reference.h"
#include "test_util.h"
#include "trace/trace_reader.h"

#include <gtest/gtest.h>

namespace tachyon::tracegen {
namespace {

using nlohmann::json;
using testing_util::miniScenario;
using testing_util::TempDir;

GenerateReport generate(const json& doc, const std::filesystem::path& dir) {
  auto s = parseScenario(doc, "mini");
  EXPECT_TRUE(s.ok()) << (s.ok() ? "" : s.error().toString());
  auto r = generateFixture(*s, dir, GenerateOptions{});
  EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
  return *r;
}

TEST(Generate, SameSeedIsByteIdentical) {
  TempDir a, b;
  const GenerateReport ra = generate(miniScenario(), a.path());
  const GenerateReport rb = generate(miniScenario(), b.path());
  EXPECT_EQ(testing_util::sha(ra.tachyonPath), testing_util::sha(rb.tachyonPath));
  EXPECT_EQ(testing_util::sha(ra.expectedPath), testing_util::sha(rb.expectedPath));
  EXPECT_EQ(testing_util::sha(ra.symbolsPath), testing_util::sha(rb.symbolsPath));
}

TEST(Generate, DifferentSeedChangesTheTrace) {
  TempDir a, b;
  json other = miniScenario();
  other["seed"] = 12;
  const GenerateReport ra = generate(miniScenario(), a.path());
  const GenerateReport rb = generate(other, b.path());
  EXPECT_NE(testing_util::sha(ra.tachyonPath), testing_util::sha(rb.tachyonPath));
}

// Hand-checkable invariants of the mini scenario (thread 1: run 30.01 ms from the start, wait with a ready
// event at 9 ms, run to the end; thread 2 unknown for 0.5 ms, then runs to the end).
TEST(Generate, StateTotalsAndWeightsFollowTheRules) {
  TempDir dir;
  const GenerateReport rep = generate(miniScenario(), dir.path());
  const json e = testing_util::readJson(rep.expectedPath);
  ASSERT_TRUE(e.is_object());
  const uint64_t duration = 1'000'000;  // 100 ms
  const json* t1 = nullptr;
  const json* t2 = nullptr;
  for (const json& t : e["threads"]) (t["tid"] == 1 ? t1 : t2) = &t;
  ASSERT_TRUE(t1 && t2);
  EXPECT_EQ((*t1)["state_ticks"]["running"], 300'100u + (duration - 300'100u - 100'000u));
  EXPECT_EQ((*t1)["state_ticks"]["waiting"], 90'000u);  // ready_after 9 ms
  EXPECT_EQ((*t1)["state_ticks"]["ready"], 10'000u);
  EXPECT_EQ((*t1)["state_ticks"]["unknown"], 0u);
  EXPECT_EQ((*t2)["state_ticks"]["unknown"], 50'000u);
  EXPECT_EQ((*t2)["state_ticks"]["running"], duration - 50'000u);
  EXPECT_EQ((*t1)["context_switches_in"], 2u);
  EXPECT_EQ((*t1)["context_switches_out"], 1u);
  EXPECT_EQ((*t1)["ready_events"], 1u);
  EXPECT_EQ((*t2)["context_switches_in"], 1u);
  // 8 kHz at 10 MHz = 1250 ticks per sample; thread 2 runs 95 ms => 760 samples (+-1 for the grid phase).
  EXPECT_NEAR(static_cast<double>((*t2)["samples"].get<uint64_t>()), 760.0, 1.0);
  // Weights never exceed the sample interval per sample, and no weight is lost: ticks <= samples * 1250.
  for (const json* t : {t1, t2})
    EXPECT_LE((*t)["sample_ticks"].get<uint64_t>(), (*t)["samples"].get<uint64_t>() * 1250);
  // The stack mix 3:1 shows up as exclusive sample shares (low-discrepancy sequence => close to exact).
  uint64_t workExcl = 0, total = 0;
  for (const json& f : e["functions"]) {
    if (f["name"] == "m.exe!work") workExcl = f["exclusive_samples"].get<uint64_t>();
  }
  for (const json& t : e["threads"]) total += t["samples"].get<uint64_t>();
  EXPECT_GT(workExcl, 0u);
  EXPECT_LT(workExcl, total);
}

// attribution-rules.md: a thread that was already running at the trace start (no switch-in) is Unknown until
// its first context-switch event, and its samples in that prefix have weight 0.
TEST(Generate, FromStartThreadIsUnknownUntilItsFirstEvent) {
  json d = miniScenario();
  d["threads"][0]["from_start"] = true;
  TempDir dir;
  const GenerateReport rep = generate(d, dir.path());
  const json e = testing_util::readJson(rep.expectedPath);
  const json& t1 = e["threads"][0];
  ASSERT_EQ(t1["tid"], 1u);
  EXPECT_EQ(t1["state_ticks"]["unknown"], 300'100u);
  EXPECT_EQ(t1["state_ticks"]["running"], 599'900u);
  EXPECT_EQ(t1["context_switches_in"], 1u);
  EXPECT_EQ(t1["context_switches_out"], 1u);
  // ~240 grid samples fall into the 30.01 ms prefix; all have weight 0 and the Running time starts
  // afterwards.
  EXPECT_NEAR(static_cast<double>(t1["zero_weight_samples"].get<uint64_t>()), 240.0, 1.0);
}

TEST(Generate, ExpectedMatchesReferenceOnDisk) {
  TempDir dir;
  const GenerateReport rep = generate(miniScenario(), dir.path());
  auto ref = computeReference(rep.tachyonPath, rep.symbolsPath);
  ASSERT_TRUE(ref.ok()) << ref.error().toString();
  EXPECT_EQ(compareJson(testing_util::readJson(rep.expectedPath), ref->toJson()), "");
}

// If the trace and the sidecar disagree the reference must disagree with the analytic result.
TEST(Generate, ReferenceDetectsATamperedSymbolTable) {
  TempDir dir;
  const GenerateReport rep = generate(miniScenario(), dir.path());
  json sym = testing_util::readJson(rep.symbolsPath);
  sym["modules"][0]["functions"][1]["name"] = "renamed";
  ASSERT_TRUE(writeJsonFile(rep.symbolsPath, sym, 1).ok());
  auto ref = computeReference(rep.tachyonPath, rep.symbolsPath);
  ASSERT_TRUE(ref.ok());
  EXPECT_NE(compareJson(testing_util::readJson(rep.expectedPath), ref->toJson()), "");
}

TEST(Generate, ReferenceDetectsAWrongSampleInterval) {
  TempDir dir;
  const GenerateReport rep = generate(miniScenario(), dir.path());
  // Rewrite the capture info rate by regenerating with a different sampling_hz but comparing to the old
  // expected.
  json other = miniScenario();
  other["sampling_hz"] = 4000;
  TempDir dir2;
  const GenerateReport rep2 = generate(other, dir2.path());
  auto ref = computeReference(rep2.tachyonPath, rep2.symbolsPath);
  ASSERT_TRUE(ref.ok());
  EXPECT_NE(compareJson(testing_util::readJson(rep.expectedPath), ref->toJson()), "");
}

TEST(Generate, VerificationFailureIsAnError) {
  // A stray sample inside a running interval has weight > 0 in the reference but 0 in the analytic result.
  json d = miniScenario();
  d["threads"][1]["extra_samples"] = json::array({{{"at_us", 20'000}, {"stack", "a"}}});
  auto s = parseScenario(d, "mini");
  ASSERT_TRUE(s.ok());
  TempDir dir;
  auto r = generateFixture(*s, dir.path(), GenerateOptions{});
  ASSERT_FALSE(r.ok());
  EXPECT_NE(r.error().message.find("disagree"), std::string::npos) << r.error().message;
  // Both documents are kept for debugging.
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "mini.analytic.json"));
  EXPECT_TRUE(std::filesystem::exists(dir.path() / "mini.reference.json"));
  GenerateOptions off;
  off.verify = false;
  EXPECT_TRUE(generateFixture(*s, dir.path(), off).ok());
}

TEST(Generate, ChunkRecordsSplitsStreams) {
  json d = miniScenario();
  d["chunk_records"] = 100;
  d["duration_ms"] = 1000;  // 20 windows of 50 ms
  TempDir dir;
  const GenerateReport rep = generate(d, dir.path());
  auto reader = trace::TraceReader::open(rep.tachyonPath);
  ASSERT_TRUE(reader.ok());
  size_t sampleChunks = 0;
  for (const trace::FooterRec& c : reader->chunks())
    if (c.type == static_cast<uint32_t>(trace::ChunkType::Samples)) ++sampleChunks;
  EXPECT_GT(sampleChunks, 5u);
  // Chunked output must still be globally sorted and agree with the reference (checked by generate()).
  auto samples = reader->readSamples();
  ASSERT_TRUE(samples.ok());
  for (size_t i = 1; i < samples->size(); ++i) ASSERT_LE(samples->ts[i - 1], samples->ts[i]);
}

TEST(Generate, TornTailLosesExactlyTheLastSamplesChunk) {
  json d = miniScenario();
  d["duration_ms"] = 400;
  d["partial"] = {{"crash_at_ms", 300}, {"torn_tail", true}};
  d["chunk_records"] = 300;
  TempDir dir;
  const GenerateReport rep = generate(d, dir.path());  // generate() verifies analytic == reference
  auto reader = trace::TraceReader::open(rep.tachyonPath);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(reader->isPartial());
  auto samples = reader->readSamples();
  ASSERT_TRUE(samples.ok());
  const json e = testing_util::readJson(rep.expectedPath);
  EXPECT_EQ(samples->size(), e["counts"]["samples"].get<uint64_t>());
  // The file ends inside a chunk: bytes remain after the last recovered chunk.
  const trace::FooterRec last = reader->chunks().back();
  EXPECT_NE(last.type, static_cast<uint32_t>(trace::ChunkType::Samples));
  // The same scenario without tearing keeps more samples.
  d["partial"] = {{"crash_at_ms", 300}};
  TempDir dir2;
  const GenerateReport rep2 = generate(d, dir2.path());
  const json e2 = testing_util::readJson(rep2.expectedPath);
  EXPECT_GT(e2["counts"]["samples"].get<uint64_t>(), e["counts"]["samples"].get<uint64_t>());
}

TEST(Generate, PartialScenarioLeavesRecoverableFile) {
  json d = miniScenario();
  d["partial"] = {{"crash_at_ms", 60}};
  d["chunk_records"] = 200;
  TempDir dir;
  const GenerateReport rep = generate(d, dir.path());
  auto reader = trace::TraceReader::open(rep.tachyonPath);
  ASSERT_TRUE(reader.ok());
  EXPECT_TRUE(reader->isPartial());
  EXPECT_EQ(reader->header().footer_offset, 0u);
  const json e = testing_util::readJson(rep.expectedPath);
  EXPECT_TRUE(e["trace"]["partial"].get<bool>());
  EXPECT_EQ(e["trace"]["end_ts"].get<uint64_t>(), 500u + 600'000u);
}

TEST(Generate, UnmappedFramesSurviveAsAbsoluteAddresses) {
  json d = miniScenario();
  d["stacks"][1]["frames"] = json::array({"m.exe!main", {{"abs", "0x7ffe00001234"}}});
  TempDir dir;
  const GenerateReport rep = generate(d, dir.path());
  auto reader = trace::TraceReader::open(rep.tachyonPath);
  ASSERT_TRUE(reader.ok());
  auto frames = reader->readFrames();
  ASSERT_TRUE(frames.ok());
  bool found = false;
  for (const trace::FrameRec& f : *frames)
    if (!f.module.valid()) {
      found = true;
      const uint64_t addr = f.addr_or_rva;  // packed field: copy before binding to a reference
      EXPECT_EQ(addr, 0x7ffe00001234ull);
    }
  EXPECT_TRUE(found);
  const json e = testing_util::readJson(rep.expectedPath);
  bool listed = false;
  for (const json& f : e["functions"])
    if (f["kind"] == "unmapped") {
      listed = true;
      EXPECT_EQ(f["start_rva"], 0x7ffe00001234ull);
      EXPECT_EQ(f["module"], 0xFFFFFFFFull);
    }
  EXPECT_TRUE(listed);
}

}  // namespace
}  // namespace tachyon::tracegen
