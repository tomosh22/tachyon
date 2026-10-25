// WP-32b: SymbolService::lookupCaller and functionKeyForCaller (return-address rule: addr_or_rva - 1).
#include "symbols/function_key.h"
#include "symbols/symbol_service.h"

#include <gtest/gtest.h>

#include <memory>

namespace tachyon::symbols {
namespace {

struct Fn {
  const char* name;
  uint64_t start, size;
};
const Fn kFns[] = {{"Main", 0x1000, 0x100}, {"A", 0x1100, 0x100}, {"B", 0x1200, 0x100}};

class TableProvider final : public ISymbolProvider {
 public:
  explicit TableProvider(SymbolStringTable& strings) : m_strings(strings) {}
  Result<void> load(const Module&, const std::filesystem::path&) override { return {}; }
  ResolvedAddress resolve(uint64_t rva) override {
    ResolvedAddress r;
    for (const Fn& f : kFns) {
      if (rva < f.start || rva >= f.start + f.size) continue;
      r.function = m_strings.intern(f.name);
      r.functionStartRva = f.start;
      r.resolved = true;
    }
    return r;
  }
  Result<std::vector<LineRange>> linesForFunction(uint64_t) override {
    return makeError(ErrorCode::NotFound, "none");
  }
  Result<FunctionBounds> functionBounds(uint64_t) override { return makeError(ErrorCode::NotFound, "none"); }

 private:
  SymbolStringTable& m_strings;
};

// Frames: 0 Main@0x1010 (non-leaf), 1 @0x1100 (non-leaf, last-instruction return address), 2 B@0x1210 (leaf
// only), 3 unmapped @0x5000 (non-leaf), 4 @0x1100 of module 1 (leaf only, module without symbols), 5 @0
// (non-leaf).
struct Env {
  model::TraceModel m;
  std::unique_ptr<SymbolService> svc;
  Env() {
    for (int i = 0; i < 2; ++i) {
      model::Module mod;
      mod.path = i == 0 ? "a.exe" : "b.exe";
      m.modules.push_back(mod);
    }
    m.frames = {{ModuleIdx(0), 0x1010},         {ModuleIdx(0), 0x1100}, {ModuleIdx(0), 0x1210},
                {ModuleIdx::invalid(), 0x5000}, {ModuleIdx(1), 0x1100}, {ModuleIdx(0), 0}};
    m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});
    auto push = [&](uint32_t parent, uint32_t frame) {
      m.nodes.push_back(model::CallstackNode{CallstackIdx(parent), FrameIdx(frame)});
      return static_cast<uint32_t>(m.nodes.size() - 1);
    };
    uint32_t n = 0;
    n = push(n, 0);
    n = push(n, 1);
    n = push(n, 3);
    n = push(n, 5);
    n = push(n, 2);  // leaf only
    push(0, 4);      // frame 4: a single-node stack, leaf only
    svc = std::make_unique<SymbolService>(std::vector<std::string>{}, std::filesystem::path{});
    svc->setLocate([](const Module& mod) -> Result<std::filesystem::path> {
      if (mod.path == "b.exe") return makeError(ErrorCode::NotFound, "no symbols");
      return std::filesystem::path("t.sym");
    });
    SymbolStringTable& strings = svc->stringSink();
    svc->setProviderFactory([&strings](const Module&, const std::filesystem::path&) {
      return std::unique_ptr<ISymbolProvider>(new TableProvider(strings));
    });
  }
  void resolve() {
    svc->resolveAllAsync(m);
    svc->waitAll();
  }
  std::string name(const ResolvedAddress& r) const {
    return std::string(svc->symbolStrings().get(r.function));
  }
};

TEST(LookupCallerTest, ResolvesTheCallInstructionBeforeTheReturnAddress) {
  Env s;
  s.resolve();
  // Frame 1 is 0x1100: exact -> A, as a return address (0x10FF) -> Main.
  const ResolvedAddress& exact = s.svc->lookup(FrameIdx(1));
  const ResolvedAddress& caller = s.svc->lookupCaller(FrameIdx(1));
  ASSERT_TRUE(exact.resolved);
  ASSERT_TRUE(caller.resolved);
  EXPECT_EQ(s.name(exact), "A");
  EXPECT_EQ(s.name(caller), "Main");
  EXPECT_EQ(caller.functionStartRva, 0x1000u);
  // Frame 0 (0x1010): both variants are Main.
  EXPECT_EQ(s.name(s.svc->lookupCaller(FrameIdx(0))), "Main");
  // Memoized separately and stable.
  EXPECT_NE(&exact, &caller);
  EXPECT_EQ(&s.svc->lookupCaller(FrameIdx(1)), &caller);
}

TEST(LookupCallerTest, OnlyNonLeafFramesAreComputed) {
  Env s;
  s.resolve();
  EXPECT_TRUE(s.svc->lookup(FrameIdx(2)).resolved);
  EXPECT_FALSE(s.svc->lookupCaller(FrameIdx(2)).resolved);  // frame 2 occurs only as a leaf
  EXPECT_FALSE(s.svc->lookupCaller(FrameIdx(4)).resolved);  // unresolved module + leaf only
}

TEST(LookupCallerTest, UnmappedUnresolvedAndOutOfRangeFramesGiveTheStaticValue) {
  Env s;
  EXPECT_FALSE(s.svc->lookupCaller(FrameIdx(1)).resolved);  // before resolveAllAsync
  s.resolve();
  EXPECT_FALSE(s.svc->lookupCaller(FrameIdx(3)).resolved);  // unmapped
  EXPECT_EQ(&s.svc->lookupCaller(FrameIdx(3)), &unresolvedAddress());
  EXPECT_FALSE(s.svc->lookupCaller(FrameIdx::invalid()).resolved);
  EXPECT_FALSE(s.svc->lookupCaller(FrameIdx(1000)).resolved);
  // rva 0 does not wrap around.
  EXPECT_FALSE(s.svc->lookupCaller(FrameIdx(5)).resolved);
}

TEST(LookupCallerTest, FunctionKeyForCallerFollowsTheCallerLookup) {
  Env s;
  s.resolve();
  EXPECT_EQ(functionKeyFor(*s.svc, FrameIdx(1)), (FunctionKey{ModuleIdx(0), 0x1100}));
  EXPECT_EQ(functionKeyForCaller(*s.svc, FrameIdx(1)), (FunctionKey{ModuleIdx(0), 0x1000}));
  // Unmapped and unresolved frames keep their own address as the key.
  EXPECT_EQ(functionKeyForCaller(*s.svc, FrameIdx(3)), (FunctionKey{ModuleIdx::invalid(), 0x5000}));
  EXPECT_EQ(functionKeyForCaller(*s.svc, FrameIdx(5)), (FunctionKey{ModuleIdx(0), 0}));
  EXPECT_EQ(functionKeyForCaller(*s.svc, FrameIdx(4)), (FunctionKey{ModuleIdx(1), 0x1100}));
}

TEST(LookupCallerTest, ConcurrentReadersSeeEitherUnresolvedOrFinalValues) {
  Env s;
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::thread reader([&] {
    while (!stop.load()) {
      const ResolvedAddress& r = s.svc->lookupCaller(FrameIdx(1));
      if (r.resolved && s.svc->symbolStrings().get(r.function) != "Main") ++bad;
    }
  });
  s.resolve();
  stop = true;
  reader.join();
  EXPECT_EQ(bad.load(), 0);
}

}  // namespace
}  // namespace tachyon::symbols
