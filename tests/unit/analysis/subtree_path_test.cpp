// Filter::subtreePath (WP-33b) and the "<no stack>" row of bottomUpFunctions: path semantics in all six
// inline x kernel modes (against the unfiltered top-down tree on `tiny` and `medium`, and with exact values
// on a hand-built model), cache keying, InvalidArgument for an empty path, lines / instructions / stats /
// frames under a path, and the topDown / bottomUp totals with frameless samples.
#include "analysis/instances.h"
#include "analysis/query.h"

#include "aggregation_test_util.h"
#include "analysis/query_cache.h"
#include "analysis/thread_states.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>

namespace tachyon::analysis {
namespace {

using model::TraceModel;
using testutil::HandModel;
using testutil::TestFunction;

constexpr uint64_t kKernelAddr = 0xFFFFFFFF81000000ull;

struct Mode {
  bool inlines;
  KernelFrames kernel;
};

std::vector<Mode> allModes() {
  std::vector<Mode> v;
  for (bool inl : {false, true})
    for (KernelFrames k : {KernelFrames::Show, KernelFrames::Group, KernelFrames::Hide})
      v.push_back({inl, k});
  return v;
}

AggregationOptions optionsOf(const Mode& m) {
  AggregationOptions o;
  o.expandInline = m.inlines;
  o.kernelFrames = m.kernel;
  o.threads = 2;
  return o;
}

std::string modeName(const Mode& m) {
  const char* k = m.kernel == KernelFrames::Show    ? "show"
                  : m.kernel == KernelFrames::Group ? "group"
                                                    : "hide";
  return std::string(m.inlines ? "inline-" : "plain-") + k;
}

std::filesystem::path generated(const std::string& file) {
  return std::filesystem::path(TACHYON_GENERATED_FIXTURE_DIR) / file;
}

struct Loaded {
  explicit Loaded(const std::string& name) : svc(std::vector<std::string>{}, std::filesystem::path{}) {
    auto r = TraceModel::load(generated(name + ".tachyon"));
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

// ---- oracles built from the unfiltered top-down tree (iterative: trees can be deep) ----

using Path = std::vector<FunctionKey>;

// Paths (root's child .. node) of up to `maxCount` nodes of depth <= maxDepth, spread over the tree.
std::vector<Path> samplePaths(const NodeAgg& root, size_t maxDepth, size_t maxCount) {
  std::vector<Path> all;
  struct Item {
    const NodeAgg* node;
    size_t child;
  };
  std::vector<Item> stack{{&root, 0}};
  Path cur;
  while (!stack.empty()) {
    Item& it = stack.back();
    if (it.child == it.node->children.size()) {
      stack.pop_back();
      if (!cur.empty()) cur.pop_back();
      continue;
    }
    const NodeAgg* c = &it.node->children[it.child++];
    cur.push_back(c->key);
    all.push_back(cur);
    if (cur.size() < maxDepth && !c->children.empty()) {
      stack.push_back({c, 0});
    } else {
      cur.pop_back();
    }
  }
  if (all.size() <= maxCount) return all;
  std::vector<Path> out;
  for (size_t i = 0; i < maxCount; ++i) out.push_back(all[i * all.size() / maxCount]);
  return out;
}

const NodeAgg* findNode(const NodeAgg& root, const Path& path) {
  const NodeAgg* n = &root;
  for (const FunctionKey& k : path) {
    const NodeAgg* next = nullptr;
    for (const NodeAgg& c : n->children)
      if (c.key == k) next = &c;
    if (next == nullptr) return nullptr;
    n = next;
  }
  return n;
}

// True when the two trees are identical (key, ticks, sample count, child order).
bool sameTree(const NodeAgg& a, const NodeAgg& b, std::string* why) {
  std::vector<std::pair<const NodeAgg*, const NodeAgg*>> work{{&a, &b}};
  while (!work.empty()) {
    auto [x, y] = work.back();
    work.pop_back();
    if (!(x->key == y->key) || x->inclusiveTicks != y->inclusiveTicks ||
        x->exclusiveTicks != y->exclusiveTicks || x->sampleCount != y->sampleCount ||
        x->children.size() != y->children.size()) {
      if (why) *why = "node mismatch at key value " + std::to_string(x->key.value);
      return false;
    }
    for (size_t i = 0; i < x->children.size(); ++i) work.emplace_back(&x->children[i], &y->children[i]);
  }
  return true;
}

// Function rows of the subtree rooted at `sub`, from the merged tree: exclusive = sum over all nodes of the
// key, inclusive and instances from the nodes with no same-key ancestor inside the subtree.
std::map<FunctionKey, FunctionRow> rowsFromSubtree(const NodeAgg& sub) {
  std::map<FunctionKey, FunctionRow> rows;
  std::map<FunctionKey, int> open;
  struct Item {
    const NodeAgg* node;
    size_t child;
  };
  std::vector<Item> stack;
  auto enter = [&](const NodeAgg* n) {
    FunctionRow& r = rows[n->key];
    r.key = n->key;
    r.exclusiveTicks += n->exclusiveTicks;
    if (open[n->key]++ == 0) {
      r.inclusiveTicks += n->inclusiveTicks;
      r.instances += n->sampleCount;
    }
    stack.push_back({n, 0});
  };
  enter(&sub);
  while (!stack.empty()) {
    Item& it = stack.back();
    if (it.child == it.node->children.size()) {
      --open[it.node->key];
      stack.pop_back();
      continue;
    }
    enter(&it.node->children[it.child++]);
  }
  for (auto i = rows.begin(); i != rows.end();) {
    if (i->second.instances == 0) {
      i = rows.erase(i);
    } else {
      ++i;
    }
  }
  return rows;
}

void checkPaths(Loaded& l, const Mode& mode, size_t maxPaths) {
  const std::string what = modeName(mode);
  QueryContext ctx(l.m, l.svc, optionsOf(mode));
  auto full = topDownTree(ctx, {});
  ASSERT_TRUE(full.ok()) << what;
  const std::vector<Path> paths = samplePaths(*full, 6, maxPaths);
  ASSERT_FALSE(paths.empty()) << what;
  for (const Path& path : paths) {
    const NodeAgg* want = findNode(*full, path);
    ASSERT_NE(want, nullptr);
    Filter f;
    f.subtreePath = path;
    auto got = topDownTree(ctx, f);
    ASSERT_TRUE(got.ok()) << what << ": " << got.error().toString();
    std::string why;
    EXPECT_TRUE(sameTree(*got, *want, &why)) << what << " depth " << path.size() << ": " << why;

    auto rows = bottomUpFunctions(ctx, f);
    ASSERT_TRUE(rows.ok()) << what;
    const auto expect = rowsFromSubtree(*want);
    ASSERT_EQ(rows->size(), expect.size()) << what << " depth " << path.size();
    for (const FunctionRow& r : *rows) {
      const auto it = expect.find(r.key);
      ASSERT_NE(it, expect.end()) << what;
      EXPECT_EQ(r.inclusiveTicks, it->second.inclusiveTicks) << what;
      EXPECT_EQ(r.exclusiveTicks, it->second.exclusiveTicks) << what;
      EXPECT_EQ(r.instances, it->second.instances) << what;
    }

    auto bf = butterfly(ctx, f, path.back());
    ASSERT_TRUE(bf.ok()) << what;
    EXPECT_EQ(bf->self.inclusiveTicks, want->inclusiveTicks) << what;
    EXPECT_EQ(bf->self.exclusiveTicks, expect.at(path.back()).exclusiveTicks) << what;
    ASSERT_EQ(bf->callers.size(), 1u) << what;
    EXPECT_EQ(bf->callers[0].key, kRootKey) << what;
    EXPECT_EQ(bf->callers[0].inclusiveTicks, want->inclusiveTicks) << what;
    EXPECT_EQ(bf->callees.size(), want->children.size()) << what;
    for (const FunctionRow& c : bf->callees) {
      const NodeAgg* child = findNode(*want, {c.key});
      ASSERT_NE(child, nullptr) << what;
      EXPECT_EQ(c.inclusiveTicks, child->inclusiveTicks) << what;
    }
  }
}

TEST(SubtreePath, TinyAllModesEqualsUnfilteredSubtree) {
  Loaded l("tiny");
  for (const Mode& mode : allModes()) checkPaths(l, mode, 40);
}

TEST(SubtreePath, MediumAllModesEqualsUnfilteredSubtree) {
  Loaded l("medium");
  for (const Mode& mode : allModes()) checkPaths(l, mode, 12);
}

TEST(SubtreePath, TotalsAgreeTinyMedium) {
  for (const char* name : {"tiny", "medium"}) {
    Loaded l(name);
    for (const Mode& mode : allModes()) {
      QueryContext ctx(l.m, l.svc, optionsOf(mode));
      auto tree = topDownTree(ctx, {});
      auto rows = bottomUpFunctions(ctx, {});
      ASSERT_TRUE(tree.ok() && rows.ok());
      uint64_t excl = 0;
      for (const FunctionRow& r : *rows) excl += r.exclusiveTicks;
      EXPECT_EQ(excl, tree->inclusiveTicks) << name << " " << modeName(mode);
    }
  }
}

// ---- hand-built model ----

//   A@0x1000 B@0x2000 C@0x3000 D@0x4000 (0x100 bytes each); samples are 100 ticks and consecutive.
//   s1 A B C  x3 | s2 A B C (other call sites) x2 | s3 A B x4 | s4 A B D x1 | s5 A D x5 |
//   s6 A kernel x2 | frameless x2
struct Hand {
  static constexpr uint64_t kA = 0x1000, kB = 0x2000, kC = 0x3000, kD = 0x4000;

  Hand() {
    const CallstackIdx s1 = h.stack({0x1010, 0x2010, 0x3010});
    const CallstackIdx s2 = h.stack({0x1050, 0x2050, 0x3010});
    const CallstackIdx s3 = h.stack({0x1010, 0x2010});
    const CallstackIdx s4 = h.stack({0x1050, 0x2060, 0x4010});
    const CallstackIdx s5 = h.stack({0x1010, 0x4020});
    const CallstackIdx s6 = h.pushAbs(h.stack({0x1010}), kKernelAddr);
    s1B = h.m.nodes[s1.get()].parent;
    s1A = h.m.nodes[s1B.get()].parent;
    uint64_t ts = 0;
    auto put = [&](CallstackIdx cs, int n) {
      for (int i = 0; i < n; ++i, ts += 100) h.sample(0, ts, cs);
    };
    put(s1, 3);
    put(s2, 2);
    put(s3, 4);
    put(s4, 1);
    put(s5, 5);
    put(s6, 2);
    put(CallstackIdx(0), 2);
    h.state(0, 0, 100000, model::ThreadState::Running);
    h.finish({TestFunction{"A", kA, 0x100}, TestFunction{"B", kB, 0x100}, TestFunction{"C", kC, 0x100},
              TestFunction{"D", kD, 0x100}});
  }
  static FunctionKey key(uint64_t rva) { return FunctionKey{ModuleIdx(0), rva}; }
  static Filter withPath(Path p) {
    Filter f;
    f.subtreePath = std::move(p);
    return f;
  }
  HandModel h;
  CallstackIdx s1A, s1B;
};

TEST(SubtreePath, HandKeepsEveryCallSiteOfThePath) {
  Hand hm;
  QueryContext ctx(hm.h.m, *hm.h.svc);
  auto full = topDownTree(ctx, {});
  ASSERT_TRUE(full.ok());
  EXPECT_EQ(full->inclusiveTicks, 1900u);
  EXPECT_EQ(full->exclusiveTicks, 200u);
  const NodeAgg* b = findNode(*full, {Hand::key(Hand::kA), Hand::key(Hand::kB)});
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->inclusiveTicks, 1000u);

  // Problem 1: one callstack node of B holds only part of the time.
  Filter one;
  one.subtreeRoot = hm.s1B;
  auto viaNode = topDownTree(ctx, one);
  ASSERT_TRUE(viaNode.ok());
  EXPECT_LT(viaNode->inclusiveTicks, 1000u);

  auto viaPath = topDownTree(ctx, Hand::withPath({Hand::key(Hand::kA), Hand::key(Hand::kB)}));
  ASSERT_TRUE(viaPath.ok());
  EXPECT_EQ(viaPath->key, Hand::key(Hand::kB));
  EXPECT_EQ(viaPath->inclusiveTicks, 1000u);
  EXPECT_EQ(viaPath->exclusiveTicks, 400u);
  EXPECT_EQ(viaPath->sampleCount, 10u);
  ASSERT_EQ(viaPath->children.size(), 2u);
  EXPECT_EQ(viaPath->children[0].key, Hand::key(Hand::kC));
  EXPECT_EQ(viaPath->children[0].inclusiveTicks, 500u);
  EXPECT_EQ(viaPath->children[1].key, Hand::key(Hand::kD));
  EXPECT_EQ(viaPath->children[1].inclusiveTicks, 100u);

  auto rows = bottomUpFunctions(ctx, Hand::withPath({Hand::key(Hand::kA), Hand::key(Hand::kB)}));
  ASSERT_TRUE(rows.ok());
  ASSERT_EQ(rows->size(), 3u);  // B, C, D: no A (cut), no "<no stack>"
  EXPECT_EQ((*rows)[0].key, Hand::key(Hand::kC));
  EXPECT_EQ((*rows)[1].key, Hand::key(Hand::kB));
  EXPECT_EQ((*rows)[1].inclusiveTicks, 1000u);
  EXPECT_EQ((*rows)[1].exclusiveTicks, 400u);

  // A path that does not start at the base of the stacks matches nothing.
  auto none = topDownTree(ctx, Hand::withPath({Hand::key(Hand::kB)}));
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->inclusiveTicks, 0u);
  EXPECT_TRUE(none->children.empty());
  auto missing = bottomUpFunctions(ctx, Hand::withPath({Hand::key(Hand::kA), Hand::key(Hand::kC)}));
  ASSERT_TRUE(missing.ok());
  EXPECT_TRUE(missing->empty());
}

TEST(SubtreePath, HandBothSubtreeRootAndPathApply) {
  Hand hm;
  QueryContext ctx(hm.h.m, *hm.h.svc);
  Filter f = Hand::withPath({Hand::key(Hand::kA), Hand::key(Hand::kB)});
  f.subtreeRoot = hm.s1A;  // only the stack s1 passes through this node
  auto t = topDownTree(ctx, f);
  ASSERT_TRUE(t.ok());
  EXPECT_EQ(t->key, Hand::key(Hand::kB));
  EXPECT_EQ(t->inclusiveTicks, 300u);
  // After the subtreeRoot cut the stack starts at A: a path that begins at B cannot match.
  f.subtreePath = Path{Hand::key(Hand::kB)};
  auto none = topDownTree(ctx, f);
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->inclusiveTicks, 0u);
}

TEST(SubtreePath, HandEmptyPathIsInvalidArgument) {
  Hand hm;
  QueryContext ctx(hm.h.m, *hm.h.svc);
  Filter f;
  f.subtreePath = Path{};
  const FunctionKey c = Hand::key(Hand::kC);
  EXPECT_EQ(topDownTree(ctx, f).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(bottomUpFunctions(ctx, f).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(butterfly(ctx, f, c).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(functionStats(ctx, f, c).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(linesForFunction(ctx, f, c).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(instructionsForFunction(ctx, f, c).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(functionInstances(ctx, f, c).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(frames(ctx, f).error().code, ErrorCode::InvalidArgument);
  EXPECT_EQ(lockSummary(ctx, f).error().code, ErrorCode::InvalidArgument);
}

TEST(SubtreePath, HandCacheKeysAreDistinct) {
  Hand hm;
  QueryContext ctx(hm.h.m, *hm.h.svc);
  Filter root, path, both;
  root.subtreeRoot = hm.s1B;
  path.subtreePath = Path{Hand::key(Hand::kA), Hand::key(Hand::kB)};
  both = path;
  both.subtreeRoot = hm.s1A;
  Filter other;  // a different path
  other.subtreePath = Path{Hand::key(Hand::kA), Hand::key(Hand::kD)};
  EXPECT_NE(hashFilter(root), hashFilter(path));
  EXPECT_NE(hashFilter(path), hashFilter(both));
  EXPECT_NE(hashFilter(path), hashFilter(other));
  EXPECT_FALSE(root == path);
  EXPECT_FALSE(path == both);

  for (const Filter* f : {&root, &path, &both, &other}) {
    const uint64_t before = ctx.cacheStats().resultMisses;
    ASSERT_TRUE(bottomUpFunctions(ctx, *f).ok());
    EXPECT_EQ(ctx.cacheStats().resultMisses, before + 1);
  }
  const CacheStats mid = ctx.cacheStats();
  for (const Filter* f : {&root, &path, &both, &other}) ASSERT_TRUE(bottomUpFunctions(ctx, *f).ok());
  EXPECT_EQ(ctx.cacheStats().resultMisses, mid.resultMisses);
  EXPECT_EQ(ctx.cacheStats().resultHits, mid.resultHits + 4);
  // The results really differ (a stale entry would give the same numbers for two keys).
  auto a = bottomUpFunctions(ctx, root);
  auto b = bottomUpFunctions(ctx, path);
  auto c = bottomUpFunctions(ctx, both);
  ASSERT_TRUE(a.ok() && b.ok() && c.ok());
  uint64_t ia = 0, ib = 0, ic = 0;
  for (const FunctionRow& r : *a) ia = std::max(ia, r.inclusiveTicks);
  for (const FunctionRow& r : *b) ib = std::max(ib, r.inclusiveTicks);
  for (const FunctionRow& r : *c) ic = std::max(ic, r.inclusiveTicks);
  EXPECT_EQ(ia, 300u);
  EXPECT_EQ(ib, 1000u);
  EXPECT_EQ(ic, 300u);
}

TEST(SubtreePath, HandLinesInstructionsStatsUnderPath) {
  Hand hm;
  QueryContext ctx(hm.h.m, *hm.h.svc);
  const Filter ab = Hand::withPath({Hand::key(Hand::kA), Hand::key(Hand::kB)});
  const Filter a = Hand::withPath({Hand::key(Hand::kA)});

  // Instructions: frames at or below the cut only; every call site of the path is included.
  auto ic = instructionsForFunction(ctx, ab, Hand::key(Hand::kC));
  ASSERT_TRUE(ic.ok());
  ASSERT_EQ(ic->size(), 1u);
  EXPECT_EQ((*ic)[0].rva, 0x3010u);
  EXPECT_EQ((*ic)[0].ticks, 500u);
  auto ib = instructionsForFunction(ctx, ab, Hand::key(Hand::kB));
  ASSERT_TRUE(ib.ok());
  std::map<uint64_t, uint64_t> got;
  for (const InstructionAgg& i : *ib) got[i.rva] = i.ticks;
  EXPECT_EQ(got, (std::map<uint64_t, uint64_t>{{0x2010, 700}, {0x2050, 200}, {0x2060, 100}}));
  auto ia = instructionsForFunction(ctx, ab, Hand::key(Hand::kA));  // above the cut
  ASSERT_TRUE(ia.ok());
  EXPECT_TRUE(ia->empty());
  auto iaa = instructionsForFunction(ctx, a, Hand::key(Hand::kA));
  ASSERT_TRUE(iaa.ok());
  got.clear();
  for (const InstructionAgg& i : *iaa) got[i.rva] = i.ticks;
  // s1 3 + s3 4 + s5 5 + s6 2 samples at 0x1010; s2 2 + s4 1 at 0x1050.
  EXPECT_EQ(got, (std::map<uint64_t, uint64_t>{{0x1010, 1400}, {0x1050, 300}}));

  // Lines (no line table: everything is line 0).
  auto lb = linesForFunction(ctx, ab, Hand::key(Hand::kB));
  ASSERT_TRUE(lb.ok());
  ASSERT_EQ(lb->size(), 1u);
  EXPECT_EQ((*lb)[0].line, 0u);
  EXPECT_EQ((*lb)[0].runningTicks, 1000u);

  // Stats: the C instance is one run of 5 consecutive samples.
  auto st = functionStats(ctx, ab, Hand::key(Hand::kC));
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(st->count, 1u);
  EXPECT_EQ(st->totalTicks, 500u);
  auto inst = functionInstances(ctx, ab, Hand::key(Hand::kC));
  ASSERT_TRUE(inst.ok());
  ASSERT_EQ((*inst)->size(), 1u);
  EXPECT_EQ((**inst)[0].begin, 0u);
  EXPECT_EQ((**inst)[0].end, 500u);
  auto none =
      functionStats(ctx, Hand::withPath({Hand::key(Hand::kA), Hand::key(Hand::kD)}), Hand::key(Hand::kC));
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->count, 0u);

  // The function filter composes with the path (the stack must still contain C after the cut).
  Filter withFn = ab;
  withFn.function = ctx.functionIdx(Hand::key(Hand::kC));
  auto t = topDownTree(ctx, withFn);
  ASSERT_TRUE(t.ok());
  EXPECT_EQ(t->inclusiveTicks, 500u);
}

TEST(SubtreePath, HandFramesTopFunctionsUnderPath) {
  Hand hm;
  model::FrameMark mark{};
  mark.ts = 0;
  mark.thread = ThreadIdx(0);
  hm.h.m.frameMarks.push_back(mark);
  mark.ts = 2000;
  hm.h.m.frameMarks.push_back(mark);
  QueryContext ctx(hm.h.m, *hm.h.svc);
  auto fr = frames(ctx, Hand::withPath({Hand::key(Hand::kA), Hand::key(Hand::kB)}));
  ASSERT_TRUE(fr.ok()) << fr.error().toString();
  ASSERT_EQ(fr->size(), 1u);
  ASSERT_FALSE((*fr)[0].threads.empty());
  EXPECT_EQ((*fr)[0].threads[0].sampleTicks, 1000u);
  ASSERT_FALSE((*fr)[0].topFunctions.empty());
  EXPECT_EQ((*fr)[0].topFunctions[0].key, Hand::key(Hand::kC));
  EXPECT_EQ((*fr)[0].topFunctions[0].exclusiveTicks, 500u);
}

TEST(SubtreePath, HandKernelModesAndFramelessTotals) {
  Hand hm;
  const FunctionKey a = Hand::key(Hand::kA);
  for (const Mode& mode : allModes()) {
    QueryContext ctx(hm.h.m, *hm.h.svc, optionsOf(mode));
    const std::string what = modeName(mode);
    // The kernel frame as the mode's tree shows it.
    FunctionKey kernel = FunctionKey{ModuleIdx::invalid(), kKernelAddr};
    if (mode.kernel == KernelFrames::Group) kernel = kKernelKey;
    auto tk = topDownTree(ctx, Hand::withPath({a, kernel}));
    ASSERT_TRUE(tk.ok()) << what;
    // Hidden: the kernel frame's time belongs to A, and a path through it matches nothing.
    EXPECT_EQ(tk->inclusiveTicks, mode.kernel == KernelFrames::Hide ? 0u : 200u) << what;
    // The whole A tree: A's own stacks plus the kernel one (all but the frameless samples).
    auto ta = topDownTree(ctx, Hand::withPath({a}));
    ASSERT_TRUE(ta.ok()) << what;
    EXPECT_EQ(ta->inclusiveTicks, 1700u) << what;

    // Problem 2: bottomUp and topDown count the same samples; "<no stack>" in every mode but Hide, where
    // the frameless samples are "<kernel>" in both views.
    auto tree = topDownTree(ctx, {});
    auto rows = bottomUpFunctions(ctx, {});
    ASSERT_TRUE(tree.ok() && rows.ok()) << what;
    uint64_t excl = 0;
    for (const FunctionRow& r : *rows) excl += r.exclusiveTicks;
    EXPECT_EQ(tree->inclusiveTicks, 1900u) << what;
    EXPECT_EQ(excl, 1900u) << what;
    const auto noStack =
        std::find_if(rows->begin(), rows->end(), [](const FunctionRow& r) { return r.key == kRootKey; });
    if (mode.kernel == KernelFrames::Hide) {
      EXPECT_EQ(noStack, rows->end()) << what;
      const auto k =
          std::find_if(rows->begin(), rows->end(), [](const FunctionRow& r) { return r.key == kKernelKey; });
      ASSERT_NE(k, rows->end()) << what;
      EXPECT_EQ(k->exclusiveTicks, 200u) << what;
    } else {
      ASSERT_NE(noStack, rows->end()) << what;
      EXPECT_EQ(noStack->inclusiveTicks, 200u) << what;
      EXPECT_EQ(noStack->exclusiveTicks, 200u) << what;
      EXPECT_EQ(noStack->instances, 2u) << what;
      EXPECT_EQ(tree->exclusiveTicks, 200u) << what;
      auto bf = butterfly(ctx, {}, kRootKey);
      ASSERT_TRUE(bf.ok()) << what;
      EXPECT_EQ(bf->self.inclusiveTicks, 200u) << what;
      EXPECT_TRUE(bf->callers.empty() && bf->callees.empty()) << what;
    }
  }
}

}  // namespace
}  // namespace tachyon::analysis
