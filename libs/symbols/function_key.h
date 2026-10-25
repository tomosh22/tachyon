// FunctionKey (spec 3.7): the identity under which sampled functions and instrumentation scopes appear in the
// same call trees.
#pragma once

#include "core/hash.h"
#include "core/strong_id.h"
#include "symbols/symbol_service.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace tachyon::symbols {

// Pseudo-module of instrumentation scopes: FunctionKey{kScopeModule, StringIdx of the scope id in the TRACE's
// string table (TraceModel::strings)}.
inline constexpr ModuleIdx kScopeModule{0xFFFFFFFEu};

// Bit set in FunctionKey::value for an inline-expanded function, see functionKeyFor().
inline constexpr uint64_t kInlineKeyBit = 1ull << 63;

struct FunctionKey {
  ModuleIdx module;
  // Meaning depends on module:
  //  * a real module: functionStartRva of the sampled function (or the frame's own rva if the frame is
  //    unresolved, so unresolved frames group per address; WP-32 renders them as <module>!0x<rva>);
  //  * kScopeModule: the scope's StringIdx (trace string table);
  //  * kInvalidIdx (unmapped frame): the absolute address.
  // With inline expansion, kInlineKeyBit | StringIdx of the inlined function's name (symbol string table).
  uint64_t value = 0;

  // All members: thread-safe (value type).
  constexpr auto operator<=>(const FunctionKey&) const = default;
  bool isScope() const { return module == kScopeModule; }
  bool isUnmapped() const { return !module.valid(); }
  bool isInline() const { return module != kScopeModule && (value & kInlineKeyBit) != 0; }

  // Thread-safe: pure function.
  static constexpr FunctionKey scope(StringIdx id) { return FunctionKey{kScopeModule, id.get()}; }
};

// Thread-safe: pure function.
struct FunctionKeyHash {
  size_t operator()(const FunctionKey& k) const noexcept {
    return static_cast<size_t>(core::hashU64(k.value ^ core::hashU64(k.module.get())));
  }
};

// Thread-safe (reads only what SymbolService::lookup reads). Key of the function a frame belongs to:
//  * unmapped frame (module == kInvalidIdx): {kInvalidIdx, absolute address};
//  * module not (yet) resolved, or the address is in no function: {module, rva};
//  * resolved: {module, functionStartRva} -- or, with expandInline and a non-empty inline chain,
//    {module, kInlineKeyBit | StringIdx(innermost inlined function)} (StringIdx into symbolStrings()).
// Keys of a frame change when its module finishes resolving; build trees after SymbolService::waitAll().
FunctionKey functionKeyFor(const SymbolService& symbols, FrameIdx frame, bool expandInline = false);

}  // namespace tachyon::symbols

template <>
struct std::hash<tachyon::symbols::FunctionKey> : tachyon::symbols::FunctionKeyHash {};
