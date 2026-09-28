// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Stroke-width invariant for the overlay tessellator.
//
// The bug these tests pin: annotation stroke width is baked in image-pixel space
// and then scaled by the view transform, so at display scales below 1:1 (zoomed
// out / large image in a small dock) strokes went sub-pixel and edges dropped out
// of rasterization depending on sub-pixel alignment — "some lines of a
// rectangle/cube disappear depending on zoom".
//
// Behaviour (Option B): stroke width SCALES with zoom (image-space thickness, so
// outlines grow as you zoom in), but is FLOORED so it is never thinner than 1px
// on screen — that floor is what stops edges from vanishing when zoomed out.

#include "pj_scene2d_core/overlay_geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace PJ::overlay_geometry {
namespace {

constexpr int kStride = kFloatsPerVertex;

// Perpendicular extent of the emitted geometry along an axis (1 = y, 0 = x).
double extent(const std::vector<float>& v, int axis) {
  double lo = 1e18;
  double hi = -1e18;
  for (size_t i = 0; i + kStride <= v.size(); i += kStride) {
    const double val = v[i + axis];
    lo = std::min(lo, val);
    hi = std::max(hi, val);
  }
  return hi - lo;
}

PointsAnnotation horizontalSegment(double thickness) {
  PointsAnnotation pa;
  pa.topology = AnnotationTopology::kLineList;
  pa.points = {{0.0, 50.0}, {100.0, 50.0}};
  pa.thickness = thickness;
  pa.color = {255, 0, 0, 255};
  return pa;
}

// effective_scale = on-screen pixels per image pixel; the renderer's view
// transform multiplies geometry by this, so on-screen width = image extent ×
// effective_scale. image_px_per_screen_px is its reciprocal.
double onScreenWidth(const std::vector<float>& geom, int axis, double effective_scale) {
  return extent(geom, axis) * effective_scale;
}

double strokeOnScreenWidth(double thickness, double effective_scale) {
  std::vector<float> geom;
  appendLineStrokes(horizontalSegment(thickness), 1.0 / effective_scale, geom);
  return onScreenWidth(geom, /*y*/ 1, effective_scale);
}

// --- Width scales with zoom (above the floor) --------------------------------

TEST(OverlayGeometryStroke, WidthEqualsThicknessAtUnityScale) {
  EXPECT_NEAR(strokeOnScreenWidth(3.0, 1.0), 3.0, 1e-4);
}

TEST(OverlayGeometryStroke, WidthGrowsWhenZoomedIn) {
  // A 3px stroke at 2× is 6px on screen; at 4× it is 12px — it scales with zoom.
  EXPECT_NEAR(strokeOnScreenWidth(3.0, 2.0), 6.0, 1e-4);
  EXPECT_NEAR(strokeOnScreenWidth(3.0, 4.0), 12.0, 1e-4);
}

// --- Floor: never thinner than 1px on screen (the anti-vanish guarantee) ------

TEST(OverlayGeometryStroke, FlooredToOnePixelWhenZoomedOut) {
  // 2px stroke at 0.4× would be 0.8px on screen → floored to 1px (NOT 0.8 or 2).
  EXPECT_NEAR(strokeOnScreenWidth(2.0, 0.4), 1.0, 1e-4);
  // Hairline 0.5px at 0.4× → 0.2px → floored to 1px.
  EXPECT_NEAR(strokeOnScreenWidth(0.5, 0.4), 1.0, 1e-4);
}

TEST(OverlayGeometryStroke, NeverBelowOnePixelAcrossScales) {
  for (double scale : {0.05, 0.1, 0.25, 0.5, 1.0, 3.0, 10.0}) {
    for (double thickness : {0.5, 1.0, 2.0, 5.0}) {
      const double w = strokeOnScreenWidth(thickness, scale);
      EXPECT_GE(w, 1.0 - 1e-4) << "scale=" << scale << " thickness=" << thickness;
      EXPECT_NEAR(w, std::max(thickness * scale, 1.0), 1e-4) << "scale=" << scale << " thickness=" << thickness;
    }
  }
}

// --- Topology is preserved (rectangle keeps all four edges) ------------------

TEST(OverlayGeometryStroke, LineLoopEmitsAllEdges) {
  PointsAnnotation rect;
  rect.topology = AnnotationTopology::kLineLoop;
  rect.points = {{10, 10}, {110, 10}, {110, 60}, {10, 60}};  // 4 corners
  rect.thickness = 2.0;
  std::vector<float> geom;
  appendLineStrokes(rect, 1.0 / 0.5, geom);
  // 4 edges × 2 triangles × 3 verts × 6 floats = 144.
  EXPECT_EQ(geom.size(), static_cast<size_t>(4 * 6 * kStride));
}

// --- Point markers follow the same scale-with-floor rule ---------------------

TEST(OverlayGeometryStroke, PointQuadScalesWithFloor) {
  PointsAnnotation pts;
  pts.topology = AnnotationTopology::kPoints;
  pts.points = {{50.0, 50.0}};
  pts.thickness = 4.0;
  {  // 4px at 2× → 8px square
    std::vector<float> geom;
    appendPointQuads(pts, 1.0 / 2.0, geom);
    ASSERT_FALSE(geom.empty());
    EXPECT_NEAR(onScreenWidth(geom, 0, 2.0), 8.0, 1e-4);
    EXPECT_NEAR(onScreenWidth(geom, 1, 2.0), 8.0, 1e-4);
  }
  {  // 4px at 0.1× would be 0.4px → floored to 1px
    std::vector<float> geom;
    appendPointQuads(pts, 1.0 / 0.1, geom);
    EXPECT_NEAR(onScreenWidth(geom, 0, 0.1), 1.0, 1e-4);
  }
}

// --- Fills are image-space (scale with zoom), NOT floored ---------------------

TEST(OverlayGeometryStroke, LoopFillIsImageSpace) {
  PointsAnnotation rect;
  rect.topology = AnnotationTopology::kLineLoop;
  rect.points = {{0, 0}, {100, 0}, {100, 80}, {0, 80}};
  rect.fill_color = {0, 0, 255, 128};
  std::vector<float> geom;
  appendLoopFill(rect, geom);
  ASSERT_FALSE(geom.empty());
  // Fill covers the full polygon in image space regardless of any zoom.
  EXPECT_NEAR(extent(geom, 0), 100.0, 1e-4);
  EXPECT_NEAR(extent(geom, 1), 80.0, 1e-4);
}

}  // namespace
}  // namespace PJ::overlay_geometry

// The viewer shows image pixels (y down) turned clockwise by `rotation_deg`, up to
// a uniform scale. Upright text = its quad, seen through that turn, is an
// unrotated w x h box whose top-left is the anchor.
TEST(OverlayGeometryText, QuadCornersReadUprightOnScreenAtEveryRotation) {
  const PJ::Point2 anchor{40.0, 25.0};
  constexpr double kWidth = 30.0;
  constexpr double kHeight = 12.0;
  for (const int rotation : {0, 90, -90, 180}) {
    const double radians = rotation * std::numbers::pi / 180.0;
    const auto to_screen = [&](const PJ::Point2& p) {
      const double dx = p.x - anchor.x;
      const double dy = p.y - anchor.y;
      return PJ::Point2{
          dx * std::cos(radians) - dy * std::sin(radians), dx * std::sin(radians) + dy * std::cos(radians)};
    };
    const auto corners = PJ::overlay_geometry::uprightTextQuadCorners(anchor, kWidth, kHeight, rotation);
    const PJ::Point2 expected[4] = {{0.0, 0.0}, {kWidth, 0.0}, {kWidth, kHeight}, {0.0, kHeight}};
    for (size_t i = 0; i < 4; ++i) {
      const PJ::Point2 on_screen = to_screen(corners[i]);
      EXPECT_NEAR(on_screen.x, expected[i].x, 1e-9) << "rotation " << rotation << " corner " << i;
      EXPECT_NEAR(on_screen.y, expected[i].y, 1e-9) << "rotation " << rotation << " corner " << i;
    }
  }
}
