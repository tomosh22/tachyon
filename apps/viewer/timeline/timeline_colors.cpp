#include "viewer/timeline/timeline_colors.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace tachyon::viewer::timeline {

namespace {

uint8_t clamp8(float v) {
  return static_cast<uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

uint64_t mix64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return x;
}

Color32 hsv(float h, float s, float v) {
  h = h - std::floor(h);
  const float c = v * s;
  const float x = c * (1.0f - std::fabs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
  const float m = v - c;
  float r = 0, g = 0, b = 0;
  switch (static_cast<int>(h * 6.0f)) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
  }
  return makeColor(clamp8((r + m) * 255.0f), clamp8((g + m) * 255.0f), clamp8((b + m) * 255.0f));
}

}  // namespace

Color32 waitCategoryColor(analysis::WaitCategory category) {
  switch (category) {
    case analysis::WaitCategory::Synchronization: return kSyncWaitColor;
    case analysis::WaitCategory::IO: return kIoWaitColor;
    case analysis::WaitCategory::Memory: return kMemoryWaitColor;
    case analysis::WaitCategory::Preempted: return kReadyColor;
    case analysis::WaitCategory::Other: break;
  }
  return kOtherWaitColor;
}

Color32 waitCategoryColorFromByte(uint8_t category) {
  if (category > static_cast<uint8_t>(analysis::WaitCategory::Other)) return kOtherWaitColor;
  return waitCategoryColor(static_cast<analysis::WaitCategory>(category));
}

Color32 stateColor(uint32_t platform, const model::ThreadStateInterval& interval) {
  switch (interval.state) {
    case model::ThreadState::Running: return kRunningColor;
    case model::ThreadState::Ready: return kReadyColor;
    case model::ThreadState::Waiting:
      return waitCategoryColor(analysis::waitReasonCategory(platform, interval.wait_reason));
    case model::ThreadState::Unknown: break;
  }
  return kUnknownColor;
}

Color32 bandColumnColor(const analysis::BandColumn& column) {
  const float total = column.running + column.ready + column.waiting + column.unknown;
  if (!(total > 0.0f)) return kUnknownColor;
  // Integer weights out of 256 (this runs for every pixel column of every row of every frame).
  const float scale = 256.0f / total;
  const int wr = std::clamp(static_cast<int>(column.running * scale + 0.5f), 0, 256);
  const int wy = std::clamp(static_cast<int>(column.ready * scale + 0.5f), 0, 256 - wr);
  const int ww = std::clamp(static_cast<int>(column.waiting * scale + 0.5f), 0, 256 - wr - wy);
  const int wu = 256 - wr - wy - ww;
  const Color32 waitColor = ww > 0 ? waitCategoryColorFromByte(column.waitCategory) : kOtherWaitColor;
  const auto channel = [&](uint8_t (*get)(Color32)) {
    const int v = (wr * get(kRunningColor) + wy * get(kReadyColor) + ww * get(waitColor) +
                   wu * get(kUnknownColor) + 128) >>
                  8;
    // Quantise to multiples of 16 so near-equal neighbours merge into one rectangle.
    return static_cast<uint8_t>(std::min(255, ((v + 8) >> 4) << 4));
  };
  return makeColor(channel(colorR), channel(colorG), channel(colorB));
}

Color32 hashColor(uint64_t key) {
  // A palette of well spread colours built once; the hash only picks an entry.
  constexpr size_t kPaletteSize = 1024;
  static const std::array<Color32, kPaletteSize> kPalette = [] {
    std::array<Color32, kPaletteSize> table{};
    for (size_t i = 0; i < kPaletteSize; ++i) {
      const uint64_t h = mix64(i + 0x9E3779B97F4A7C15ULL);
      const float hue = static_cast<float>(i) * 0.61803398875f;  // golden-ratio hue walk
      const float sat = 0.45f + 0.25f * static_cast<float>(h & 0xFF) / 255.0f;
      const float val = 0.65f + 0.25f * static_cast<float>((h >> 8) & 0xFF) / 255.0f;
      table[i] = hsv(hue, sat, val);
    }
    return table;
  }();
  return kPalette[mix64(key + 0x9E3779B97F4A7C15ULL) & (kPaletteSize - 1)];
}

Color32 callstackColor(const model::TraceModel& model, CallstackIdx node, FrameColorMode mode) {
  if (!node.valid() || node.get() >= model.nodes.size()) return kUnknownColor;
  const FrameIdx frameIdx = model.nodes[node.get()].frame;
  if (!frameIdx.valid() || frameIdx.get() >= model.frames.size()) return kUnknownColor;
  const model::Frame& f = model.frames[frameIdx.get()];
  uint64_t key = f.module.get();
  if (mode == FrameColorMode::ByFunction) key = (key << 40) ^ f.addr_or_rva;
  return hashColor(key);
}

Color32 stripColumnColor(const model::TraceModel& model, const analysis::StripColumn& column,
                         FrameColorMode mode) {
  if (!column.stack.valid() || !(column.coverage > 0.0f)) return 0;
  const Color32 base = callstackColor(model, column.stack, mode);
  // Mixed columns (low share of the dominant stack) fade towards grey so they read as "many stacks".
  const int shareQ = static_cast<int>(std::clamp(column.share, 0.0f, 1.0f) * 255.0f);
  const int coverageQ = static_cast<int>(std::clamp(column.coverage, 0.0f, 1.0f) * 255.0f);
  const int k = 115 + ((141 * shareQ) >> 8);  // 0.45 .. 1.0 in 1/256
  const int grey = (90 * (256 - k)) >> 8;
  const auto fade = [&](uint8_t v) { return static_cast<uint8_t>((((v * k) >> 8) + grey) & 0xF0); };
  const int alpha = 89 + ((166 * coverageQ) >> 8);  // 0.35 .. 1.0
  // Quantised so adjacent columns of the same stack merge into one rect.
  return makeColor(fade(colorR(base)), fade(colorG(base)), fade(colorB(base)),
                   static_cast<uint8_t>(alpha & 0xE0));
}

Color32 scaleBrightness(Color32 color, float factor) {
  factor = std::clamp(factor, 0.0f, 2.0f);
  return makeColor(clamp8(colorR(color) * factor), clamp8(colorG(color) * factor),
                   clamp8(colorB(color) * factor), colorA(color));
}

Color32 withAlpha(Color32 color, uint8_t alpha) {
  return (color & 0x00FFFFFFu) | (static_cast<uint32_t>(alpha) << 24);
}

Color32 textColorFor(Color32 background) {
  const float luma = 0.299f * colorR(background) + 0.587f * colorG(background) + 0.114f * colorB(background);
  return luma > 140.0f ? makeColor(10, 10, 12) : makeColor(245, 245, 245);
}

}  // namespace tachyon::viewer::timeline
