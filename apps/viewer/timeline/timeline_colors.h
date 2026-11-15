// Colour mapping of the threads timeline (WP-42): thread state / wait category colours, the blended state
// band, the sample strip and flame chart hashing. Pure functions on packed 0xAABBGGRR colours (the layout of
// ImGui's ImU32), so no ImGui dependency and fully unit-testable.
#pragma once

#include "analysis/lod.h"
#include "analysis/wait_reasons.h"
#include "model/model.h"
#include "symbols/symbol_service.h"
#include "viewer/theme.h"
#include "viewer/timeline/timeline_data.h"

#include <cstdint>

namespace tachyon::viewer::timeline {

using Color32 = uint32_t;  // 0xAABBGGRR

constexpr Color32 makeColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(g) << 8) | r;
}
constexpr uint8_t colorR(Color32 c) {
  return static_cast<uint8_t>(c & 0xFF);
}
constexpr uint8_t colorG(Color32 c) {
  return static_cast<uint8_t>((c >> 8) & 0xFF);
}
constexpr uint8_t colorB(Color32 c) {
  return static_cast<uint8_t>((c >> 16) & 0xFF);
}
constexpr uint8_t colorA(Color32 c) {
  return static_cast<uint8_t>(c >> 24);
}

// WP-67e: the thread state colours come from the active theme (StatePalette, defined once in theme.cpp):
// green Running, red/pink synchronization wait, dark blue I/O, light blue paging/memory, grey other, amber
// Ready, and a background-ish "unknown". Every function below takes the palette to use, so the pure colour
// mapping stays testable and is never tied to one theme.

// All functions in this header: thread-safe, pure.

Color32 waitCategoryColor(analysis::WaitCategory category, const StatePalette& palette);
// `category` is BandColumn::waitCategory (kNoWaitCategory and unknown values map to Other).
Color32 waitCategoryColorFromByte(uint8_t category, const StatePalette& palette);
// Colour of a thread state interval (Waiting uses the interval's wait reason on `platform`).
Color32 stateColor(uint32_t platform, const model::ThreadStateInterval& interval,
                   const StatePalette& palette);
// Colour of one band column: the state colours weighted by their fractions, the remainder is "unknown".
// Quantised to multiples of 16 per channel so equal-looking neighbours compare equal (run merging).
Color32 bandColumnColor(const analysis::BandColumn& column, const StatePalette& palette);

// How flame chart and strip colours are derived from a frame.
enum class FrameColorMode : uint8_t { ByModule, ByFunction };

// Stable pseudo-random colour from a 64-bit key (golden-ratio hue walk, fixed saturation/value).
Color32 hashColor(uint64_t key);
// Colour of callstack node `node` (its leaf frame). Invalid or out-of-range nodes give `unknown`.
// With `symbols`, ByFunction groups frames by their function key (resolved frames of one function share a
// colour); without, by frame address.
Color32 callstackColor(const model::TraceModel& model, CallstackIdx node, FrameColorMode mode,
                       const symbols::SymbolService* symbols, FramePosition position, Color32 unknown);
// Strip column colour: callstackColor faded towards the background by (1 - share) and by coverage (alpha).
Color32 stripColumnColor(const model::TraceModel& model, const analysis::StripColumn& column,
                         FrameColorMode mode, const symbols::SymbolService* symbols = nullptr);
// Multiplies RGB by `factor` (clamped to 0..2), keeps alpha.
Color32 scaleBrightness(Color32 color, float factor);
Color32 withAlpha(Color32 color, uint8_t alpha);
// Black or white, whichever reads better on `background`.
Color32 textColorFor(Color32 background);

}  // namespace tachyon::viewer::timeline
