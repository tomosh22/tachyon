#include "symbols/function_key.h"

namespace tachyon::symbols {

namespace {

FunctionKey keyFor(const model::Frame& f, const ResolvedAddress& r, bool expandInline) {
  if (!f.module.valid()) return FunctionKey{ModuleIdx::invalid(), f.addr_or_rva};
  if (!r.resolved) return FunctionKey{f.module, f.addr_or_rva};
  if (expandInline && !r.inlines.empty()) {
    return FunctionKey{f.module, kInlineKeyBit | r.inlines.front().function.get()};
  }
  return FunctionKey{f.module, r.functionStartRva};
}

}  // namespace

FunctionKey functionKeyFor(const SymbolService& symbols, FrameIdx frameIdx, bool expandInline) {
  return keyFor(symbols.frame(frameIdx), symbols.lookup(frameIdx), expandInline);
}

FunctionKey functionKeyForCaller(const SymbolService& symbols, FrameIdx frameIdx, bool expandInline) {
  return keyFor(symbols.frame(frameIdx), symbols.lookupCaller(frameIdx), expandInline);
}

}  // namespace tachyon::symbols
