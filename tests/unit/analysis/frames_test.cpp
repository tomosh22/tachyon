// WP-35: frames(), slowestFrames(), frameSeries() on hand-built models and on the `frames` fixture.
#include "analysis/query.h"

#include "aggregation_test_util.h"
#include "analysis/frames.h"
#include "analysis/query_cache.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::TestFunction;

const std::vector<TestFunction> kFns = {{"A", 0x1000, 0x100}, {"B", 0x1100, 0x100}, {"C", 0x1200, 0x100}};
constexpr uint64_t kA = 0x1000, kB = 0x1100, kC = 0x1200;

FunctionKey fk(uint64_t start) {
  return FunctionKey{ModuleIdx(0), start};
}

struct FrameModel {
  HandModel hm{3};
  StringIdx frameName, otherName;
  CallstackIdx sA, sAB, sB, sC;

  FrameModel() {
    model::TraceModel& m = hm.m;
    frameName = m.strings.intern("Frame");
    otherName = m.strings.intern("Other");
    mark(0, 1000, 10, frameName);
    mark(0, 2000, 11, frameName);
    mark(0, 3500, 12, frameName);
    mark(0, 4000, 13, frameName);
    mark(1, 1500, 100, otherName);
    mark(1, 2500, 101, otherName);
    hm.state(0, 0, 1500, ThreadState::Running);
    hm.state(0, 1500, 2500, ThreadState::Waiting);
    hm.state(0, 2500, 10000, ThreadState::Running);
    hm.state(1, 0, 10000, ThreadState::Running);
    hm.state(2, 0, 10000, ThreadState::Waiting);
    sA = hm.stack({kA + 0x10});
    sAB = hm.push(sA, kB + 0x10);  // below sA: shares its node
    sB = hm.stack({kB + 0x10});
    sC = hm.stack({kC + 0x10});
    // Frame 0 [1000, 2000)
    for (uint64_t ts : {1100, 1200}) hm.sample(0, ts, sA);
    hm.sample(0, 1300, sAB);
    hm.sample(0, 1400, sC);
    hm.sample(0, 1600, sC);  // Waiting: weight 0
    for (uint64_t ts : {1100, 1200, 1300}) hm.sample(1, ts, sC);
    hm.sample(2, 1500, sA);  // Waiting thread: weight 0
    // Frame 1 [2000, 3500)
    hm.sample(0, 2600, sB);
    hm.sample(0, 2700, sB);
  }
  void mark(uint32_t thread, uint64_t ts, uint64_t number, StringIdx name) {
    model::FrameMark f;
    f.ts = ts;
    f.thread = ThreadIdx(thread);
    f.name = name;
    f.frame_number = number;
    hm.m.frameMarks.push_back(f);
  }
  void finish() { hm.finish(kFns); }
};

const FrameThreadTime* threadEntry(const FrameSummary& f, uint32_t thread) {
  for (const FrameThreadTime& t : f.threads)
    if (t.thread == ThreadIdx(thread)) return &t;
  return nullptr;
}

TEST(FrameSeriesTest, OrderedByMarkCountThenFirstMark) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  const auto series = frameSeries(ctx);
  ASSERT_EQ(series.size(), 2u);
  EXPECT_EQ(series[0].name, fm.frameName);
  EXPECT_EQ(series[0].thread, ThreadIdx(0));
  EXPECT_EQ(series[0].markCount, 4u);
  EXPECT_EQ(series[1].name, fm.otherName);
  EXPECT_EQ(series[1].thread, ThreadIdx(1));
  EXPECT_EQ(series[1].markCount, 2u);
}

TEST(FrameSeriesTest, TieBreaksOnFirstMarkThenThreadThenName) {
  FrameModel fm;
  fm.mark(2, 500, 1, fm.otherName);  // Other on thread 2 now also has... only 1 mark
  fm.mark(2, 600, 2, fm.otherName);  // 2 marks, first mark 500 < 1500
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  const auto series = frameSeries(ctx);
  ASSERT_EQ(series.size(), 3u);
  EXPECT_EQ(series[1].thread, ThreadIdx(2));  // same count as thread 1's series, earlier first mark
  EXPECT_EQ(series[2].thread, ThreadIdx(1));
}

TEST(Frames, DefaultSeriesIsTheOneWithTheMostMarks) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  auto r = frames(ctx, {});
  ASSERT_TRUE(r.ok()) << r.error().toString();
  ASSERT_EQ(r->size(), 3u);  // 4 marks -> 3 frames
  const FrameSummary& f0 = (*r)[0];
  EXPECT_EQ(f0.index, 0u);
  EXPECT_EQ(f0.number, 10u);
  EXPECT_EQ(f0.start, 1000u);
  EXPECT_EQ(f0.end, 2000u);
  EXPECT_EQ(f0.duration, 1000u);
  EXPECT_EQ(f0.markThread, ThreadIdx(0));
  EXPECT_EQ(f0.seriesName, fm.frameName);
  ASSERT_EQ(f0.threads.size(), 2u);  // thread 2 has no Running time and no weighted samples: omitted
  const FrameThreadTime* t0 = threadEntry(f0, 0);
  const FrameThreadTime* t1 = threadEntry(f0, 1);
  ASSERT_TRUE(t0 && t1);
  EXPECT_EQ(t0->runningTicks, 500u);  // [1000, 1500)
  EXPECT_EQ(t0->sampleTicks, 400u);   // 1100..1400; the sample at 1600 weighs 0
  EXPECT_EQ(t1->runningTicks, 1000u);
  EXPECT_EQ(t1->sampleTicks, 300u);
  EXPECT_EQ(f0.threads[0].thread, ThreadIdx(0));  // sorted by thread
  EXPECT_EQ(f0.threads[1].thread, ThreadIdx(1));
  ASSERT_EQ(f0.topFunctions.size(), 3u);
  EXPECT_EQ(f0.topFunctions[0].key, fk(kC));
  EXPECT_EQ(f0.topFunctions[0].exclusiveTicks, 400u);
  EXPECT_EQ(f0.topFunctions[0].samples, 5u);  // zero-weight samples are counted (1600 on thread 0)
  EXPECT_EQ(f0.topFunctions[1].key, fk(kA));
  EXPECT_EQ(f0.topFunctions[1].exclusiveTicks, 200u);
  EXPECT_EQ(f0.topFunctions[1].samples, 3u);  // including the weight-0 sample of the waiting thread 2
  EXPECT_EQ(f0.topFunctions[2].key, fk(kB));
  EXPECT_EQ(f0.topFunctions[2].exclusiveTicks, 100u);

  const FrameSummary& f1 = (*r)[1];
  EXPECT_EQ(f1.number, 11u);
  EXPECT_EQ(f1.duration, 1500u);
  ASSERT_EQ(f1.threads.size(), 2u);
  EXPECT_EQ(threadEntry(f1, 0)->runningTicks, 1000u);  // [2500, 3500)
  EXPECT_EQ(threadEntry(f1, 0)->sampleTicks, 200u);
  EXPECT_EQ(threadEntry(f1, 1)->runningTicks, 1500u);
  EXPECT_EQ(threadEntry(f1, 1)->sampleTicks, 0u);
  ASSERT_EQ(f1.topFunctions.size(), 1u);
  EXPECT_EQ(f1.topFunctions[0].key, fk(kB));
  EXPECT_EQ(f1.topFunctions[0].exclusiveTicks, 200u);

  const FrameSummary& f2 = (*r)[2];
  EXPECT_EQ(f2.index, 2u);
  EXPECT_EQ(f2.number, 12u);
  EXPECT_EQ(f2.duration, 500u);
  EXPECT_TRUE(f2.topFunctions.empty());
  EXPECT_EQ(threadEntry(f2, 0)->runningTicks, 500u);
}

TEST(Frames, TopFunctionsOptionAndTieOrder) {
  FrameModel fm;
  // Frame 2 [3500, 4000): equal exclusive time for B and A, and C: order by samples then key.
  fm.hm.sample(0, 3600, fm.sA);
  fm.hm.sample(0, 3700, fm.sB);
  fm.hm.sample(0, 3800, fm.sC);
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  FrameOptions o;
  o.topFunctions = 2;
  auto r = frames(ctx, {}, o);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ((*r)[2].topFunctions.size(), 2u);
  EXPECT_EQ((*r)[2].topFunctions[0].key, fk(kA));  // all 100 ticks, 1 sample: key order
  EXPECT_EQ((*r)[2].topFunctions[1].key, fk(kB));
  EXPECT_EQ((*r)[0].topFunctions.size(), 2u);  // truncated from 3
  EXPECT_EQ((*r)[0].topFunctions[0].key, fk(kC));
  o.topFunctions = 0;
  auto none = frames(ctx, {}, o);
  ASSERT_TRUE(none.ok());
  EXPECT_TRUE((*none)[0].topFunctions.empty());
  EXPECT_EQ((*none)[0].threads.size(), 2u);
}

TEST(Frames, ExplicitSeriesAndUnknownSeries) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  FrameOptions o;
  o.series = FrameSeries{ThreadIdx(1), fm.otherName, 0};
  auto r = frames(ctx, {}, o);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0].start, 1500u);
  EXPECT_EQ((*r)[0].end, 2500u);
  EXPECT_EQ((*r)[0].number, 100u);
  EXPECT_EQ((*r)[0].markThread, ThreadIdx(1));
  EXPECT_EQ(threadEntry((*r)[0], 1)->runningTicks, 1000u);
  EXPECT_EQ(threadEntry((*r)[0], 0), nullptr);  // thread 0 waits during [1500, 2500) and its sample weighs 0
}

TEST(Frames, UnknownSeriesIsInvalid) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  FrameOptions o;
  o.series = FrameSeries{ThreadIdx(2), fm.frameName, 0};
  auto r = frames(ctx, {}, o);
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
}

TEST(Frames, TimeFilterSelectsOverlappingFramesWholly) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  auto count = [&](uint64_t t0, uint64_t t1) {
    Filter f;
    f.t0 = t0;
    f.t1 = t1;
    auto r = frames(ctx, f);
    EXPECT_TRUE(r.ok());
    return r.ok() ? r->size() : size_t{0};
  };
  EXPECT_EQ(count(0, 1000), 0u);  // frame 0 starts at 1000: [0,1000) does not overlap
  EXPECT_EQ(count(0, 1001), 1u);
  EXPECT_EQ(count(1999, 2000), 1u);  // only frame 0
  EXPECT_EQ(count(2000, 2001), 1u);  // only frame 1: frame 0 ends at 2000
  EXPECT_EQ(count(1500, 3600), 3u);
  EXPECT_EQ(count(4000, 9000), 0u);  // the last mark opens no frame
  EXPECT_EQ(count(5, 5), 0u);
  Filter f;
  f.t0 = 2100;
  f.t1 = 2200;
  auto r = frames(ctx, f);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 1u);
  EXPECT_EQ((*r)[0].duration, 1500u);  // reported whole, not clipped to the filter
  EXPECT_EQ(threadEntry((*r)[0], 0)->runningTicks, 1000u);
}

TEST(Frames, ThreadFilterSelectsReportedThreadsAndFunctions) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  Filter f;
  f.threads = std::vector<ThreadIdx>{ThreadIdx(1)};
  auto r = frames(ctx, f);
  ASSERT_TRUE(r.ok());
  const FrameSummary& f0 = (*r)[0];
  ASSERT_EQ(f0.threads.size(), 1u);
  EXPECT_EQ(f0.threads[0].thread, ThreadIdx(1));
  ASSERT_EQ(f0.topFunctions.size(), 1u);
  EXPECT_EQ(f0.topFunctions[0].key, fk(kC));
  EXPECT_EQ(f0.topFunctions[0].exclusiveTicks, 300u);
}

TEST(Frames, FunctionAndSubtreeFiltersRestrictSamplesNotRunningTime) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  Filter fa;
  fa.function = ctx.functionIdx(fk(kA));  // stacks containing A: samples 1100, 1200 (A) and 1300 (A->B)
  auto r = frames(ctx, fa);
  ASSERT_TRUE(r.ok());
  const FrameSummary& f0 = (*r)[0];
  EXPECT_EQ(threadEntry(f0, 0)->sampleTicks, 300u);
  EXPECT_EQ(threadEntry(f0, 0)->runningTicks, 500u);
  ASSERT_NE(threadEntry(f0, 1), nullptr);
  EXPECT_EQ(threadEntry(f0, 1)->sampleTicks, 0u);
  EXPECT_EQ(threadEntry(f0, 1)->runningTicks, 1000u);
  ASSERT_EQ(f0.topFunctions.size(), 2u);
  EXPECT_EQ(f0.topFunctions[0].key, fk(kA));
  EXPECT_EQ(f0.topFunctions[0].exclusiveTicks, 200u);
  EXPECT_EQ(f0.topFunctions[1].key, fk(kB));

  Filter sub;
  sub.subtreeRoot = fm.sA;  // the node of A: the same three samples, stacks cut at A
  auto s = frames(ctx, sub);
  ASSERT_TRUE(s.ok());
  EXPECT_EQ((*s)[0].topFunctions.size(), 2u);
  EXPECT_EQ(threadEntry((*s)[0], 0)->sampleTicks, 300u);
  EXPECT_TRUE((*s)[1].topFunctions.empty());  // the samples of frame 1 are not below A
}

TEST(Frames, WithoutMarksTheListIsEmpty) {
  HandModel hm;
  hm.state(0, 0, 1000, ThreadState::Running);
  hm.sample(0, 10, hm.stack({kA + 0x10}));
  hm.finish(kFns);
  QueryContext ctx(hm.m, *hm.svc);
  auto r = frames(ctx, {});
  ASSERT_TRUE(r.ok());
  EXPECT_TRUE(r->empty());
  auto s = slowestFrames(ctx, {}, 3);
  ASSERT_TRUE(s.ok());
  EXPECT_TRUE(s->empty());
  EXPECT_TRUE(frameSeries(ctx).empty());
}

TEST(Frames, SingleMarkOpensNoFrameAndUnsortedMarksAreOrdered) {
  HandModel hm;
  hm.state(0, 0, 10000, ThreadState::Running);
  const StringIdx n = hm.m.strings.intern("F");
  auto mark = [&](uint64_t ts, uint64_t number) {
    model::FrameMark f;
    f.ts = ts;
    f.thread = ThreadIdx(0);
    f.name = n;
    f.frame_number = number;
    hm.m.frameMarks.push_back(f);
  };
  mark(500, 1);
  hm.finish(kFns);
  {
    QueryContext ctx(hm.m, *hm.svc);
    auto r = frames(ctx, {});
    ASSERT_TRUE(r.ok());
    EXPECT_TRUE(r->empty());
  }
  mark(300, 0);
  mark(500, 2);  // duplicate timestamp: a zero-length frame
  mark(900, 3);
  QueryContext ctx(hm.m, *hm.svc);
  auto r = frames(ctx, {});
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 3u);
  EXPECT_EQ((*r)[0].number, 0u);
  EXPECT_EQ((*r)[0].duration, 200u);
  EXPECT_EQ((*r)[1].number, 1u);  // stable: file order among equal timestamps
  EXPECT_EQ((*r)[1].duration, 0u);
  EXPECT_EQ((*r)[2].number, 2u);
  EXPECT_EQ((*r)[2].duration, 400u);
}

TEST(SlowestFrames, ReturnsTheLongestFramesLongestFirst) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  auto s = slowestFrames(ctx, {}, 2);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(s->size(), 2u);
  EXPECT_EQ((*s)[0].number, 11u);              // 1500
  EXPECT_EQ((*s)[1].number, 10u);              // 1000
  EXPECT_EQ((*s)[0].topFunctions.size(), 1u);  // full detail, same as frames()
  EXPECT_EQ((*s)[1].threads.size(), 2u);
  auto all = slowestFrames(ctx, {}, 99);
  ASSERT_TRUE(all.ok());
  ASSERT_EQ(all->size(), 3u);
  EXPECT_EQ((*all)[2].number, 12u);
  auto zero = slowestFrames(ctx, {}, 0);
  ASSERT_TRUE(zero.ok());
  EXPECT_TRUE(zero->empty());
  // restricted by the filter: only frames overlapping [3000, 4000)
  Filter f;
  f.t0 = 3000;
  f.t1 = 4000;
  auto late = slowestFrames(ctx, f, 1);
  ASSERT_TRUE(late.ok());
  ASSERT_EQ(late->size(), 1u);
  EXPECT_EQ((*late)[0].number, 11u);
}

TEST(SlowestFrames, EqualDurationsOrderByStart) {
  HandModel hm;
  hm.state(0, 0, 10000, ThreadState::Running);
  const StringIdx n = hm.m.strings.intern("F");
  for (uint64_t i = 0; i < 6; ++i) {
    model::FrameMark f;
    f.ts = i * 100;
    f.thread = ThreadIdx(0);
    f.name = n;
    f.frame_number = i;
    hm.m.frameMarks.push_back(f);
  }
  hm.finish();
  QueryContext ctx(hm.m, *hm.svc);
  auto s = slowestFrames(ctx, {}, 3);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(s->size(), 3u);
  EXPECT_EQ((*s)[0].number, 0u);
  EXPECT_EQ((*s)[1].number, 1u);
  EXPECT_EQ((*s)[2].number, 2u);
}

TEST(Frames, CachedResultsAreSharedAndKeyedByOptions) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  auto a = framesShared(ctx, {});
  ASSERT_TRUE(a.ok());
  const CacheStats before = ctx.cacheStats();
  auto b = framesShared(ctx, {});
  ASSERT_TRUE(b.ok());
  EXPECT_EQ(a->get(), b->get());
  EXPECT_EQ(ctx.cacheStats().resultHits, before.resultHits + 1);
  // The explicit default series is the same entry.
  FrameOptions explicitDefault;
  explicitDefault.series = FrameSeries{ThreadIdx(0), fm.frameName, 0};
  auto c = framesShared(ctx, {}, explicitDefault);
  ASSERT_TRUE(c.ok());
  EXPECT_EQ(a->get(), c->get());
  // A different top-N or series or kind is a different entry.
  FrameOptions top1;
  top1.topFunctions = 1;
  auto d = framesShared(ctx, {}, top1);
  ASSERT_TRUE(d.ok());
  EXPECT_NE(a->get(), d->get());
  EXPECT_EQ((*d)->at(0).topFunctions.size(), 1u);
  auto s1 = slowestFramesShared(ctx, {}, 1);
  auto s2 = slowestFramesShared(ctx, {}, 2);
  auto s1b = slowestFramesShared(ctx, {}, 1);
  ASSERT_TRUE(s1.ok() && s2.ok() && s1b.ok());
  EXPECT_NE(s1->get(), s2->get());
  EXPECT_EQ(s1->get(), s1b->get());
  EXPECT_GT(ctx.cache().counters(QueryKind::Frames).hits, 0u);
  EXPECT_GT(ctx.cache().counters(QueryKind::SlowestFrames).hits, 0u);
  EXPECT_GT(ctx.cacheStats().bytes, 0u);
}

TEST(Frames, CancelledQueryReturnsCancelledAndIsNotCached) {
  FrameModel fm;
  fm.finish();
  QueryContext ctx(fm.hm.m, *fm.hm.svc);
  std::atomic<bool> flag{true};
  auto r = frames(ctx, {}, {}, Cancel{&flag});
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.error().code, ErrorCode::Cancelled);
  EXPECT_EQ(ctx.cacheStats().entries, 0u);
  auto s = slowestFrames(ctx, {}, 2, {}, Cancel{&flag});
  ASSERT_FALSE(s.ok());
  EXPECT_EQ(s.error().code, ErrorCode::Cancelled);
  EXPECT_TRUE(frames(ctx, {}).ok());
}

TEST(Frames, ParallelAndSequentialAgreeOnManyFrames) {
  // Enough frames to fan out over several workers; compare against a one-worker context.
  HandModel hm(2);
  hm.state(0, 0, 400000, ThreadState::Running);
  hm.state(1, 0, 400000, ThreadState::Running);
  const StringIdx n = hm.m.strings.intern("F");
  const CallstackIdx sa = hm.stack({kA + 0x10}), sb = hm.stack({kA + 0x10, kB + 0x10});
  for (uint64_t i = 0; i < 3000; ++i) {
    model::FrameMark f;
    f.ts = i * 100;
    f.thread = ThreadIdx(0);
    f.name = n;
    f.frame_number = i;
    hm.m.frameMarks.push_back(f);
    hm.sample(0, i * 100 + 10, i % 3 == 0 ? sa : sb);
    hm.sample(1, i * 100 + 20, sb);
  }
  hm.finish(kFns);
  AggregationOptions one;
  one.threads = 1;
  AggregationOptions many;
  many.threads = 4;
  QueryContext c1(hm.m, *hm.svc, one), c4(hm.m, *hm.svc, many);
  auto a = frames(c1, {});
  auto b = frames(c4, {});
  ASSERT_TRUE(a.ok() && b.ok());
  ASSERT_EQ(a->size(), 2999u);
  ASSERT_EQ(b->size(), a->size());
  for (size_t i = 0; i < a->size(); ++i) {
    ASSERT_EQ((*a)[i].start, (*b)[i].start);
    ASSERT_EQ((*a)[i].threads.size(), (*b)[i].threads.size());
    ASSERT_EQ((*a)[i].topFunctions.size(), (*b)[i].topFunctions.size());
    for (size_t k = 0; k < (*a)[i].topFunctions.size(); ++k) {
      ASSERT_EQ((*a)[i].topFunctions[k].key, (*b)[i].topFunctions[k].key);
      ASSERT_EQ((*a)[i].topFunctions[k].exclusiveTicks, (*b)[i].topFunctions[k].exclusiveTicks);
    }
  }
}

// ---- fixture -----------------------------------------------------------------------------------------

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

struct Loaded {
  explicit Loaded(const std::string& name) : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    std::ifstream f(generated(name + ".expected.json"));
    expected = nlohmann::json::parse(f);
    auto r = model::TraceModel::load(generated(name + ".tachyon"));
    EXPECT_TRUE(r.ok()) << (r.ok() ? "" : r.error().toString());
    if (r.ok()) m = std::move(*r);
    EXPECT_TRUE(buildThreadStates(m).ok());
    EXPECT_TRUE(svc.addSymbolFileOverride(generated(name + ".symbols.json")).ok());
    svc.resolveAllAsync(m);
    svc.waitAll();
  }
  model::TraceModel m;
  nlohmann::json expected;
  symbols::SymbolService svc;
};

std::vector<nlohmann::json> expectedFrames(const Loaded& l, const std::string& name) {
  std::vector<nlohmann::json> out;
  for (const auto& f : l.expected["frames"])
    if (f["name"].get<std::string>() == name) out.push_back(f);
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a["start"] < b["start"]; });
  return out;
}

void expectSeriesMatches(const Loaded& l, const QueryContext& ctx, const std::string& name,
                         ThreadIdx markThread, const Filter& filter, std::optional<uint32_t> onlyTid) {
  FrameOptions o;
  o.series = FrameSeries{markThread, l.m.strings.find(name), 0};
  auto r = frames(ctx, filter, o);
  ASSERT_TRUE(r.ok()) << r.error().toString();
  const auto exp = expectedFrames(l, name);
  ASSERT_FALSE(exp.empty());
  ASSERT_EQ(r->size(), exp.size()) << name;
  for (size_t i = 0; i < exp.size(); ++i) {
    const FrameSummary& f = (*r)[i];
    EXPECT_EQ(f.start, exp[i]["start"].get<uint64_t>()) << name << " frame " << i;
    EXPECT_EQ(f.end, exp[i]["end"].get<uint64_t>()) << name << " frame " << i;
    EXPECT_EQ(f.number, exp[i]["number"].get<uint64_t>()) << name << " frame " << i;
    EXPECT_EQ(f.index, i);
    EXPECT_EQ(f.duration, f.end - f.start);
    std::map<uint32_t, FrameThreadTime> got;
    for (const FrameThreadTime& t : f.threads) got[l.m.threads[t.thread.get()].tid] = t;
    size_t expectedNonZero = 0;
    for (const auto& et : exp[i]["threads"]) {
      const uint32_t tid = et["tid"].get<uint32_t>();
      if (onlyTid && *onlyTid != tid) continue;
      const uint64_t run = et["running_ticks"].get<uint64_t>(), smp = et["sample_ticks"].get<uint64_t>();
      if (run != 0 || smp != 0) ++expectedNonZero;
      const auto it = got.find(tid);
      EXPECT_EQ(it == got.end() ? 0u : it->second.runningTicks, run)
          << name << " frame " << i << " tid " << tid;
      EXPECT_EQ(it == got.end() ? 0u : it->second.sampleTicks, smp)
          << name << " frame " << i << " tid " << tid;
    }
    EXPECT_EQ(f.threads.size(), expectedNonZero) << name << " frame " << i;
  }
}

TEST(FramesFixture, FrameListIsExactForBothSeries) {
  Loaded l("frames");
  QueryContext ctx(l.m, l.svc);
  expectSeriesMatches(l, ctx, "Frame", l.m.tidToThread.at(501), {}, std::nullopt);
  expectSeriesMatches(l, ctx, "RenderFrame", l.m.tidToThread.at(502), {}, std::nullopt);
}

TEST(FramesFixture, DefaultSeriesIsFrame) {
  Loaded l("frames");
  QueryContext ctx(l.m, l.svc);
  const auto series = frameSeries(ctx);
  ASSERT_EQ(series.size(), 2u);
  EXPECT_EQ(series[0].name, l.m.strings.find("Frame"));
  EXPECT_EQ(series[0].markCount, l.expected["counts"]["frame_marks"].get<uint32_t>() - 59u);
  auto r = frames(ctx, {});
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r->size(), expectedFrames(l, "Frame").size());
  EXPECT_EQ(r->front().seriesName, l.m.strings.find("Frame"));
}

TEST(FramesFixture, ThreadFilterReportsOnlyThatThread) {
  Loaded l("frames");
  QueryContext ctx(l.m, l.svc);
  Filter f;
  f.threads = std::vector<ThreadIdx>{l.m.tidToThread.at(503)};
  expectSeriesMatches(l, ctx, "Frame", l.m.tidToThread.at(501), f, 503u);
}

TEST(FramesFixture, TimeFilterKeepsWholeOverlappingFrames) {
  Loaded l("frames");
  QueryContext ctx(l.m, l.svc);
  const auto exp = expectedFrames(l, "Frame");
  Filter f;
  f.t0 = exp[10]["start"].get<uint64_t>() + 1;
  f.t1 = exp[13]["start"].get<uint64_t>() + 1;  // overlaps frames 10..13
  auto r = frames(ctx, f);
  ASSERT_TRUE(r.ok());
  ASSERT_EQ(r->size(), 4u);
  EXPECT_EQ(r->front().index, 10u);
  EXPECT_EQ(r->back().index, 13u);
  EXPECT_EQ(r->front().start, exp[10]["start"].get<uint64_t>());
}

TEST(FramesFixture, SlowestFrameIsTheHitch) {
  Loaded l("frames");
  QueryContext ctx(l.m, l.svc);
  ASSERT_EQ(l.expected["hitch_frames"].size(), 1u);
  const auto& hitch = l.expected["hitch_frames"][0];
  auto s = slowestFrames(ctx, {}, 3);
  ASSERT_TRUE(s.ok());
  ASSERT_EQ(s->size(), 3u);
  EXPECT_EQ((*s)[0].number, hitch["number"].get<uint64_t>());
  EXPECT_EQ((*s)[0].start, hitch["start"].get<uint64_t>());
  EXPECT_EQ((*s)[0].duration, hitch["duration_ticks"].get<uint64_t>());
  EXPECT_GE((*s)[0].duration, (*s)[1].duration);
  EXPECT_GE((*s)[1].duration, (*s)[2].duration);
  // The hitch frame's main-thread time is dominated by the hitch stack.
  ASSERT_FALSE((*s)[0].topFunctions.empty());
  EXPECT_EQ(functionKeyName(ctx, (*s)[0].topFunctions[0].key).find("game.exe!"), 0u);
}

TEST(FramesFixture, TopFunctionsMatchBottomUpOverTheFrameRange) {
  Loaded l("frames");
  QueryContext ctx(l.m, l.svc);
  FrameOptions o;
  o.topFunctions = 5;
  auto r = frames(ctx, {}, o);
  ASSERT_TRUE(r.ok());
  size_t checked = 0;
  for (const FrameSummary& f : *r) {
    Filter range;
    range.t0 = f.start;
    range.t1 = f.end;
    auto rows = bottomUpFunctions(ctx, range);
    ASSERT_TRUE(rows.ok());
    std::map<FunctionKey, uint64_t> excl;
    std::vector<uint64_t> ticks;
    for (const FunctionRow& row : *rows) {
      if (row.exclusiveTicks == 0) continue;
      excl[row.key] = row.exclusiveTicks;
      ticks.push_back(row.exclusiveTicks);
    }
    std::sort(ticks.rbegin(), ticks.rend());
    const size_t n = std::min<size_t>(5, ticks.size());
    ASSERT_EQ(f.topFunctions.size(), n) << "frame " << f.index;
    for (size_t k = 0; k < n; ++k) {
      EXPECT_EQ(f.topFunctions[k].exclusiveTicks, ticks[k]) << "frame " << f.index << " rank " << k;
      ASSERT_TRUE(excl.count(f.topFunctions[k].key));
      EXPECT_EQ(excl[f.topFunctions[k].key], f.topFunctions[k].exclusiveTicks);
    }
    ++checked;
  }
  EXPECT_EQ(checked, expectedFrames(l, "Frame").size());
}

}  // namespace
}  // namespace tachyon::analysis
