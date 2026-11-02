// Shared helpers of lines_test.cpp and instructions_test.cpp (WP-34): a symbol provider with line tables and
// inline chains, and a probe model with running threads.
#pragma once

#include "aggregation_test_util.h"
#include "analysis/query.h"

#include <map>

namespace tachyon::analysis::linetest {

using model::ThreadState;
using testutil::HandModel;

// ---- A provider with line tables and inline chains --------------------------------------------------------

struct LineSpec {
  uint64_t off, size;
  uint32_t line;
  const char* file;  // nullptr = the function's file
};
struct InlineSpec {
  uint64_t off, size;
  const char* function;
  const char* file;
  uint32_t line;
  uint32_t callSiteLine = 0;  // the physical function's call site of the chain (0 = provider can't tell)
  const char* callSiteFile = nullptr;
};
struct FnSpec {
  const char* name;
  const char* file;
  uint64_t start, size;
  std::vector<LineSpec> lines;
  std::vector<InlineSpec> inlines;
};

class LineProvider final : public symbols::ISymbolProvider {
 public:
  LineProvider(symbols::SymbolStringTable& strings, std::vector<FnSpec> fns)
      : m_strings(strings), m_fns(std::move(fns)) {}
  Result<void> load(const model::Module&, const std::filesystem::path&) override { return {}; }

  symbols::ResolvedAddress resolve(uint64_t rva) override {
    symbols::ResolvedAddress r;
    const FnSpec* f = find(rva);
    if (f == nullptr) return r;
    r.resolved = true;
    r.function = m_strings.intern(f->name);
    r.functionStartRva = f->start;
    if (f->file != nullptr) r.file = m_strings.intern(f->file);
    r.line = 0;
    for (const LineSpec& l : f->lines) {
      if (rva >= f->start + l.off && rva < f->start + l.off + l.size) {
        r.file = m_strings.intern(l.file != nullptr ? l.file : f->file);
        r.line = l.line;
      }
    }
    for (const InlineSpec& in : f->inlines) {
      if (rva >= f->start + in.off && rva < f->start + in.off + in.size) {
        r.inlines.push_back(
            symbols::InlineFrame{m_strings.intern(in.function), m_strings.intern(in.file), in.line});
        if (in.callSiteLine != 0) {
          r.callSiteFile = m_strings.intern(in.callSiteFile);
          r.callSiteLine = in.callSiteLine;
        }
      }
    }
    return r;
  }

  Result<std::vector<symbols::LineRange>> linesForFunction(uint64_t start) override {
    for (const FnSpec& f : m_fns) {
      if (f.start != start) continue;
      std::vector<symbols::LineRange> out;
      for (const LineSpec& l : f.lines)
        out.push_back(symbols::LineRange{f.start + l.off, static_cast<uint32_t>(l.size),
                                         m_strings.intern(l.file != nullptr ? l.file : f.file), l.line});
      return out;
    }
    return makeError(ErrorCode::NotFound, "no function");
  }

  Result<symbols::FunctionBounds> functionBounds(uint64_t rva) override {
    const FnSpec* f = find(rva);
    if (f == nullptr) return makeError(ErrorCode::NotFound, "no function");
    return symbols::FunctionBounds{f->start, f->size, m_strings.intern(f->name)};
  }

 private:
  const FnSpec* find(uint64_t rva) const {
    for (const FnSpec& f : m_fns)
      if (rva >= f.start && rva < f.start + f.size) return &f;
    return nullptr;
  }
  symbols::SymbolStringTable& m_strings;
  std::vector<FnSpec> m_fns;
};

constexpr uint64_t kMain = 0x2000, kF = 0x1000, kG = 0x3000, kH = 0x4000, kN = 0x5000, kD = 0x6000,
                   kE = 0x7000, kS = 0x8000, kM = 0x9000;

inline std::vector<FnSpec> specs() {
  return {
      // E: DWARF convention with an exact call site (e.cpp:25) for the whole chain.
      {"E",
       "e.cpp",
       kE,
       0x100,
       {{0x00, 0x30, 20, nullptr}, {0x30, 0x30, 7, "callee.h"}, {0x60, 0xA0, 60, nullptr}},
       {{0x30, 0x30, "Callee", "callee.h", 7, 25, "e.cpp"}, {0x30, 0x30, "Mid", "mid.h", 33, 25, "e.cpp"}}},
      // S: the inlined callee lives in the function's own file; the line-table row (200) is the callee body.
      {"S",
       "s.cpp",
       kS,
       0x100,
       {{0x00, 0x20, 100, nullptr}, {0x20, 0x20, 200, nullptr}, {0x40, 0xC0, 110, nullptr}},
       {{0x20, 0x20, "Callee", "s.cpp", 200, 105, "s.cpp"}}},
      // M: the call site is reported in another file than the function's: not usable, fall back.
      {"M",
       "m.cpp",
       kM,
       0x100,
       {{0x00, 0x30, 10, nullptr}, {0x30, 0x30, 7, "callee.h"}, {0x60, 0xA0, 60, nullptr}},
       {{0x30, 0x30, "Callee", "callee.h", 7, 99, "other.cpp"}}},
      // N: no line information at all (ResolvedAddress::file stays invalid).
      {"N", nullptr, kN, 0x100, {}, {}},
      // D: DWARF/DIA convention. inlines[0] sits at the line-table position (callee.h:7); the outer frame
      // carries
      // the position inside Mid (mid.h:33), not a position of D itself.
      {"D",
       "d.cpp",
       kD,
       0x100,
       {{0x00, 0x30, 10, nullptr}, {0x30, 0x30, 7, "callee.h"}, {0x60, 0xA0, 60, nullptr}},
       {{0x30, 0x30, "Callee", "callee.h", 7}, {0x30, 0x30, "Mid", "mid.h", 33}}},
      {"Main", "main.cpp", kMain, 0x100, {{0x00, 0x80, 100, nullptr}, {0x80, 0x80, 110, nullptr}}, {}},
      // F: lines 10 [0,0x10) 20 [0x10,0x30) 30 [0x30,0x50) 40 [0x50,0x80) 50 [0x80,0x100)
      {"F",
       "f.cpp",
       kF,
       0x100,
       {{0x00, 0x10, 10, nullptr},
        {0x10, 0x20, 20, nullptr},
        {0x30, 0x20, 30, nullptr},
        {0x50, 0x30, 40, nullptr},
        {0x80, 0x80, 50, nullptr}},
       {{0x50, 0x30, "Callee", "f.cpp", 7}}},
      {"G", "g.cpp", kG, 0x100, {{0x00, 0x100, 5, nullptr}}, {}},
      // H: the inlined range [0x50,0x80) is in the other file (DWARF style): call site = nearest own line
      // (30).
      {"H",
       "h.cpp",
       kH,
       0x100,
       {{0x00, 0x30, 10, nullptr},
        {0x30, 0x20, 30, nullptr},
        {0x50, 0x30, 7, "callee.h"},
        {0x80, 0x80, 50, nullptr}},
       {{0x50, 0x30, "Callee", "callee.h", 7}}},
  };
}

constexpr uint64_t kKernelAddr = 0xFFFF800000001000ull;

inline FunctionKey fk(uint64_t start) {
  return FunctionKey{ModuleIdx(0), start};
}

struct Probe {
  HandModel h;
  uint64_t ts = 0;
  explicit Probe(uint32_t threads = 1) : h(threads) {
    for (uint32_t t = 0; t < threads; ++t) h.state(t, 0, 100000, ThreadState::Running);
  }
  void samples(CallstackIdx s, int n, uint32_t thread = 0, uint16_t flags = 0) {
    for (int i = 0; i < n; ++i, ts += 200) h.sample(thread, ts, s, flags);
  }
  void finish() {
    h.svc = std::make_unique<symbols::SymbolService>(std::vector<std::string>{}, std::filesystem::path{});
    h.svc->setLocate(
        [](const model::Module&) -> Result<std::filesystem::path> { return std::filesystem::path("t.sym"); });
    symbols::SymbolStringTable& strings = h.svc->stringSink();
    h.svc->setProviderFactory(
        [&strings](const model::Module&,
                   const std::filesystem::path&) -> std::unique_ptr<symbols::ISymbolProvider> {
          return std::make_unique<LineProvider>(strings, specs());
        });
    h.svc->resolveAllAsync(h.m);
    h.svc->waitAll();
  }
};

inline std::map<uint32_t, LineAgg> byLine(const std::vector<LineAgg>& v) {
  std::map<uint32_t, LineAgg> m;
  for (const LineAgg& l : v) m[l.line] = l;
  return m;
}
inline std::map<uint64_t, uint64_t> byRva(const std::vector<InstructionAgg>& v) {
  std::map<uint64_t, uint64_t> m;
  for (const InstructionAgg& i : v) m[i.rva] = i.ticks;
  return m;
}

}  // namespace tachyon::analysis::linetest
