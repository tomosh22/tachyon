// WP-64: queries, thread states and the LOD built on a streaming preview (a prefix of the file) are
// consistent with the complete model, and rebuilding on the complete model gives the complete answer.
#include "analysis/lod.h"
#include "analysis/query.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>

namespace tachyon::analysis {
namespace {

using model::TraceModel;

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

struct Prepared {
  Prepared(const std::string& name, double fraction)
      : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    model::LoadOptions opt;
    opt.prefixFraction = fraction;
    auto r = TraceModel::load(generated(name + ".tachyon"), opt);
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(generated(name + ".symbols.json")).ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
  }
  TraceModel m;
  symbols::SymbolService svc;
};

std::map<FunctionKey, uint32_t> instancesByKey(const std::vector<FunctionRow>& rows) {
  std::map<FunctionKey, uint32_t> out;
  for (const FunctionRow& r : rows) out[r.key] = r.instances;
  return out;
}

TEST(StreamingQueries, APreviewAnswersLikeTheCompleteModelOverTheSameSamples) {
  Prepared preview("medium", 0.3);
  Prepared full("medium", 1.0);
  ASSERT_FALSE(preview.m.loadStatus.complete);
  ASSERT_TRUE(full.m.loadStatus.complete);
  // The samples of the preview are exactly the samples of the complete model before the first one that was
  // not loaded; the chunks are time-ordered in the fixture.
  uint64_t horizon = ~0ull;  // smallest first-unloaded timestamp
  for (size_t t = 0; t < full.m.threads.size(); ++t) {
    const size_t n = preview.m.samplesByThread[t].ts.size();
    if (n < full.m.samplesByThread[t].ts.size()) horizon = std::min(horizon, full.m.samplesByThread[t].ts[n]);
  }
  ASSERT_NE(horizon, ~0ull);

  QueryContext pctx(preview.m, preview.svc);
  QueryContext fctx(full.m, full.svc);
  Filter pf;  // the whole preview
  Filter ff;
  ff.t0 = full.m.header.start_ts;
  ff.t1 = horizon;
  pf.t0 = preview.m.header.start_ts;
  pf.t1 = horizon;
  auto pb = bottomUpFunctions(pctx, pf);
  auto fb = bottomUpFunctions(fctx, ff);
  ASSERT_TRUE(pb.ok() && fb.ok());
  EXPECT_EQ(instancesByKey(*pb), instancesByKey(*fb));
  uint64_t total = 0;
  for (const FunctionRow& r : *pb) total += r.exclusiveTicks;
  EXPECT_GT(total, 0u);

  // Topdown root count is the number of samples before the horizon.
  auto ptree = topDownTree(pctx, pf);
  auto ftree = topDownTree(fctx, ff);
  ASSERT_TRUE(ptree.ok() && ftree.ok());
  EXPECT_EQ(ptree->sampleCount, ftree->sampleCount);
  // The unfiltered preview query covers all of its samples.
  auto whole = topDownTree(pctx, Filter{});
  ASSERT_TRUE(whole.ok());
  EXPECT_EQ(whole->sampleCount, preview.m.loadStatus.samplesLoaded);

  // Rebuilding on the complete model answers for the complete model (queries re-run on completion).
  auto fullWhole = topDownTree(fctx, Filter{});
  ASSERT_TRUE(fullWhole.ok());
  EXPECT_EQ(fullWhole->sampleCount, full.m.loadStatus.samplesLoaded);
  EXPECT_GT(fullWhole->sampleCount, whole->sampleCount);
}

TEST(StreamingQueries, ThreadStatesOfAPreviewStayInsideItsRange) {
  Prepared preview("medium", 0.3);
  Prepared full("medium", 1.0);
  const uint64_t end = preview.m.header.end_ts;
  size_t intervals = 0;
  for (size_t t = 0; t < preview.m.statesByThread.size(); ++t) {
    for (const model::ThreadStateInterval& iv : preview.m.statesByThread[t]) {
      ++intervals;
      EXPECT_LE(iv.start, end);
      // States may end at the horizon; nothing starts past it.
      EXPECT_LT(iv.start, end);
    }
    // Everything the preview knows is a prefix of what the complete model knows (intervals before the last
    // one are identical).
    const auto& a = preview.m.statesByThread[t];
    const auto& b = full.m.statesByThread[t];
    if (a.size() > 1) {
      ASSERT_GE(b.size(), a.size() - 1);
      for (size_t i = 0; i + 1 < a.size(); ++i) {
        EXPECT_EQ(a[i].start, b[i].start);
        EXPECT_EQ(a[i].end, b[i].end);
        EXPECT_EQ(a[i].state, b[i].state);
      }
    }
  }
  EXPECT_GT(intervals, 0u);
}

TEST(StreamingQueries, TheLodOfAPreviewCoversItsRangeAndTheCompleteOneTheWholeCapture) {
  Prepared preview("medium", 0.3);
  Prepared full("medium", 1.0);
  auto plod = buildLod(preview.m);
  auto flod = buildLod(full.m);
  ASSERT_TRUE(plod.ok() && flod.ok());
  EXPECT_LE(plod->memoryBytes(), flod->memoryBytes());
  RowQuery q;
  q.thread = ThreadIdx(0);
  q.t0 = full.m.header.start_ts;
  q.t1 = full.m.header.end_ts;
  q.pixelWidth = 400;
  StateBandResult pb, fb;
  SampleStripResult ps, fs;
  ASSERT_TRUE(plod->rowColumns(q, pb, ps).ok());
  ASSERT_TRUE(flod->rowColumns(q, fb, fs).ok());
  // Over the full range the preview knows nothing about the part that was not loaded.
  size_t pbKnown = 0, fbKnown = 0, psKnown = 0, fsKnown = 0;
  for (size_t c = 0; c < 400; ++c) {
    pbKnown += pb.columns[c].unknown < 1.0f ? 1 : 0;
    fbKnown += fb.columns[c].unknown < 1.0f ? 1 : 0;
    psKnown += ps.columns[c].stack.valid() ? 1 : 0;
    fsKnown += fs.columns[c].stack.valid() ? 1 : 0;
  }
  EXPECT_LT(psKnown, fsKnown);
  EXPECT_LE(pbKnown, fbKnown);
  EXPECT_GT(psKnown, 0u);
  // A preview range query inside the loaded part gives the same answer on both (up to the clipped tail).
  q.t1 = preview.m.header.end_ts - preview.m.header.end_ts % 1;  // the preview's own range
  q.t0 = preview.m.header.start_ts;
  StateBandResult pb2, fb2;
  SampleStripResult ps2, fs2;
  ASSERT_TRUE(plod->rowColumns(q, pb2, ps2).ok());
  ASSERT_TRUE(flod->rowColumns(q, fb2, fs2).ok());
  size_t same = 0;
  for (size_t c = 0; c + 2 < 400; ++c) same += ps2.columns[c].stack == fs2.columns[c].stack ? 1 : 0;
  EXPECT_GT(same, 380u);
}

}  // namespace
}  // namespace tachyon::analysis
