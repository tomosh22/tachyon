// Corrupt / truncated / odd inputs, cancellation, progress, and hand-built traces.
#include "model/model.h"
#include "model_test_util.h"
#include "trace/trace_writer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace tachyon::model {
namespace {

using testing_util::TempDir;

std::vector<char> readAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void writeAll(const std::filesystem::path& p, const char* data, size_t n) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(data, static_cast<std::streamsize>(n));
}

// A load result is acceptable if it is an error, or a model that is internally consistent.
void expectErrorOrConsistent(const Result<TraceModel>& r) {
  if (!r.ok()) return;
  const TraceModel& m = *r;
  EXPECT_EQ(m.samplesByThread.size(), m.threads.size());
  for (const SampleColumns& s : m.samplesByThread) {
    EXPECT_TRUE(std::is_sorted(s.ts.begin(), s.ts.end()));
    for (CallstackIdx cs : s.callstack) ASSERT_TRUE(!cs.valid() || cs.get() < m.nodes.size());
  }
}

TEST(ModelRobustness, MissingFileIsAnError) {
  auto r = TraceModel::load("/nonexistent/definitely/missing.tachyon");
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(ModelRobustness, TruncatedMediumCopy) {
  TempDir dir;
  const std::vector<char> bytes = readAll(testing_util::fixturePath("medium"));
  ASSERT_GT(bytes.size(), 1000u);
  const auto copy = dir.path() / "medium_cut.tachyon";
  for (size_t keep : {size_t{0}, size_t{10}, size_t{63}, size_t{64}, size_t{100}, bytes.size() / 3,
                      bytes.size() / 2, bytes.size() - 100, bytes.size() - 1}) {
    SCOPED_TRACE(keep);
    writeAll(copy, bytes.data(), keep);
    LoadOptions opt;
    opt.threads = 2;
    auto r = TraceModel::load(copy, opt);
    expectErrorOrConsistent(r);
    if (r.ok()) EXPECT_TRUE(r->isPartial);
  }
}

TEST(ModelRobustness, CorruptedBytesNeverCrash) {
  TempDir dir;
  const std::vector<char> bytes = readAll(testing_util::fixturePath("tiny"));
  const auto copy = dir.path() / "tiny_bad.tachyon";
  for (size_t at = 0; at < bytes.size(); at += bytes.size() / 97 + 1) {
    SCOPED_TRACE(at);
    std::vector<char> bad = bytes;
    for (size_t i = at; i < std::min(bad.size(), at + 16); ++i) bad[i] = static_cast<char>(0xA5 ^ i);
    writeAll(copy, bad.data(), bad.size());
    LoadOptions opt;
    opt.threads = 2;
    expectErrorOrConsistent(TraceModel::load(copy, opt));
  }
}

TEST(ModelRobustness, GarbageFile) {
  TempDir dir;
  const auto p = dir.path() / "garbage.tachyon";
  const std::string junk(4096, 'x');
  writeAll(p, junk.data(), junk.size());
  EXPECT_FALSE(TraceModel::load(p).ok());
}

TEST(ModelRobustness, CancelledLoad) {
  std::atomic<bool> cancel{true};
  LoadOptions opt;
  opt.cancel = &cancel;
  auto r = TraceModel::load(testing_util::fixturePath("tiny"), opt);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
}

TEST(ModelRobustness, ProgressIsMonotonicAndEndsAtOne) {
  std::vector<float> seen;
  auto r = TraceModel::load(testing_util::fixturePath("frames"), {}, [&](float p) { seen.push_back(p); });
  ASSERT_TRUE(r.ok());
  ASSERT_FALSE(seen.empty());
  EXPECT_TRUE(std::is_sorted(seen.begin(), seen.end()));
  EXPECT_GE(seen.front(), 0.0f);
  EXPECT_FLOAT_EQ(seen.back(), 1.0f);
}

trace::FileHeader makeHeader() {
  trace::FileHeader h;
  h.clock_frequency_hz = 1'000'000'000;
  h.platform = trace::kPlatformLinux;
  h.start_ts = 0;
  h.end_ts = 1000;
  return h;
}

TEST(ModelHandBuilt, SynthesisedThreadsMergeSortAndMedianInterval) {
  TempDir dir;
  const auto path = dir.path() / "synth.tachyon";
  {
    auto w = trace::TraceWriter::open(path, makeHeader());
    ASSERT_TRUE(w.ok());
    trace::SampleBatch a;  // tid 7: 100,200,300 ; tid 8: 120
    a.push(100, 7, CallstackIdx(0), 1);
    a.push(120, 8, CallstackIdx(0), 2);
    a.push(200, 7, CallstackIdx(0), 1);
    a.push(300, 7, CallstackIdx(0), 1, trace::kSampleFlagKernelLeaf);
    trace::SampleBatch b;  // second chunk goes back in time for tid 7
    b.push(150, 7, CallstackIdx(0), 3);
    b.push(250, 7, CallstackIdx(0), 3);
    w->append(a);
    w->append(b);
    ASSERT_TRUE(w->finish().ok());
  }
  auto r = TraceModel::load(path);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  const TraceModel& m = *r;
  EXPECT_FALSE(m.isPartial);
  ASSERT_EQ(m.threads.size(), 2u);
  // Synthesised in ascending tid order, with no name.
  EXPECT_EQ(m.threads[0].tid, 7u);
  EXPECT_EQ(m.threads[1].tid, 8u);
  EXPECT_EQ(m.threads[0].role, ThreadRole::Unknown);
  EXPECT_FALSE(m.threads[0].is_target);
  const SampleColumns& s7 = m.samplesByThread[m.tidToThread.at(7).get()];
  EXPECT_EQ(s7.ts, (std::vector<uint64_t>{100, 150, 200, 250, 300}));
  EXPECT_EQ(s7.cpu, (std::vector<uint16_t>{1, 3, 1, 3, 1}));
  EXPECT_EQ(s7.flags, (std::vector<uint16_t>{0, 0, 0, 0, trace::kSampleFlagKernelLeaf}));
  // first_ts / last_ts come from the events.
  EXPECT_EQ(m.threads[0].first_ts, 100u);
  EXPECT_EQ(m.threads[0].last_ts, 300u);
  EXPECT_EQ(m.threads[1].first_ts, 120u);
  // No CaptureInfo: median of the deltas (50) is used.
  EXPECT_TRUE(m.captureInfo.raw_json.empty());
  EXPECT_EQ(m.sampleIntervalTicks, 50u);
  // Root node is always present.
  EXPECT_EQ(m.nodes.size(), 1u);
}

TEST(ModelHandBuilt, CaptureInfoDrivesIntervalAndEventsLost) {
  TempDir dir;
  const auto path = dir.path() / "info.tachyon";
  {
    auto w = trace::TraceWriter::open(path, makeHeader());
    ASSERT_TRUE(w.ok());
    w->appendCaptureInfo(R"({"sampling_hz": 8000, "events_lost": 5, "agent": "x"})");
    ASSERT_TRUE(w->finish().ok());
  }
  auto r = TraceModel::load(path);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  EXPECT_EQ(r->captureInfo.sampling_hz, 8000u);
  EXPECT_EQ(r->captureInfo.events_lost, 5u);
  EXPECT_EQ(r->sampleIntervalTicks, 125'000u);  // 1e9 / 8000
  EXPECT_TRUE(r->threads.empty());
}

TEST(ModelHandBuilt, SamplingHzRounds) {
  TempDir dir;
  const auto path = dir.path() / "round.tachyon";
  {
    trace::FileHeader h = makeHeader();
    h.clock_frequency_hz = 10'000'000;
    auto w = trace::TraceWriter::open(path, h);
    ASSERT_TRUE(w.ok());
    w->appendCaptureInfo(R"({"sampling_hz": 3000})");  // 3333.33 -> 3333
    ASSERT_TRUE(w->finish().ok());
  }
  auto r = TraceModel::load(path);
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->sampleIntervalTicks, 3333u);
}

TEST(ModelHandBuilt, DanglingCallstackIsErrorInFinalizedFileAndDroppedInPartial) {
  TempDir dir;
  const auto good = dir.path() / "dangling.tachyon";
  {
    auto w = trace::TraceWriter::open(good, makeHeader());
    ASSERT_TRUE(w.ok());
    trace::SampleBatch s;
    s.push(1, 5, CallstackIdx(99), 0);
    w->append(s);
    ASSERT_TRUE(w->finish().ok());
  }
  auto r = TraceModel::load(good);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::CorruptData);

  const auto partial = dir.path() / "dangling_partial.tachyon";
  {
    auto w = trace::TraceWriter::open(partial, makeHeader());
    ASSERT_TRUE(w.ok());
    trace::SampleBatch s;
    s.push(1, 5, CallstackIdx(99), 0);
    w->append(s);
    // No finish(): the file stays partial.
  }
  auto p = TraceModel::load(partial);
  ASSERT_TRUE(p.ok()) << p.error().toString();
  EXPECT_TRUE(p->isPartial);
  ASSERT_EQ(p->samplesByThread.size(), 1u);
  EXPECT_FALSE(p->samplesByThread[0].callstack[0].valid());
}

}  // namespace
}  // namespace tachyon::model
