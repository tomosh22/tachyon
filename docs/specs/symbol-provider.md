<!-- Orchestrator-owned copy of IMPLEMENTATION_PLAN.md Part 3.6. Keep in sync with the plan and the matching header. Changes only via orchestrator. -->

### 3.6 Symbol provider interface (`libs/symbols/symbol_provider.h`)
```cpp
struct ModuleId { uint8_t kind; std::array<uint8_t,32> bytes; uint8_t len; uint32_t age; };   // PDB GUID+age or GNU build-id
struct InlineFrame { StringIdx function; StringIdx file; uint32_t line; };
struct ResolvedAddress { StringIdx function; uint64_t functionStartRva; StringIdx file; uint32_t line; std::vector<InlineFrame> inlines; /* innermost first */ bool resolved; };
class ISymbolProvider { public:
  virtual Result<void> load(const Module&, const std::filesystem::path& symbolFile) = 0;
  virtual ResolvedAddress resolve(uint64_t rva) = 0;                          // thread-safe after load
  virtual Result<std::vector<LineRange>> linesForFunction(uint64_t functionStartRva) = 0;
  virtual Result<FunctionBounds> functionBounds(uint64_t rva) = 0; };
class SymbolLocator { public: Result<std::filesystem::path> locate(const Module&); /* local paths, symstore cache, HTTP servers */ };
class SymbolService { public: SymbolService(std::vector<std::string> servers, std::filesystem::path cacheDir); void resolveAllAsync(TraceModel&, std::function<void(ModuleIdx,SymbolState)> onModule); const ResolvedAddress& lookup(FrameIdx) const; /* memoized */ };
```
Symbol server HTTP layout: `<server>/<pdbfile>/<GUID as 32 upper hex><age hex>/<pdbfile>` (also `file.ptr` redirection and `index2.txt` two-tier). Local cache uses the same two-tier layout under `cacheDir`.

## Orchestrator clarifications (binding; merged with WP-20, 2026-10-25)
Headers `libs/symbols/symbol_provider.h` and `libs/symbols/function_key.h` (`tachyon::symbols`) are now orchestrator-owned.
- `ModuleId` comes from `libs/model/model.h`. `LineRange { rva, size, file, line }`, `FunctionBounds { startRva, size, name }`.
- **Strings:** StringIdx values in `ResolvedAddress`, `InlineFrame`, `LineRange` and `FunctionBounds` index `SymbolService::symbolStrings()` (a `symbols::SymbolStringTable`: append-only, lock-free `get`, views stable for the service's lifetime). They do NOT index `TraceModel::strings`. Scope FunctionKeys (`kScopeModule`) carry trace-string-table indices.
- `FunctionKey { ModuleIdx module; uint64_t value; }`. Resolved: `{module, functionStartRva}`. Unresolved but mapped: `{module, rva}`. Unmapped: `{kInvalidIdx, absolute address}`. Scope: `{kScopeModule = 0xFFFFFFFE, StringIdx}`. Inline (with expandInline): `{module, (1<<63) | symbol StringIdx of the innermost inlined function}`; `isInline()` requires a valid, non-scope module.
- `SymbolService`: the 3.6 constructor plus `setLocate`, `setProviderFactory`, `stringSink()` and `addSymbolFileOverride`. `resolveAllAsync` runs on a worker pool, and onModule fires from worker threads. `lookup` is lock-free, never blocks, and returns a static unresolved value until the module is published. `moduleState()` is valid during the run; `Module.state` is written only by `waitAll()`. Keys change when a module resolves, so build aggregation trees after `waitAll()`.

## Clarification added after the WP-32 review (2026-10-25)
- **Return-address rule:** see docs/specs/analysis-query.md. `SymbolService` gains `const ResolvedAddress& lookupCaller(FrameIdx) const` (lock-free and memoized, with the same publication rules as `lookup`). It resolves `addr_or_rva - 1`. `functionKeyFor` gets a caller variant. Leaf frames, and the interrupted user frame of a kernel-leaf sample, use `lookup`.
