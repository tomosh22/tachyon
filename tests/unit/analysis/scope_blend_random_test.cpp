// WP-53: randomized comparison of the scope blending against a naive, independent implementation of rule 5
// (every scope scanned for every sample, function ranges matched by brute force), over random stacks (with
// unmapped frames, frames in symbol gaps, empty stacks), random nested scopes (callers on last bytes of
// functions, in gaps, at unmapped addresses) and several threads.
#include "aggregation_test_util.h"
#include "analysis/query.h"
#include "scope_test_util.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace tachyon::analysis {
namespace {

using model::ThreadState;
using testutil::HandModel;
using testutil::pathsOf;
using testutil::ScopeBuilder;
using testutil::TestFunction;

struct NaiveFunction {
  const char* name;
  uint64_t start, size;
};

const NaiveFunction kNaive[] = {{"F0", 0x1000, 0x100}, {"F1", 0x1200, 0x100}, {"F2", 0x1400, 0x100},
                                {"F3", 0x1600, 0x100}, {"F4", 0x1800, 0x100}, {"F5", 0x1a00, 0x100}};

struct NaiveFrame {
  bool mapped;
  uint64_t addr;
};

struct NaiveScope {
  std::string id;
  uint64_t begin, end, caller;
  uint32_t depth;
};

const NaiveFunction* functionAt(uint64_t addr) {
  for (const NaiveFunction& f : kNaive)
    if (addr >= f.start && addr < f.start + f.size) return &f;
  return nullptr;
}

std::string hexName(uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

// Blended stack of one sample, root first, as readable names.
std::vector<std::string> naiveBlend(const std::vector<NaiveFrame>& frames,
                                    const std::vector<NaiveScope>& open) {
  const size_t n = frames.size();
  std::vector<const NaiveFunction*> fn(n, nullptr);
  std::vector<std::string> names(n);
  for (size_t i = 0; i < n; ++i) {
    const bool leaf = i + 1 == n;
    if (!frames[i].mapped) {
      names[i] = "<unmapped>!" + hexName(frames[i].addr);
    } else {
      fn[i] = functionAt(frames[i].addr - (leaf ? 0 : 1));  // return-address rule
      // An unresolved frame is keyed {module, address}; functionKeyName() shows the name of a function that
      // starts exactly there (a display quirk of keys that happen to equal a function start).
      const NaiveFunction* same = functionAt(frames[i].addr);
      names[i] =
          fn[i] != nullptr
              ? fn[i]->name
              : (same != nullptr && same->start == frames[i].addr ? same->name : hexName(frames[i].addr));
    }
  }
  // Slot of each scope: the deepest matching frame, monotone; an unmatched scope sits above the first frame.
  std::vector<size_t> slot;
  size_t prev = n == 0 ? 0 : 1;
  for (const NaiveScope& s : open) {
    size_t m = 0;  // 0 = none
    for (size_t i = 0; i < n; ++i) {
      bool hit;
      if (!frames[i].mapped)
        hit = frames[i].addr == s.caller;
      else
        hit = fn[i] != nullptr && s.caller >= fn[i]->start && s.caller < fn[i]->start + fn[i]->size;
      if (hit) m = i + 1;
    }
    prev = std::max(prev, m);
    slot.push_back(prev);
  }
  std::vector<std::string> out;
  size_t next = 0;
  while (next < open.size() && slot[next] == 0) out.push_back("scope:" + open[next++].id);
  for (size_t i = 0; i < n; ++i) {
    out.push_back(names[i]);
    while (next < open.size() && slot[next] == i + 1) out.push_back("scope:" + open[next++].id);
  }
  return out;
}

class RandomBlend : public ::testing::TestWithParam<uint32_t> {};

TEST_P(RandomBlend, MatchesTheNaiveReference) {
  uint64_t rng = 0x9E3779B97F4A7C15ull * (GetParam() + 1);
  auto rnd = [&](uint32_t n) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return static_cast<uint32_t>(rng % n);
  };
  constexpr uint32_t kThreads = 3;
  HandModel h(kThreads);
  ScopeBuilder sc(h);
  std::vector<TestFunction> table;
  for (const NaiveFunction& f : kNaive) table.push_back(TestFunction{f.name, f.start, f.size});
  const uint64_t kAbs = 0x7ffe00001000ull;

  struct Stack {
    CallstackIdx node;
    std::vector<NaiveFrame> frames;
  };
  std::vector<Stack> stacks;
  for (int i = 0; i < 12; ++i) {
    Stack s;
    s.node = CallstackIdx(0);
    const uint32_t depth = rnd(6);  // 0..5 frames (0 = empty stack)
    for (uint32_t d = 0; d < depth; ++d) {
      NaiveFrame f;
      if (rnd(8) == 0) {
        f = {false, kAbs + 4 * rnd(3)};
        s.node = h.pushAbs(s.node, f.addr);
      } else {
        f = {true, 0x1000 + rnd(0x1c00)};  // inside a function, in a gap, or past the last function
        s.node = h.push(s.node, f.addr);
      }
      s.frames.push_back(f);
    }
    stacks.push_back(std::move(s));
  }

  std::vector<std::vector<NaiveScope>> scopes(kThreads);
  const uint64_t span = 6000;
  for (uint32_t t = 0; t < kThreads; ++t) {
    h.state(t, 0, 100000, ThreadState::Running);
    std::vector<size_t> open;  // indices into scopes[t]
    for (uint64_t ts = 10; ts < span; ts += 1 + rnd(120)) {
      const bool begin = open.empty() || (open.size() < 4 && rnd(3) != 0);
      if (begin) {
        NaiveScope s;
        s.id = "S" + std::to_string(rnd(7));
        s.begin = ts;
        s.end = 0;
        s.depth = static_cast<uint32_t>(open.size());
        const uint32_t kind = rnd(4);
        s.caller = kind == 0   ? kAbs + 4 * rnd(3)
                   : kind == 1 ? kNaive[rnd(6)].start + 0xFF  // last byte of a function
                               : 0x1000 + rnd(0x1c00);
        open.push_back(scopes[t].size());
        scopes[t].push_back(s);
        sc.begin(ts, s.id.c_str(), s.caller, 100 + t);
      } else {
        scopes[t][open.back()].end = ts;
        open.pop_back();
        sc.end(ts, 100 + t);
      }
    }
    for (size_t i : open) scopes[t][i].end = h.m.header.end_ts;  // unterminated
  }

  std::map<std::string, uint64_t> want;  // path -> exclusive ticks
  uint64_t samples = 0;
  for (uint32_t t = 0; t < kThreads; ++t) {
    for (uint64_t ts = 5; ts < span + 100; ts += 7 + rnd(40)) {
      const Stack& st = stacks[rnd(static_cast<uint32_t>(stacks.size()))];
      h.sample(t, ts, st.node);
      std::vector<NaiveScope> open;
      for (const NaiveScope& s : scopes[t])
        if (s.begin <= ts && ts < s.end) open.push_back(s);
      std::sort(open.begin(), open.end(),
                [](const NaiveScope& a, const NaiveScope& b) { return a.depth < b.depth; });
      std::string path;
      for (const std::string& n : naiveBlend(st.frames, open)) path += (path.empty() ? "" : "/") + n;
      want[path] += HandModel::kInterval;
      ++samples;
    }
  }
  sc.sort();
  h.finish(table);
  AggregationOptions on;
  on.blendScopes = true;
  QueryContext ctx(h.m, *h.svc, on);
  auto tree = topDownTree(ctx, {});
  ASSERT_TRUE(tree.ok()) << tree.error().toString();
  EXPECT_EQ(tree->sampleCount, samples);
  // A sample with no frame and no scope is attributed to the root itself: not a path.
  want.erase("");
  auto got = pathsOf(ctx, *tree);
  got.erase("");
  EXPECT_EQ(got, want);
  auto rows = bottomUpFunctions(ctx, {});
  ASSERT_TRUE(rows.ok());
  uint64_t exclusive = 0;
  for (const FunctionRow& r : *rows) exclusive += r.exclusiveTicks;
  EXPECT_LE(exclusive, tree->inclusiveTicks);  // samples with an empty blended stack count only in the root
  // The worker count does not change the result.
  AggregationOptions one = on;
  one.threads = 1;
  QueryContext single(h.m, *h.svc, one);
  auto tree1 = topDownTree(single, {});
  ASSERT_TRUE(tree1.ok());
  auto got1 = pathsOf(single, *tree1);
  got1.erase("");
  EXPECT_EQ(got1, want);
  EXPECT_EQ(tree1->inclusiveTicks, tree->inclusiveTicks);
}

INSTANTIATE_TEST_SUITE_P(Seeds, RandomBlend, ::testing::Range(0u, 40u));

}  // namespace
}  // namespace tachyon::analysis
