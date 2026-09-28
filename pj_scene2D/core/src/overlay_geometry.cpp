// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_scene2d_core/overlay_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace PJ::overlay_geometry {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Image-space half-width of a stroke. `thickness` is in image pixels, so the
// width SCALES with zoom (the view transform enlarges the geometry as you zoom
// in). It is FLOORED so the stroke is never thinner than kMinStrokeScreenPx ON
// SCREEN — without that floor a sub-pixel stroke can miss every pixel center and
// vanish, which is exactly how rectangle/cube edges disappeared when zoomed out.
// image_px_per_screen_px (= 1 / effective_view_scale) converts the screen-pixel
// floor into image-space units at the current zoom.
double strokeHalfWidth(double thickness, double image_px_per_screen_px) {
  const double floor_image = kMinStrokeScreenPx * image_px_per_screen_px;
  return 0.5 * std::max(thickness, floor_image);
}

void pushVertex(std::vector<float>& out, double x, double y, const ColorRGBA& c) {
  out.push_back(static_cast<float>(x));
  out.push_back(static_cast<float>(y));
  out.push_back(static_cast<float>(c.r) / 255.0f);
  out.push_back(static_cast<float>(c.g) / 255.0f);
  out.push_back(static_cast<float>(c.b) / 255.0f);
  out.push_back(static_cast<float>(c.a) / 255.0f);
}

ColorRGBA vertexColor(const PointsAnnotation& pa, size_t i) {
  if (pa.colors.size() == pa.points.size()) {
    return pa.colors[i];
  }
  return pa.color;
}

// Each segment becomes 2 triangles forming a rectangle perpendicular to ab.
// No miter joins — adjacent segments butt-join with a possible visible gap at
// sharp angles, acceptable for bboxes and gentle polylines. `half_image` is the
// half-width of the stroke in IMAGE-pixel units.
void appendThickSegment(
    std::vector<float>& out, const Point2& a, const Point2& b, const ColorRGBA& ca, const ColorRGBA& cb,
    double half_image) {
  const double dx = b.x - a.x;
  const double dy = b.y - a.y;
  const double len = std::sqrt(dx * dx + dy * dy);
  if (len < 1e-6) {
    return;
  }
  const double nx = -dy / len * half_image;
  const double ny = dx / len * half_image;
  pushVertex(out, a.x - nx, a.y - ny, ca);
  pushVertex(out, a.x + nx, a.y + ny, ca);
  pushVertex(out, b.x + nx, b.y + ny, cb);
  pushVertex(out, a.x - nx, a.y - ny, ca);
  pushVertex(out, b.x + nx, b.y + ny, cb);
  pushVertex(out, b.x - nx, b.y - ny, cb);
}

int circleSegments(double radius) {
  return radius < 10.0 ? 32 : 64;
}

std::vector<Point2> circlePerimeter(const CircleAnnotation& c, int n) {
  std::vector<Point2> out;
  out.reserve(static_cast<size_t>(n));
  const double step = 2.0 * kPi / static_cast<double>(n);
  for (int i = 0; i < n; ++i) {
    const double a = step * static_cast<double>(i);
    out.push_back({c.center.x + c.radius * std::cos(a), c.center.y + c.radius * std::sin(a)});
  }
  return out;
}

}  // namespace

void appendLineStrokes(const PointsAnnotation& pa, double image_px_per_screen_px, std::vector<float>& out) {
  const auto& pts = pa.points;
  if (pts.size() < 2) {
    return;
  }
  const double half_image = strokeHalfWidth(pa.thickness, image_px_per_screen_px);
  switch (pa.topology) {
    case AnnotationTopology::kLineLoop:
      for (size_t i = 0; i + 1 < pts.size(); ++i) {
        appendThickSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1), half_image);
      }
      appendThickSegment(out, pts.back(), pts.front(), vertexColor(pa, pts.size() - 1), vertexColor(pa, 0), half_image);
      break;
    case AnnotationTopology::kLineStrip:
      for (size_t i = 0; i + 1 < pts.size(); ++i) {
        appendThickSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1), half_image);
      }
      break;
    case AnnotationTopology::kLineList:
      for (size_t i = 0; i + 1 < pts.size(); i += 2) {
        appendThickSegment(out, pts[i], pts[i + 1], vertexColor(pa, i), vertexColor(pa, i + 1), half_image);
      }
      break;
    case AnnotationTopology::kPoints:
      break;
  }
}

void appendPointQuads(const PointsAnnotation& pa, double image_px_per_screen_px, std::vector<float>& out) {
  if (pa.topology != AnnotationTopology::kPoints || pa.points.empty()) {
    return;
  }
  const double h = strokeHalfWidth(pa.thickness, image_px_per_screen_px);
  for (size_t i = 0; i < pa.points.size(); ++i) {
    const auto& p = pa.points[i];
    const ColorRGBA c = vertexColor(pa, i);
    pushVertex(out, p.x - h, p.y - h, c);
    pushVertex(out, p.x + h, p.y - h, c);
    pushVertex(out, p.x + h, p.y + h, c);
    pushVertex(out, p.x - h, p.y - h, c);
    pushVertex(out, p.x + h, p.y + h, c);
    pushVertex(out, p.x - h, p.y + h, c);
  }
}

void appendLoopFill(const PointsAnnotation& pa, std::vector<float>& out) {
  if (pa.topology != AnnotationTopology::kLineLoop || pa.fill_color.a == 0 || pa.points.size() < 3) {
    return;
  }
  const Point2& p0 = pa.points[0];
  for (size_t i = 1; i + 1 < pa.points.size(); ++i) {
    pushVertex(out, p0.x, p0.y, pa.fill_color);
    pushVertex(out, pa.points[i].x, pa.points[i].y, pa.fill_color);
    pushVertex(out, pa.points[i + 1].x, pa.points[i + 1].y, pa.fill_color);
  }
}

void appendCircleStroke(const CircleAnnotation& circle, double image_px_per_screen_px, std::vector<float>& out) {
  if (circle.color.a == 0 || circle.radius <= 0.0) {
    return;
  }
  const double half_image = strokeHalfWidth(circle.thickness, image_px_per_screen_px);
  const auto perim = circlePerimeter(circle, circleSegments(circle.radius));
  for (size_t i = 0; i < perim.size(); ++i) {
    appendThickSegment(out, perim[i], perim[(i + 1) % perim.size()], circle.color, circle.color, half_image);
  }
}

void appendCircleFill(const CircleAnnotation& circle, std::vector<float>& out) {
  if (circle.fill_color.a == 0 || circle.radius <= 0.0) {
    return;
  }
  const auto perim = circlePerimeter(circle, circleSegments(circle.radius));
  for (size_t i = 0; i < perim.size(); ++i) {
    const Point2& a = perim[i];
    const Point2& b = perim[(i + 1) % perim.size()];
    pushVertex(out, circle.center.x, circle.center.y, circle.fill_color);
    pushVertex(out, a.x, a.y, circle.fill_color);
    pushVertex(out, b.x, b.y, circle.fill_color);
  }
}

std::array<Point2, 4> uprightTextQuadCorners(const Point2& anchor, double width, double height, int rotation_deg) {
  // Exact quarter-turn cos/sin: no drift, so text stays pixel-aligned.
  const double sin_turn = rotation_deg == 90 ? 1.0 : (rotation_deg == -90 ? -1.0 : 0.0);
  const double cos_turn = rotation_deg == 180 ? -1.0 : (sin_turn == 0.0 ? 1.0 : 0.0);
  // Inverse (counter-clockwise, y-down) turn of each screen-space offset.
  const auto corner = [&](double screen_dx, double screen_dy) {
    return Point2{
        anchor.x + screen_dx * cos_turn + screen_dy * sin_turn, anchor.y - screen_dx * sin_turn + screen_dy * cos_turn};
  };
  return {corner(0.0, 0.0), corner(width, 0.0), corner(width, height), corner(0.0, height)};
}

}  // namespace PJ::overlay_geometry
