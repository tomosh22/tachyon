// RectBatch: appends many filled rectangles to an ImGui draw list with one PrimReserve per chunk instead of
// one per rectangle (extracted from the timeline widget by WP-54 so the instrumentation lanes share it).
#pragma once

#include <imgui.h>

#include <algorithm>
#include <cstddef>

namespace tachyon::viewer::common {

// Main-thread only. Nothing else may be added to the draw list while a batch is alive.
class RectBatch {
 public:
  RectBatch(ImDrawList* dl, size_t upperBound) : m_dl(dl), m_remaining(upperBound) { refill(); }
  ~RectBatch() { release(); }
  RectBatch(const RectBatch&) = delete;
  RectBatch& operator=(const RectBatch&) = delete;

  void add(float x0, float y0, float x1, float y1, ImU32 color) {
    if (m_used == m_reserved) {
      release();
      refill();
    }
    m_dl->PrimRect(ImVec2(x0, y0), ImVec2(x1, y1), color);
    ++m_used;
  }

 private:
  static constexpr size_t kChunk = 4096;  // 16384 vertices: far below the 16-bit index limit
  void refill() {
    m_reserved = std::min(m_remaining, kChunk);
    m_used = 0;
    if (m_reserved > 0) m_dl->PrimReserve(static_cast<int>(m_reserved * 6), static_cast<int>(m_reserved * 4));
  }
  void release() {
    if (m_reserved > m_used) {
      m_dl->PrimUnreserve(static_cast<int>((m_reserved - m_used) * 6),
                          static_cast<int>((m_reserved - m_used) * 4));
    }
    m_remaining -= std::min(m_remaining, m_used);
    m_reserved = m_used = 0;
  }

  ImDrawList* m_dl;
  size_t m_remaining;
  size_t m_reserved = 0, m_used = 0;
};

}  // namespace tachyon::viewer::common
