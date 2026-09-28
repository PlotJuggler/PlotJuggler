#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <vector>

#include "pj_scene2d_core/scene_frame.h"

// Backend-agnostic tessellation of scene-overlay annotations (lines, polygons,
// circles, points) into interleaved vertex data for the GPU overlay pipelines.
//
// Vertex layout: 6 floats per vertex — [x, y, r, g, b, a]:
//   - x, y  : position in IMAGE-pixel space (the vertex shader maps this to NDC
//             via frameSize and the shared view transform).
//   - r,g,b,a: color, normalized 0..1.
//
// Stroke widths (line thickness, point size, circle outline) are in IMAGE pixels,
// so they SCALE with zoom — the view transform enlarges the geometry as you zoom
// in. They are FLOORED so a stroke is never thinner than kMinStrokeScreenPx ON
// SCREEN: without that floor a sub-pixel-thin quad can miss every pixel center
// (no anti-aliasing) and drop out of rasterization, which is how rectangle/cube
// edges "disappeared depending on zoom". `image_px_per_screen_px`
// (= 1 / effective_view_scale, where effective_view_scale = on-screen px per image
// px = zoom × letterbox-fit) converts that screen-pixel floor into image units at
// the current zoom — which is why stroke geometry must be re-expanded when the
// view scale changes, not only when the annotation set changes.
//
// Area fills (polygon fill, circle fill) describe a region in image space and so
// scale naturally with zoom with no floor; the fill expanders take no scale arg.

namespace PJ::overlay_geometry {

/// Floats per emitted vertex (vec2 position + vec4 color).
inline constexpr int kFloatsPerVertex = 6;

/// Minimum on-screen stroke width, in screen pixels. Without anti-aliasing a
/// thinner quad can miss every pixel center and disappear; flooring here keeps
/// strokes visible at any zoom.
inline constexpr double kMinStrokeScreenPx = 1.0;

/// Expand the line topologies (kLineLoop / kLineStrip / kLineList) of `pa` into
/// triangles appended to `out`. Width scales with zoom, floored at 1px on screen.
/// kPoints is ignored here (see appendPointQuads). No-op for fewer than 2 points.
void appendLineStrokes(const PointsAnnotation& pa, double image_px_per_screen_px, std::vector<float>& out);

/// Expand kPoints into filled squares appended to `out`. Size scales with zoom,
/// floored at 1px on screen. No-op unless topology is kPoints.
void appendPointQuads(const PointsAnnotation& pa, double image_px_per_screen_px, std::vector<float>& out);

/// Triangle-fan fill of a kLineLoop polygon (convex-only) in image space.
/// No-op unless topology is kLineLoop with a non-transparent fill_color and >=3
/// points. Not affected by zoom.
void appendLoopFill(const PointsAnnotation& pa, std::vector<float>& out);

/// Expand a circle outline into triangles appended to `out`. Both the radius and
/// the outline thickness are image-space (scale with zoom); the thickness is
/// floored at 1px on screen. No-op for an empty/transparent outline.
void appendCircleStroke(const CircleAnnotation& circle, double image_px_per_screen_px, std::vector<float>& out);

/// Triangle-fan fill of a circle in image space (scales with zoom). No-op for an
/// empty/transparent fill.
void appendCircleFill(const CircleAnnotation& circle, std::vector<float>& out);

/// Image-space corners (top-left, top-right, bottom-right, bottom-left) of a
/// `width` x `height` text quad anchored at its top-left, counter-rotated about the
/// anchor so it reads upright once the view turns the image clockwise by
/// `rotation_deg` (0, 90, -90 or 180; anything else is treated as 0). The anchor
/// still rotates with the image.
[[nodiscard]] std::array<Point2, 4> uprightTextQuadCorners(
    const Point2& anchor, double width, double height, int rotation_deg);

}  // namespace PJ::overlay_geometry
