#include "viewer/theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace tachyon::viewer {

namespace {

constexpr Rgba rgb(uint8_t r, uint8_t g, uint8_t b) {
  return makeRgba(r, g, b);
}

// The two palettes. Neutral greys plus one accent (blue); the state colours keep the profiler convention
// (green running, amber ready, pink-red synchronization wait, dark blue I/O, light blue memory, grey other)
// and differ in lightness as well as in hue so they stay apart under the common colour-vision deficiencies.
constexpr ThemePalette kDark{
    ColorScheme::Dark,
    /*windowBg*/ rgb(31, 33, 37),
    /*chromeBg*/ rgb(40, 42, 47),
    /*frameBg*/ rgb(47, 50, 56),
    /*border*/ rgb(66, 70, 78),
    /*text*/ rgb(228, 230, 234),
    /*textMuted*/ rgb(154, 160, 170),
    /*accent*/ rgb(76, 154, 255),
    /*accentSoft*/ rgb(44, 70, 110),
    /*hover*/ rgb(54, 58, 65),
    /*warn*/ rgb(235, 170, 60),
    /*error*/ rgb(255, 120, 110),
    /*info*/ rgb(110, 175, 255),
    StatePalette{/*running*/ rgb(70, 190, 90), /*ready*/ rgb(240, 170, 30), /*syncWait*/ rgb(222, 88, 104),
                 /*ioWait*/ rgb(60, 100, 205), /*memoryWait*/ rgb(120, 185, 240),
                 /*otherWait*/ rgb(140, 142, 150), /*unknown*/ rgb(44, 47, 53)},
    /*laneBg*/ rgb(26, 28, 31),
    /*laneBgAlt*/ rgb(33, 35, 39),
    /*rulerBg*/ rgb(40, 42, 47),
    /*pendingFill*/ rgb(44, 47, 53),
    /*pendingStripe*/ rgb(58, 62, 70),
    /*overlay*/ rgb(255, 255, 255),
    /*highlight*/ rgb(255, 190, 40),
    /*barFill*/ rgb(86, 150, 235),
};

constexpr ThemePalette kLight{
    ColorScheme::Light,
    /*windowBg*/ rgb(255, 255, 255),
    /*chromeBg*/ rgb(238, 240, 243),
    /*frameBg*/ rgb(244, 245, 247),
    /*border*/ rgb(208, 212, 218),
    /*text*/ rgb(29, 32, 36),
    /*textMuted*/ rgb(88, 96, 108),
    /*accent*/ rgb(26, 111, 209),
    /*accentSoft*/ rgb(208, 226, 250),
    /*hover*/ rgb(228, 232, 238),
    /*warn*/ rgb(150, 92, 0),
    /*error*/ rgb(190, 40, 40),
    /*info*/ rgb(20, 100, 190),
    StatePalette{/*running*/ rgb(30, 140, 62), /*ready*/ rgb(240, 165, 20), /*syncWait*/ rgb(240, 128, 142),
                 /*ioWait*/ rgb(45, 75, 175), /*memoryWait*/ rgb(110, 175, 235),
                 /*otherWait*/ rgb(150, 152, 160), /*unknown*/ rgb(232, 234, 238)},
    /*laneBg*/ rgb(246, 247, 249),
    /*laneBgAlt*/ rgb(240, 242, 245),
    /*rulerBg*/ rgb(236, 238, 242),
    /*pendingFill*/ rgb(226, 229, 234),
    /*pendingStripe*/ rgb(210, 214, 221),
    /*overlay*/ rgb(0, 0, 0),
    /*highlight*/ rgb(230, 140, 0),
    /*barFill*/ rgb(60, 130, 225),
};

ImVec4 vec(Rgba c, float alpha = -1.0f) {
  return ImVec4(rgbaR(c) / 255.0f, rgbaG(c) / 255.0f, rgbaB(c) / 255.0f,
                alpha < 0.0f ? rgbaA(c) / 255.0f : alpha);
}

Rgba mix(Rgba a, Rgba b, float t) {
  const auto ch = [&](uint8_t x, uint8_t y) {
    return static_cast<uint8_t>(std::lround(static_cast<float>(x) + (static_cast<float>(y) - x) * t));
  };
  return makeRgba(ch(rgbaR(a), rgbaR(b)), ch(rgbaG(a), rgbaG(b)), ch(rgbaB(a), rgbaB(b)));
}

double linear(uint8_t v) {
  const double c = v / 255.0;
  return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(Rgba c) {
  return 0.2126 * linear(rgbaR(c)) + 0.7152 * linear(rgbaG(c)) + 0.0722 * linear(rgbaB(c));
}

}  // namespace

const ThemePalette& paletteFor(ColorScheme scheme) {
  return scheme == ColorScheme::Light ? kLight : kDark;
}

ColorScheme resolveTheme(ThemeKind kind, ColorScheme os) {
  switch (kind) {
    case ThemeKind::Light: return ColorScheme::Light;
    case ThemeKind::Dark: return ColorScheme::Dark;
    case ThemeKind::System: break;
  }
  return os;
}

double contrastRatio(Rgba a, Rgba b) {
  const double la = luminance(a), lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

double colorDistance(Rgba a, Rgba b) {
  const double dr = static_cast<double>(rgbaR(a)) - rgbaR(b);
  const double dg = static_cast<double>(rgbaG(a)) - rgbaG(b);
  const double db = static_cast<double>(rgbaB(a)) - rgbaB(b);
  return std::sqrt(dr * dr + dg * dg + db * db);
}

const ThemePalette& currentPalette() {
  if (ImGui::GetCurrentContext() != nullptr) {
    if (const void* user = ImGui::GetIO().UserData) return *static_cast<const ThemePalette*>(user);
  }
  return kDark;
}

float effectiveUiScale(float dpiScale, float userScale) {
  return std::clamp(dpiScale * userScale, Settings::kMinUiScale, Settings::kMaxUiScale);
}

void applyTheme(ColorScheme scheme, float scale) {
  const ThemePalette& p = paletteFor(scheme);
  ImGui::GetIO().UserData = const_cast<ThemePalette*>(&p);  // read back by currentPalette(); never written
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();
  ImGui::StyleColorsDark(&style);  // fills every slot; the palette overrides the ones it names
  ImVec4* c = style.Colors;
  const bool dark = scheme == ColorScheme::Dark;
  const Rgba hoverStrong = mix(p.hover, p.text, 0.10f);

  c[ImGuiCol_Text] = vec(p.text);
  c[ImGuiCol_TextDisabled] = vec(p.textMuted);
  c[ImGuiCol_WindowBg] = vec(p.windowBg, 1.0f);
  c[ImGuiCol_ChildBg] = vec(p.windowBg, 0.0f);
  c[ImGuiCol_PopupBg] = vec(p.windowBg, 1.0f);
  c[ImGuiCol_Border] = vec(p.border);
  c[ImGuiCol_BorderShadow] = vec(p.border, 0.0f);
  c[ImGuiCol_FrameBg] = vec(p.frameBg);
  c[ImGuiCol_FrameBgHovered] = vec(p.hover);
  c[ImGuiCol_FrameBgActive] = vec(hoverStrong);
  c[ImGuiCol_TitleBg] = vec(p.chromeBg);
  c[ImGuiCol_TitleBgActive] = vec(p.chromeBg);
  c[ImGuiCol_TitleBgCollapsed] = vec(p.chromeBg);
  c[ImGuiCol_MenuBarBg] = vec(p.chromeBg);
  c[ImGuiCol_ScrollbarBg] = vec(p.windowBg, 0.0f);
  c[ImGuiCol_ScrollbarGrab] = vec(mix(p.border, p.textMuted, 0.35f));
  c[ImGuiCol_ScrollbarGrabHovered] = vec(mix(p.border, p.textMuted, 0.6f));
  c[ImGuiCol_ScrollbarGrabActive] = vec(p.textMuted);
  c[ImGuiCol_CheckMark] = vec(p.accent);
  c[ImGuiCol_SliderGrab] = vec(p.accent);
  c[ImGuiCol_SliderGrabActive] = vec(mix(p.accent, p.text, 0.2f));
  c[ImGuiCol_Button] = vec(p.frameBg);
  c[ImGuiCol_ButtonHovered] = vec(p.hover);
  c[ImGuiCol_ButtonActive] = vec(hoverStrong);
  c[ImGuiCol_Header] = vec(p.accentSoft);
  c[ImGuiCol_HeaderHovered] = vec(p.hover);
  c[ImGuiCol_HeaderActive] = vec(mix(p.accentSoft, p.accent, 0.25f));
  c[ImGuiCol_Separator] = vec(p.border);
  c[ImGuiCol_SeparatorHovered] = vec(p.accent);
  c[ImGuiCol_SeparatorActive] = vec(p.accent);
  c[ImGuiCol_ResizeGrip] = vec(p.border, 0.5f);
  c[ImGuiCol_ResizeGripHovered] = vec(p.accent, 0.7f);
  c[ImGuiCol_ResizeGripActive] = vec(p.accent);
  c[ImGuiCol_Tab] = vec(p.chromeBg);
  c[ImGuiCol_TabHovered] = vec(p.hover);
  c[ImGuiCol_TabSelected] = vec(p.windowBg);
  c[ImGuiCol_TabSelectedOverline] = vec(p.accent);
  c[ImGuiCol_TabDimmed] = vec(p.chromeBg);
  c[ImGuiCol_TabDimmedSelected] = vec(p.windowBg);
  c[ImGuiCol_TabDimmedSelectedOverline] = vec(p.border);
  c[ImGuiCol_DockingPreview] = vec(p.accent, 0.35f);
  c[ImGuiCol_DockingEmptyBg] = vec(p.laneBg);
  c[ImGuiCol_PlotLines] = vec(p.textMuted);
  c[ImGuiCol_PlotLinesHovered] = vec(p.highlight);
  c[ImGuiCol_PlotHistogram] = vec(p.accent);
  c[ImGuiCol_PlotHistogramHovered] = vec(p.highlight);
  c[ImGuiCol_TableHeaderBg] = vec(p.chromeBg);
  c[ImGuiCol_TableBorderStrong] = vec(p.border);
  c[ImGuiCol_TableBorderLight] = vec(mix(p.border, p.windowBg, 0.5f));
  c[ImGuiCol_TableRowBg] = vec(p.windowBg, 0.0f);
  c[ImGuiCol_TableRowBgAlt] = vec(p.overlay, dark ? 0.03f : 0.025f);
  c[ImGuiCol_TextLink] = vec(p.accent);
  c[ImGuiCol_TextSelectedBg] = vec(p.accent, 0.35f);
  c[ImGuiCol_DragDropTarget] = vec(p.highlight);
  c[ImGuiCol_NavCursor] = vec(p.accent);
  c[ImGuiCol_NavWindowingHighlight] = vec(p.overlay, 0.7f);
  c[ImGuiCol_NavWindowingDimBg] = vec(p.overlay, 0.2f);
  c[ImGuiCol_ModalWindowDimBg] = vec(dark ? makeRgba(0, 0, 0) : makeRgba(40, 44, 52), 0.35f);

  // Compact, flat look: thin borders, square-ish corners, tight title / tab strips.
  style.WindowRounding = 0.0f;
  style.ChildRounding = 0.0f;
  style.PopupRounding = 3.0f;
  style.FrameRounding = 2.0f;
  style.TabRounding = 2.0f;
  style.ScrollbarRounding = 2.0f;
  style.GrabRounding = 2.0f;
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.TabBarBorderSize = 1.0f;
  style.TabBarOverlineSize = 2.0f;
  style.DockingSeparatorSize = 2.0f;
  style.WindowPadding = ImVec2(6.0f, 4.0f);
  style.FramePadding = ImVec2(6.0f, 3.0f);
  style.CellPadding = ImVec2(5.0f, 2.0f);
  style.ItemSpacing = ImVec2(8.0f, 4.0f);
  style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
  style.ScrollbarSize = 12.0f;
  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.ScaleAllSizes(scale);
  ImGui::GetIO().FontGlobalScale = scale;
}

}  // namespace tachyon::viewer
