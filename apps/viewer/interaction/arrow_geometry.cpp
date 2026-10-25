#include "viewer/interaction/arrow_geometry.h"

#include <algorithm>
#include <cmath>

namespace tachyon::viewer::interaction {

namespace {

// Liang-Barsky. On success t0/t1 are the parameters of the clipped segment on a->b and `entry`/`exit` the
// edges crossed at t0 and t1 (None when the end point is inside).
struct Clipped {
  bool ok = false;
  float t0 = 0, t1 = 1;
  EdgeSide entry = EdgeSide::None, exit = EdgeSide::None;
};

Clipped clipSegment(const Rect& r, Vec2 a, Vec2 b) {
  Clipped c;
  const float dx = b.x - a.x, dy = b.y - a.y;
  struct Edge {
    float p, q;
    EdgeSide side;
  };
  const Edge edges[4] = {{-dx, a.x - r.x0, EdgeSide::Left},
                         {dx, r.x1 - a.x, EdgeSide::Right},
                         {-dy, a.y - r.y0, EdgeSide::Top},
                         {dy, r.y1 - a.y, EdgeSide::Bottom}};
  for (const Edge& e : edges) {
    if (e.p == 0.0f) {
      if (e.q < 0.0f) return c;  // parallel to this edge and outside
      continue;
    }
    const float t = e.q / e.p;
    if (e.p < 0.0f) {  // entering
      if (t > c.t1) return c;
      if (t > c.t0) {
        c.t0 = t;
        c.entry = e.side;
      }
    } else {  // leaving
      if (t < c.t0) return c;
      if (t < c.t1) {
        c.t1 = t;
        c.exit = e.side;
      }
    }
  }
  c.ok = true;
  return c;
}

Vec2 lerp(Vec2 a, Vec2 b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

}  // namespace

ArrowGeometry computeArrow(const ArrowInput& in) {
  ArrowGeometry g;
  Vec2 start = in.waker;
  if (!in.wakerRowKnown) {
    g.wakerHidden = true;
    start = {in.waker.x, in.clip.y0};
  }
  const Clipped c = clipSegment(in.clip, start, in.woken);
  if (!c.ok) {
    // Nothing of the segment is on screen. A hidden waker still gets its marker when the ReadyRec position
    // is inside the plot horizontally.
    if (g.wakerHidden && in.waker.x >= in.clip.x0 && in.waker.x <= in.clip.x1) {
      g.markerSide = EdgeSide::Top;
      g.markerPos = {in.waker.x, in.clip.y0};
    }
    return g;
  }
  g.visible = true;
  g.from = lerp(start, in.woken, c.t0);
  g.to = lerp(start, in.woken, c.t1);
  g.fromClipped = c.t0 > 0.0f || g.wakerHidden;
  g.toClipped = c.t1 < 1.0f;
  if (g.wakerHidden) {
    g.markerSide = EdgeSide::Top;
    g.markerPos = g.from;
  } else if (c.t0 > 0.0f) {
    g.markerSide = c.entry;
    g.markerPos = g.from;
  }
  return g;
}

ArrowHead arrowHead(Vec2 from, Vec2 to, float size) {
  ArrowHead h;
  h.tip = to;
  float dx = to.x - from.x, dy = to.y - from.y;
  const float len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-6f) {
    dx = 0.0f;
    dy = 1.0f;
  } else {
    dx /= len;
    dy /= len;
  }
  // Back along the direction, then +-0.5 size perpendicular.
  const Vec2 base{to.x - dx * size, to.y - dy * size};
  h.left = {base.x - dy * size * 0.5f, base.y + dx * size * 0.5f};
  h.right = {base.x + dy * size * 0.5f, base.y - dx * size * 0.5f};
  return h;
}

}  // namespace tachyon::viewer::interaction
