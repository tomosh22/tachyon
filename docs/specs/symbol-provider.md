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
