#include "symbols/function_key.h"

namespace tachyon::symbols {

FunctionKey functionKeyFor(const SymbolService& symbols, FrameIdx frameIdx, bool expandInline) {
  const model::Frame f = symbols.frame(frameIdx);
  if (!f.module.valid()) return FunctionKey{ModuleIdx::invalid(), f.addr_or_rva};
  const ResolvedAddress& r = symbols.lookup(frameIdx);
  if (!r.resolved) return FunctionKey{f.module, f.addr_or_rva};
  if (expandInline && !r.inlines.empty()) {
    return FunctionKey{f.module, kInlineKeyBit | r.inlines.front().function.get()};
  }
  return FunctionKey{f.module, r.functionStartRva};
}

}  // namespace tachyon::symbols
