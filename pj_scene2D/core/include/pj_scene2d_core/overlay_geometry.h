#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <array>
#include <vector>

#include "pj_scene2d_core/scene_frame.h"

// Tessellates overlay annotations into triangles for the GPU overlay pipelines. A vertex is [x, y, r, g, b, a]:
// position in IMAGE pixels (the vertex shader applies frameSize and the view transform), colour in 0..1.
//
// Stroke widths are image pixels, so they scale with zoom, floored at kMinStrokeScreenPx on screen: with no
// anti-aliasing a thinner quad can miss every pixel centre and vanish. `image_px_per_screen_px` (1 / on-screen px per
// image px) converts the floor, so strokes must be re-expanded when the view scale changes. Fills have no floor.

namespace PJ::overlay_geometry {

inline constexpr int kFloatsPerVertex = 6;

inline constexpr double kMinStrokeScreenPx = 1.0;

/// kLineLoop / kLineStrip / kLineList; no-op for kPoints or fewer than 2 points.
void appendLineStrokes(const PointsAnnotation& pa, double image_px_per_screen_px, std::vector<float>& out);

/// kPoints as filled squares; no-op for any other topology.
void appendPointQuads(const PointsAnnotation& pa, double image_px_per_screen_px, std::vector<float>& out);

/// Triangle fan, so correct for convex polygons only. No-op unless kLineLoop with a non-zero fill alpha and
/// at least 3 points.
void appendLoopFill(const PointsAnnotation& pa, std::vector<float>& out);

/// No-op for an empty or transparent outline.
void appendCircleStroke(const CircleAnnotation& circle, double image_px_per_screen_px, std::vector<float>& out);

/// No-op for an empty or transparent fill.
void appendCircleFill(const CircleAnnotation& circle, std::vector<float>& out);

/// Image-space corners (top-left, top-right, bottom-right, bottom-left) of a
/// `width` x `height` text quad anchored at its top-left, counter-rotated about the
/// anchor so it reads upright once the view turns the image clockwise by
/// `rotation_deg` (0, 90, -90 or 180; anything else is treated as 0). The anchor
/// still rotates with the image.
[[nodiscard]] std::array<Point2, 4> uprightTextQuadCorners(
    const Point2& anchor, double width, double height, int rotation_deg);

}  // namespace PJ::overlay_geometry
