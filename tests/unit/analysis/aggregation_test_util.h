// Hand-built models for the attribution / aggregation tests.
#pragma once

#include "analysis/query.h"
#include "model/model.h"
#include "symbols/symbol_service.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tachyon::analysis::testutil {

// A function of the in-memory symbol table used by HandModel::finish(functions).
struct TestFunction {
  const char* name;
  uint64_t start, size;
};

// Provider over a fixed function table (no lines, no inlines), identical for every module.
class TableProvider final : public symbols::ISymbolProvider {
 public:
  TableProvider(symbols::SymbolStringTable& strings, std::vector<TestFunction> fns)
      : m_strings(strings), m_fns(std::move(fns)) {}
  Result<void> load(const model::Module&, const std::filesystem::path&) override { return {}; }
  symbols::ResolvedAddress resolve(uint64_t rva) override {
    symbols::ResolvedAddress r;
    for (const TestFunction& f : m_fns) {
      if (rva < f.start || rva >= f.start + f.size) continue;
      r.function = m_strings.intern(f.name);
      r.functionStartRva = f.start;
      r.resolved = true;
      break;
    }
    return r;
  }
  Result<std::vector<symbols::LineRange>> linesForFunction(uint64_t) override {
    return makeError(ErrorCode::NotFound, "no lines");
  }
  Result<symbols::FunctionBounds> functionBounds(uint64_t rva) override {
    for (const TestFunction& f : m_fns) {
      if (rva >= f.start && rva < f.start + f.size)
        return symbols::FunctionBounds{f.start, f.size, m_strings.intern(f.name)};
    }
    return makeError(ErrorCode::NotFound, "no function");
  }

 private:
  symbols::SymbolStringTable& m_strings;
  std::vector<TestFunction> m_fns;
};

// One process-less model with `threadCount` threads, one module ("game.exe", never resolved because no
// symbol file exists, so every mapped frame keys as {module, rva}) and helpers to build stacks and samples.
class HandModel {
 public:
  static constexpr uint64_t kInterval = 100;

  explicit HandModel(uint32_t threadCount = 1) {
    m.header.start_ts = 0;
    m.header.end_ts = 100000;
    m.sampleIntervalTicks = kInterval;
    m.nodes.push_back(model::CallstackNode{CallstackIdx::invalid(), FrameIdx::invalid()});  // root
    model::Module mod;
    mod.path = "C:\\bin\\game.exe";
    m.modules.push_back(mod);
    model::Module kmod;
    kmod.path = "/boot/vmlinux";
    m.modules.push_back(kmod);
    for (uint32_t t = 0; t < threadCount; ++t) {
      model::Thread th;
      th.tid = 100 + t;
      m.threads.push_back(th);
      m.tidToThread[th.tid] = ThreadIdx(t);
    }
    m.samplesByThread.resize(threadCount);
    m.statesByThread.resize(threadCount);
    m.scopesByThread.resize(threadCount);
  }

  // Appends a node below `parent` for a frame of module 0 at `rva`.
  CallstackIdx push(CallstackIdx parent, uint64_t rva) { return pushFrame(parent, ModuleIdx(0), rva); }
  // An unmapped frame (absolute address).
  CallstackIdx pushAbs(CallstackIdx parent, uint64_t addr) {
    return pushFrame(parent, ModuleIdx::invalid(), addr);
  }
  // A frame of the kernel module (module 1).
  CallstackIdx pushKernelModule(CallstackIdx parent, uint64_t rva) {
    return pushFrame(parent, ModuleIdx(1), rva);
  }

  CallstackIdx pushFrame(CallstackIdx parent, ModuleIdx module, uint64_t addr) {
    m.frames.push_back(model::Frame{module, addr});
    m.nodes.push_back(model::CallstackNode{parent, FrameIdx(static_cast<uint32_t>(m.frames.size() - 1))});
    return CallstackIdx(static_cast<uint32_t>(m.nodes.size() - 1));
  }

  // Builds a stack from root-first rvas of module 0.
  CallstackIdx stack(std::initializer_list<uint64_t> rvas) {
    CallstackIdx cur(0);
    for (uint64_t r : rvas) cur = push(cur, r);
    return cur;
  }

  void state(uint32_t thread, uint64_t start, uint64_t end, model::ThreadState st) {
    model::ThreadStateInterval iv;
    iv.start = start;
    iv.end = end;
    iv.state = st;
    m.statesByThread[thread].push_back(iv);
  }

  void sample(uint32_t thread, uint64_t ts, CallstackIdx cs, uint16_t flags = 0) {
    auto& c = m.samplesByThread[thread];
    c.ts.push_back(ts);
    c.callstack.push_back(cs);
    c.cpu.push_back(0);
    c.flags.push_back(flags);
  }

  // Must be called once, after building and before querying.
  void finish() { finish({}); }
  // As finish(), but modules resolve against `functions` (RVAs of every mapped module).
  void finish(std::vector<TestFunction> functions) {
    svc = std::make_unique<symbols::SymbolService>(std::vector<std::string>{}, std::filesystem::path{});
    if (!functions.empty()) {
      svc->setLocate([](const model::Module&) -> Result<std::filesystem::path> {
        return std::filesystem::path("t.sym");
      });
      symbols::SymbolStringTable& strings = svc->stringSink();
      svc->setProviderFactory(
          [&strings, functions](const model::Module&,
                                const std::filesystem::path&) -> std::unique_ptr<symbols::ISymbolProvider> {
            return std::make_unique<TableProvider>(strings, functions);
          });
    }
    svc->resolveAllAsync(m);
    svc->waitAll();
  }

  model::TraceModel m;
  std::unique_ptr<symbols::SymbolService> svc;
};

}  // namespace tachyon::analysis::testutil
