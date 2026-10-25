// Screen geometry of the wake-up arrow (WP-46): from the waker thread's row at the ReadyRec position to the
// woken thread's row at its switch-in, clipped to the visible rows area, with an edge marker when the
// waker's end is not on screen. Pure (no ImGui), unit-tested.
#pragma once

#include <cstdint>

namespace tachyon::viewer::interaction {

struct Vec2 {
  float x = 0, y = 0;
  bool operator==(const Vec2&) const = default;
};
struct Rect {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // x1 > x0, y1 > y0
};

enum class EdgeSide : uint8_t { None, Left, Right, Top, Bottom };

struct ArrowInput {
  Vec2 waker;                 // where the arrow starts (x = ReadyRec position; y = centre of the waker's row)
  Vec2 woken;                 // where it ends (x = switch-in position; y = centre of the woken thread's row)
  bool wakerRowKnown = true;  // false: the waker has no row (hidden thread, or not a thread of the trace)
  Rect clip;                  // the visible plot / rows area
};

struct ArrowGeometry {
  bool visible = false;      // something of the arrow is inside `clip`
  Vec2 from, to;             // clipped end points (valid when visible)
  bool fromClipped = false;  // the waker end was cut off (or the waker row is hidden)
  bool toClipped = false;    // the woken end was cut off: no arrow head
  bool wakerHidden = false;  // no row for the waker
  // Marker at the clipped waker end, labelled with the waker's name by the caller.
  EdgeSide markerSide = EdgeSide::None;
  Vec2 markerPos;
};

// Thread-safe: pure function. A waker row that is hidden starts the arrow at the top edge of `clip` above the
// ReadyRec position (marker on the Top edge). Otherwise the segment waker -> woken is clipped to `clip`
// (Liang-Barsky); when the waker end moved, the marker sits there, on the edge the segment crossed.
ArrowGeometry computeArrow(const ArrowInput& input);

// Thread-safe: pure function. The three points of an arrow head whose tip is `to`, pointing along from -> to.
struct ArrowHead {
  Vec2 tip, left, right;
};
ArrowHead arrowHead(Vec2 from, Vec2 to, float size);

}  // namespace tachyon::viewer::interaction
