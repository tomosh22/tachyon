// WP-46: geometry of the wake-up arrow (on-screen, clipped, hidden waker).
#include "viewer/interaction/arrow_geometry.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace tachyon::viewer::interaction;

namespace {

const Rect kClip{100, 50, 500, 350};

ArrowInput input(Vec2 waker, Vec2 woken, bool known = true) {
  ArrowInput in;
  in.waker = waker;
  in.woken = woken;
  in.wakerRowKnown = known;
  in.clip = kClip;
  return in;
}

}  // namespace

TEST(ArrowGeometry, FullyOnScreenKeepsBothEndPoints) {
  const ArrowGeometry g = computeArrow(input({150, 100}, {300, 200}));
  ASSERT_TRUE(g.visible);
  EXPECT_EQ(g.from, (Vec2{150, 100}));
  EXPECT_EQ(g.to, (Vec2{300, 200}));
  EXPECT_FALSE(g.fromClipped);
  EXPECT_FALSE(g.toClipped);
  EXPECT_FALSE(g.wakerHidden);
  EXPECT_EQ(g.markerSide, EdgeSide::None);
}

TEST(ArrowGeometry, WakerRowAboveTheViewIsClippedWithATopMarker) {
  // The waker's row is scrolled off the top: y = -50 (above the clip rect at y0 = 50).
  const ArrowGeometry g = computeArrow(input({200, -50}, {300, 150}));
  ASSERT_TRUE(g.visible);
  EXPECT_TRUE(g.fromClipped);
  EXPECT_FALSE(g.toClipped);
  EXPECT_FLOAT_EQ(g.from.y, kClip.y0);
  EXPECT_NEAR(g.from.x, 250.0f, 1e-3);  // on the line at y = 50: halfway from (200,-50) to (300,150)
  EXPECT_EQ(g.to, (Vec2{300, 150}));
  EXPECT_EQ(g.markerSide, EdgeSide::Top);
  EXPECT_EQ(g.markerPos, g.from);
}

TEST(ArrowGeometry, WakerRowBelowTheViewGetsABottomMarker) {
  const ArrowGeometry g = computeArrow(input({200, 600}, {300, 100}));
  ASSERT_TRUE(g.visible);
  EXPECT_TRUE(g.fromClipped);
  EXPECT_FLOAT_EQ(g.from.y, kClip.y1);
  EXPECT_EQ(g.markerSide, EdgeSide::Bottom);
}

TEST(ArrowGeometry, WakeUpLeftOfThePlotGetsALeftMarker) {
  const ArrowGeometry g = computeArrow(input({20, 150}, {300, 150}));
  ASSERT_TRUE(g.visible);
  EXPECT_TRUE(g.fromClipped);
  EXPECT_FLOAT_EQ(g.from.x, kClip.x0);
  EXPECT_FLOAT_EQ(g.from.y, 150.0f);
  EXPECT_EQ(g.markerSide, EdgeSide::Left);
}

TEST(ArrowGeometry, SwitchInRightOfThePlotLosesTheHeadButKeepsTheWakerEnd) {
  const ArrowGeometry g = computeArrow(input({200, 100}, {900, 200}));
  ASSERT_TRUE(g.visible);
  EXPECT_FALSE(g.fromClipped);
  EXPECT_TRUE(g.toClipped);
  EXPECT_FLOAT_EQ(g.to.x, kClip.x1);
  EXPECT_EQ(g.markerSide, EdgeSide::None);  // markers are for the waker end
}

TEST(ArrowGeometry, HiddenWakerStartsAtTheTopEdgeAboveTheWakeUp) {
  const ArrowGeometry g = computeArrow(input({250, 9999}, {400, 200}, /*known=*/false));
  ASSERT_TRUE(g.visible);
  EXPECT_TRUE(g.wakerHidden);
  EXPECT_TRUE(g.fromClipped);
  EXPECT_EQ(g.from, (Vec2{250, kClip.y0}));
  EXPECT_EQ(g.markerSide, EdgeSide::Top);
  EXPECT_EQ(g.markerPos, g.from);
  EXPECT_EQ(g.to, (Vec2{400, 200}));
}

TEST(ArrowGeometry, HiddenWakerWithTheWakeUpOffToTheLeftStillClipsIntoView) {
  const ArrowGeometry g = computeArrow(input({0, 0}, {400, 200}, false));
  ASSERT_TRUE(g.visible);
  EXPECT_TRUE(g.wakerHidden);
  EXPECT_GE(g.from.x, kClip.x0);
  EXPECT_GE(g.from.y, kClip.y0);
  EXPECT_EQ(g.markerSide, EdgeSide::Top);
}

TEST(ArrowGeometry, SegmentCompletelyOutsideIsNotVisible) {
  const ArrowGeometry g = computeArrow(input({10, 10}, {60, 400}));  // left of the clip rect entirely
  EXPECT_FALSE(g.visible);
  EXPECT_EQ(g.markerSide, EdgeSide::None);
  // A hidden waker whose wake-up is inside the plot still leaves a marker even if the woken end is gone.
  const ArrowGeometry h = computeArrow(input({300, 0}, {900, 1000}, false));
  EXPECT_TRUE(h.wakerHidden);
  EXPECT_EQ(h.markerSide, EdgeSide::Top);
}

TEST(ArrowGeometry, ZeroLengthAndVerticalSegmentsAreHandled) {
  const ArrowGeometry z = computeArrow(input({200, 100}, {200, 100}));
  EXPECT_TRUE(z.visible);
  EXPECT_EQ(z.from, z.to);
  const ArrowGeometry v = computeArrow(input({200, 0}, {200, 300}));
  ASSERT_TRUE(v.visible);
  EXPECT_FLOAT_EQ(v.from.y, kClip.y0);
  EXPECT_EQ(v.markerSide, EdgeSide::Top);
  const ArrowGeometry outside =
      computeArrow(input({50, 0}, {50, 300}));  // parallel to the left edge, outside
  EXPECT_FALSE(outside.visible);
}

TEST(ArrowGeometry, ArrowHeadPointsAlongTheSegment) {
  const ArrowHead h = arrowHead({0, 0}, {10, 0}, 4.0f);
  EXPECT_EQ(h.tip, (Vec2{10, 0}));
  EXPECT_FLOAT_EQ(h.left.x, 6.0f);
  EXPECT_FLOAT_EQ(h.right.x, 6.0f);
  EXPECT_FLOAT_EQ(std::fabs(h.left.y), 2.0f);
  EXPECT_FLOAT_EQ(h.left.y, -h.right.y);
  const ArrowHead degenerate = arrowHead({5, 5}, {5, 5}, 4.0f);  // no direction: must not produce NaN
  EXPECT_FALSE(std::isnan(degenerate.left.x));
}
