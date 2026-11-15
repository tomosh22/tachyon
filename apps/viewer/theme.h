// Theme and DPI handling for the ImGui style (WP-67e: Light / Dark palettes, System follows the OS).
//
// All colours of the viewer live in the two ThemePalette tables of theme.cpp: the ImGui style colours, the
// timeline / thread state colours and the data-view backgrounds. Colours are packed 0xAABBGGRR (ImGui's
// ImU32 layout) so the pure functions that use them stay free of ImGui.
#pragma once

#include "viewer/settings.h"

#include <cstdint>

namespace tachyon::viewer {

using Rgba = uint32_t;  // 0xAABBGGRR

constexpr Rgba makeRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(g) << 8) | r;
}
constexpr uint8_t rgbaR(Rgba c) {
  return static_cast<uint8_t>(c & 0xFF);
}
constexpr uint8_t rgbaG(Rgba c) {
  return static_cast<uint8_t>((c >> 8) & 0xFF);
}
constexpr uint8_t rgbaB(Rgba c) {
  return static_cast<uint8_t>((c >> 16) & 0xFF);
}
constexpr uint8_t rgbaA(Rgba c) {
  return static_cast<uint8_t>(c >> 24);
}

// A resolved look: what the UI is drawn with. ThemeKind::System resolves to one of these.
enum class ColorScheme : uint8_t { Light, Dark };

// Colours of the thread states (timeline bands, Call Graph thread-state bars, Source gutter bars).
struct StatePalette {
  Rgba running, ready, syncWait, ioWait, memoryWait, otherWait;
  Rgba unknown;  // background-ish: "no data"
};

struct ThemePalette {
  ColorScheme scheme;
  // Neutral UI.
  Rgba windowBg;    // panels
  Rgba chromeBg;    // menu bar, tab strips, title strips, table headers
  Rgba frameBg;     // inputs, plot backgrounds
  Rgba border;      // thin separators
  Rgba text;        // body text; >= 4.5:1 on windowBg, chromeBg and frameBg
  Rgba textMuted;   // secondary text (thread ids, hints); >= 4.5:1 on windowBg
  Rgba accent;      // the one accent colour (selection, active tab, links, histogram bars)
  Rgba accentSoft;  // selected row / hover background tinted with the accent; `text` reads on it
  Rgba hover;       // neutral hover background
  // Text colours that read on windowBg.
  Rgba warn, error, info;
  // Data views.
  StatePalette states;
  Rgba laneBg;       // timeline plot background of an empty row / graph
  Rgba laneBgAlt;    // alternate row background
  Rgba rulerBg;      // time ruler
  Rgba pendingFill;  // "not computed yet" fill of a band
  Rgba pendingStripe;
  Rgba overlay;    // contrast overlays and outlines on data (white in dark mode, black in light mode)
  Rgba highlight;  // instance / search highlight (amber)
  Rgba barFill;    // histogram and bar chart bars
};

// Thread-safe: pure. The palette of a resolved scheme (static tables, valid for the process lifetime).
const ThemePalette& paletteFor(ColorScheme scheme);

// Thread-safe: pure. `os` is what the operating system prefers; used only when `kind` is System.
ColorScheme resolveTheme(ThemeKind kind, ColorScheme os);

// Thread-safe: pure. `color` with alpha replaced.
constexpr Rgba rgbaWithAlpha(Rgba color, uint8_t alpha) {
  return (color & 0x00FFFFFFu) | (static_cast<uint32_t>(alpha) << 24);
}

// Thread-safe: pure. WCAG 2.x contrast ratio (1..21) of two opaque colours (alpha ignored).
double contrastRatio(Rgba a, Rgba b);
// Thread-safe: pure. Euclidean distance of two colours in 0..255 RGB space.
double colorDistance(Rgba a, Rgba b);

// Main-thread only. Resets the current ImGui style, applies the palette's colours and scales sizes and fonts
// by `scale` (display DPI scale times the user's UI scale). Requires a live ImGui context.
void applyTheme(ColorScheme scheme, float scale);

// Main-thread only. The palette applyTheme() last applied to the current ImGui context (kept in
// ImGuiIO::UserData, so it is per context, not global); the dark palette without a context or before the
// first applyTheme(). Lets code that has no PanelContext (status bar, dialogs) pick semantic text colours.
const ThemePalette& currentPalette();

// Thread-safe: pure function. Combined scale, clamped to the supported range.
float effectiveUiScale(float dpiScale, float userScale);

}  // namespace tachyon::viewer
