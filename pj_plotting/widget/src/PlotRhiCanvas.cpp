// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotRhiCanvas.h"

#include <pj_widgets/GraphicsApi.h>
#include <qwt_axis.h>
#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_item.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_div.h>
#include <qwt_scale_map.h>
#include <qwt_series_data.h>
#include <qwt_symbol.h>
#include <qwt_text.h>
#include <rhi/qrhi.h>

#include <QByteArray>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QLineF>
#include <QLoggingCategory>
#include <QPainter>
#include <QPicture>
#include <QPointF>
#include <QSize>
#include <QTimer>
#include <QTransform>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <tuple>
#include <vector>

#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotSampleReducer.h"

using namespace Qt::StringLiterals;

void pjPlotRhiInitResources() {
  Q_INIT_RESOURCE(pj_plot_rhi_shaders);
}

namespace PJ {

Q_LOGGING_CATEGORY(lcPlotRhi, "pj.plotting.rhi")

namespace {

using Vertex = PlotRhiVertex;
using TextVertex = PlotRhiTextVertex;

// Hard frame-wide vertex ceiling, enforced inside the builder so it holds even
// when dashed/symbol expansion turns each retained sample into many vertices.
// 1,000,000 vertices at sizeof(Vertex) == 24 B
// is ~24 MB — a firm cap against exhausting the 32-bit wasm heap (a few hundred
// MB total, shared with Qt/Qwt/the datastore), while still far exceeding any
// legible on-screen line density.
constexpr std::size_t kMaxFrameVertices = 1'000'000;
constexpr std::size_t kTextVerticesPerLayer = 6;
constexpr std::size_t kMaxTextLayers = 256;
constexpr std::size_t kMaxTextVertices = kTextVerticesPerLayer * kMaxTextLayers;
constexpr std::size_t kMaxGeometryVertices = kMaxFrameVertices - kMaxTextVertices;
constexpr int kSymbolSegments = 12;
constexpr qreal kTau = 6.28318530717958647692;

// Reducer planning deliberately leaves one quarter of the hard ceiling for
// grids, markers, dash expansion, and estimator error. GeometryBuilder remains
// the final, exact safety boundary.
constexpr std::size_t kPlannedCurveVertices = (kMaxFrameVertices * 3) / 4;

[[nodiscard]] bool finitePoint(const QPointF& point) {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

[[nodiscard]] qreal renderTargetScale(QSize logical_size, QSize target_size) {
  if (logical_size.isEmpty() || target_size.isEmpty()) {
    return 1.0;
  }
  return std::max(
      static_cast<qreal>(target_size.width()) / logical_size.width(),
      static_cast<qreal>(target_size.height()) / logical_size.height());
}

[[nodiscard]] QPointF ndcFromLogical(const QPointF& point, QSize logical_size) {
  if (logical_size.isEmpty()) {
    return {};
  }
  return {
      (point.x() / logical_size.width()) * 2.0 - 1.0,
      1.0 - (point.y() / logical_size.height()) * 2.0,
  };
}

// Fold a double into a running FNV-1a-style fingerprint via its raw bit
// pattern, so equal plots hash equal without touching the payload arrays twice.
void hashDouble(std::uint64_t& state, double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value));
  std::memcpy(&bits, &value, sizeof(bits));
  state = (state ^ bits) * 0x100000001b3ULL;
}

void hashUInt(std::uint64_t& state, std::uint64_t value) {
  state = (state ^ value) * 0x100000001b3ULL;
}

[[nodiscard]] QShader loadShader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  return QShader::fromSerialized(file.readAll());
}

[[nodiscard]] QString backendName(QRhi::Implementation backend) {
  switch (backend) {
    case QRhi::Null:
      return u"Null"_s;
    case QRhi::Vulkan:
      return u"Vulkan"_s;
    case QRhi::OpenGLES2:
      return u"OpenGLES2/WebGL"_s;
    case QRhi::D3D11:
      return u"Direct3D11"_s;
    case QRhi::Metal:
      return u"Metal"_s;
    case QRhi::D3D12:
      return u"Direct3D12"_s;
  }
  return u"Unknown"_s;
}

// Appends triangulated 2D geometry (in logical widget pixels, converted to NDC
// on push) into a caller-owned vertex vector. The vector is reused across
// frames: the caller clear()s it (retaining capacity) before building.
class GeometryBuilder {
 public:
  GeometryBuilder(std::vector<Vertex>& sink, QSize logical_size, qreal device_pixel_ratio)
      : vertices_(sink),
        logical_size_(logical_size),
        cosmetic_width_(1.0 / std::max<qreal>(std::numeric_limits<qreal>::epsilon(), device_pixel_ratio)) {}

  [[nodiscard]] qreal effectivePenWidth(qreal width) const noexcept {
    return width > 0.0 ? width : cosmetic_width_;
  }

  [[nodiscard]] std::size_t vertexCount() const noexcept {
    return vertices_.size();
  }

  // True once the frame-wide vertex ceiling (kMaxFrameVertices) was hit and
  // further geometry was dropped. render() surfaces this as a throttled warning.
  [[nodiscard]] bool truncated() const noexcept {
    return truncated_;
  }

  // Give one curve an exact share of the frame budget. Hitting this sub-limit
  // stops only that curve; endCurveBudget() re-arms the builder for the next
  // curve, preventing an early pathological dashed curve from starving every
  // later item.
  void beginCurveBudget(std::size_t vertex_budget) {
    active_vertex_limit_ = std::min(kMaxGeometryVertices, vertices_.size() + vertex_budget);
    curve_limit_hit_ = false;
  }

  [[nodiscard]] bool endCurveBudget() {
    const bool limited = curve_limit_hit_;
    active_vertex_limit_ = kMaxGeometryVertices;
    curve_limit_hit_ = false;
    return limited;
  }

  void rewind(std::size_t vertex_count, bool truncated) {
    vertices_.resize(std::min(vertex_count, vertices_.size()));
    active_vertex_limit_ = kMaxGeometryVertices;
    curve_limit_hit_ = false;
    truncated_ = truncated;
  }

  void addLine(
      QPointF from, QPointF to, qreal width, const QColor& color, Qt::PenStyle style,
      const std::vector<qreal>& dash_pattern) {
    if (finitePoint(from) && finitePoint(to) && QLineF(from, to).length() <= std::numeric_limits<qreal>::epsilon() &&
        style != Qt::NoPen && color.isValid() && color.alpha() > 0) {
      const qreal point_width = effectivePenWidth(width);
      addRect(QRectF(from - QPointF(point_width * 0.5, point_width * 0.5), QSizeF(point_width, point_width)), color);
      return;
    }
    addPolyline({from, to}, width, color, style, dash_pattern, {});
  }

  void addPolyline(
      const std::vector<QPointF>& points, qreal width, const QColor& color, Qt::PenStyle style,
      const std::vector<qreal>& dash_pattern, const std::vector<qreal>& phase_offsets = {}) {
    if (curve_limit_hit_ || points.size() < 2 || style == Qt::NoPen || !color.isValid() || color.alpha() == 0 ||
        std::any_of(points.cbegin(), points.cend(), [](const QPointF& point) { return !finitePoint(point); })) {
      return;
    }
    const std::vector<QPointF>* stroke_points = &points;
    const std::vector<qreal>* stroke_offsets = phase_offsets.size() == points.size() ? &phase_offsets : nullptr;
    std::vector<QPointF> compact_points;
    std::vector<qreal> compact_offsets;
    const auto duplicate =
        std::adjacent_find(points.cbegin(), points.cend(), [](const QPointF& lhs, const QPointF& rhs) {
          return QLineF(lhs, rhs).length() <= std::numeric_limits<qreal>::epsilon();
        });
    if (duplicate != points.cend()) {
      compact_points.reserve(points.size());
      if (stroke_offsets != nullptr) {
        compact_offsets.reserve(points.size());
      }
      for (std::size_t index = 0; index < points.size(); ++index) {
        const QPointF& point = points[index];
        if (compact_points.empty() ||
            QLineF(compact_points.back(), point).length() > std::numeric_limits<qreal>::epsilon()) {
          compact_points.push_back(point);
          if (stroke_offsets != nullptr) {
            compact_offsets.push_back((*stroke_offsets)[index]);
          }
        } else if (stroke_offsets != nullptr) {
          // A reduced path can return to the same mapped point after traversing
          // skipped source geometry. Keep the later source phase for the next
          // outgoing segment even though no zero-length geometry is emitted.
          compact_offsets.back() = (*stroke_offsets)[index];
        }
      }
      stroke_points = &compact_points;
      stroke_offsets = stroke_offsets != nullptr ? &compact_offsets : nullptr;
    }
    if (stroke_points->size() < 2) {
      return;
    }
    width = effectivePenWidth(width);
    if (dash_pattern.empty()) {
      addSolidPolyline(*stroke_points, width, color);
      return;
    }

    qreal path_offset = 0.0;
    for (std::size_t index = 1; index < stroke_points->size(); ++index) {
      if (curve_limit_hit_) {
        return;
      }
      const qreal length = QLineF((*stroke_points)[index - 1], (*stroke_points)[index]).length();
      const qreal segment_offset = stroke_offsets != nullptr ? (*stroke_offsets)[index - 1] : path_offset;
      const qreal previous_end =
          index > 1 ? (stroke_offsets != nullptr
                           ? (*stroke_offsets)[index - 2] +
                                 QLineF((*stroke_points)[index - 2], (*stroke_points)[index - 1]).length()
                           : segment_offset)
                    : 0.0;
      const qreal next_start = index + 1 < stroke_points->size()
                                   ? (stroke_offsets != nullptr ? (*stroke_offsets)[index] : segment_offset + length)
                                   : 0.0;
      const bool continues_from_previous =
          index > 1 && dashDrawnAt(dash_pattern, previous_end) && dashDrawnAt(dash_pattern, segment_offset);
      const bool continues_to_next = index + 1 < stroke_points->size() &&
                                     dashDrawnAt(dash_pattern, segment_offset + length) &&
                                     dashDrawnAt(dash_pattern, next_start);
      addDashedSegment(
          (*stroke_points)[index - 1], (*stroke_points)[index], width, color, dash_pattern, segment_offset,
          continues_from_previous, continues_to_next);
      path_offset += length;
      if (continues_to_next) {
        addBevelJoin((*stroke_points)[index - 1], (*stroke_points)[index], (*stroke_points)[index + 1], width, color);
      }
    }
  }

  // Convenience for one-off lines (grid/marker) with no cached dash pattern.
  void addLine(QPointF from, QPointF to, qreal width, const QColor& color, Qt::PenStyle style = Qt::SolidLine) {
    addLine(from, to, width, color, style, dashPattern(style, effectivePenWidth(width)));
  }

  void addEllipse(const QPointF& center, const QSizeF& radii, const QColor& color) {
    if (curve_limit_hit_ || !finitePoint(center) || radii.width() <= 0.0 || radii.height() <= 0.0 || !color.isValid() ||
        color.alpha() == 0) {
      return;
    }
    for (int index = 0; index < kSymbolSegments; ++index) {
      const qreal angle_a = kTau * static_cast<qreal>(index) / kSymbolSegments;
      const qreal angle_b = kTau * static_cast<qreal>(index + 1) / kSymbolSegments;
      const QPointF edge_a(
          center.x() + std::cos(angle_a) * radii.width(), center.y() + std::sin(angle_a) * radii.height());
      const QPointF edge_b(
          center.x() + std::cos(angle_b) * radii.width(), center.y() + std::sin(angle_b) * radii.height());
      addTriangle(center, edge_a, edge_b, color);
    }
  }

  void addEllipseRing(
      const QPointF& center, const QSizeF& outer_radii, const QSizeF& inner_radii, const QColor& color) {
    if (curve_limit_hit_ || !finitePoint(center) || outer_radii.width() <= 0.0 || outer_radii.height() <= 0.0 ||
        inner_radii.width() <= 0.0 || inner_radii.height() <= 0.0 || inner_radii.width() >= outer_radii.width() ||
        inner_radii.height() >= outer_radii.height() || !color.isValid() || color.alpha() == 0) {
      return;
    }
    for (int index = 0; index < kSymbolSegments; ++index) {
      const qreal angle_a = kTau * static_cast<qreal>(index) / kSymbolSegments;
      const qreal angle_b = kTau * static_cast<qreal>(index + 1) / kSymbolSegments;
      const QPointF outer_a(
          center.x() + std::cos(angle_a) * outer_radii.width(), center.y() + std::sin(angle_a) * outer_radii.height());
      const QPointF outer_b(
          center.x() + std::cos(angle_b) * outer_radii.width(), center.y() + std::sin(angle_b) * outer_radii.height());
      const QPointF inner_a(
          center.x() + std::cos(angle_a) * inner_radii.width(), center.y() + std::sin(angle_a) * inner_radii.height());
      const QPointF inner_b(
          center.x() + std::cos(angle_b) * inner_radii.width(), center.y() + std::sin(angle_b) * inner_radii.height());
      addTriangle(outer_a, inner_a, outer_b, color);
      addTriangle(inner_a, inner_b, outer_b, color);
    }
  }

  void addRect(const QRectF& rect, const QColor& color) {
    if (curve_limit_hit_ || !rect.isValid() || !finitePoint(rect.topLeft()) || !finitePoint(rect.bottomRight()) ||
        !color.isValid() || color.alpha() == 0 || !rect.intersects(QRectF(QPointF(0.0, 0.0), QSizeF(logical_size_)))) {
      return;
    }
    addTriangle(rect.topLeft(), rect.bottomLeft(), rect.bottomRight(), color);
    addTriangle(rect.topLeft(), rect.bottomRight(), rect.topRight(), color);
  }

  [[nodiscard]] static std::vector<qreal> dashPattern(Qt::PenStyle style, qreal scale) {
    switch (style) {
      case Qt::DashLine:
        return {4.0 * scale, 2.0 * scale};
      case Qt::DotLine:
        return {1.0 * scale, 2.0 * scale};
      case Qt::DashDotLine:
        return {4.0 * scale, 2.0 * scale, 1.0 * scale, 2.0 * scale};
      case Qt::DashDotDotLine:
        return {4.0 * scale, 2.0 * scale, 1.0 * scale, 2.0 * scale, 1.0 * scale, 2.0 * scale};
      default:
        return {};
    }
  }

 private:
  [[nodiscard]] bool clipLine(const QPointF& from, const QPointF& to, qreal& lower, qreal& upper) const {
    const QRectF bounds(QPointF(0.0, 0.0), QSizeF(logical_size_));
    const qreal dx = to.x() - from.x();
    const qreal dy = to.y() - from.y();
    lower = 0.0;
    upper = 1.0;
    const std::array<qreal, 4> p = {-dx, dx, -dy, dy};
    const std::array<qreal, 4> q = {
        from.x() - bounds.left(), bounds.right() - from.x(), from.y() - bounds.top(), bounds.bottom() - from.y()};
    for (std::size_t index = 0; index < p.size(); ++index) {
      if (std::abs(p[index]) <= std::numeric_limits<qreal>::epsilon()) {
        if (q[index] < 0.0) {
          return false;
        }
        continue;
      }
      const qreal ratio = q[index] / p[index];
      if (p[index] < 0.0) {
        lower = std::max(lower, ratio);
      } else {
        upper = std::min(upper, ratio);
      }
      if (lower > upper) {
        return false;
      }
    }
    return true;
  }

  void addSolidLine(
      const QPointF& from, const QPointF& to, qreal width, const QColor& color, bool extend_start, bool extend_end) {
    const QLineF line(from, to);
    if (line.length() <= std::numeric_limits<qreal>::epsilon()) {
      return;
    }
    const QPointF direction(line.dx() / line.length(), line.dy() / line.length());
    const QPointF normal(-direction.y(), direction.x());
    const qreal half_width = width * 0.5;
    const QPointF offset = normal * half_width;
    const QPointF start = extend_start ? from - (direction * half_width) : from;
    const QPointF end = extend_end ? to + (direction * half_width) : to;
    addTriangle(start - offset, end - offset, end + offset, color);
    addTriangle(start - offset, end + offset, start + offset, color);
  }

  void addSolidPolyline(const std::vector<QPointF>& points, qreal width, const QColor& color) {
    std::size_t first_segment = points.size();
    std::size_t last_segment = 0;
    for (std::size_t index = 1; index < points.size(); ++index) {
      if (QLineF(points[index - 1], points[index]).length() > std::numeric_limits<qreal>::epsilon()) {
        first_segment = std::min(first_segment, index);
        last_segment = index;
      }
    }
    if (first_segment == points.size()) {
      return;
    }
    for (std::size_t index = first_segment; index <= last_segment; ++index) {
      if (curve_limit_hit_) {
        return;
      }
      QPointF from = points[index - 1];
      QPointF to = points[index];
      if (QLineF(from, to).length() <= std::numeric_limits<qreal>::epsilon()) {
        continue;
      }
      qreal lower = 0.0;
      qreal upper = 1.0;
      if (clipLine(from, to, lower, upper)) {
        const QPointF delta = to - from;
        addSolidLine(
            from + (delta * lower), from + (delta * upper), width, color, index == first_segment,
            index == last_segment);
      }
      if (index < last_segment) {
        addBevelJoin(points[index - 1], points[index], points[index + 1], width, color);
      }
    }
  }

  void addDashedSegment(
      const QPointF& from, const QPointF& to, qreal width, const QColor& color, const std::vector<qreal>& dash_pattern,
      qreal path_offset, bool continues_from_previous, bool continues_to_next) {
    const QLineF line(from, to);
    const qreal length = line.length();
    if (!std::isfinite(length) || length <= std::numeric_limits<qreal>::epsilon()) {
      return;
    }
    qreal lower = 0.0;
    qreal upper = 1.0;
    if (!clipLine(from, to, lower, upper)) {
      return;
    }
    const QPointF delta = to - from;
    const QPointF clipped_from = from + (delta * lower);
    const QPointF clipped_to = from + (delta * upper);
    const qreal clipped_length = QLineF(clipped_from, clipped_to).length();
    if (!std::isfinite(clipped_length) || clipped_length <= std::numeric_limits<qreal>::epsilon()) {
      return;
    }
    const QPointF direction = (clipped_to - clipped_from) / clipped_length;
    const qreal pattern_length = std::accumulate(dash_pattern.cbegin(), dash_pattern.cend(), 0.0);
    if (!std::isfinite(pattern_length) || pattern_length <= 0.0) {
      return;
    }
    const auto phase_component = [pattern_length](qreal offset) {
      return std::isfinite(offset) ? std::fmod(std::max<qreal>(0.0, offset), pattern_length) : 0.0;
    };
    // Normalize each potentially huge component before adding it. Keeping the
    // loop phase canvas-sized is essential: at offsets around 1e100, adding a
    // local cursor may otherwise round back to the same value for every dash.
    const qreal clipped_path_phase =
        std::fmod(phase_component(path_offset) + phase_component(lower * length), pattern_length);
    qreal cursor = 0.0;
    const qreal end = clipped_length;
    while (cursor < end) {
      if (curve_limit_hit_) {
        return;
      }
      const auto [pattern_index, remaining] = dashStateAt(dash_pattern, clipped_path_phase + cursor);
      qreal next = std::min(end, cursor + remaining);
      if (next <= cursor) {
        // fmod() can leave a sub-ULP remainder at a dash boundary. nextafter()
        // is the only addition-independent progress guarantee when cursor is
        // large; the local clipped distance normally keeps it canvas-sized.
        next = std::nextafter(cursor, end);
      }
      if ((pattern_index % 2U) == 0U && next > cursor) {
        constexpr qreal kBoundaryEpsilon = 1e-6;
        const bool connected_start =
            continues_from_previous && (lower * length) <= kBoundaryEpsilon && cursor <= kBoundaryEpsilon;
        const bool connected_end = continues_to_next && ((1.0 - upper) * length) <= kBoundaryEpsilon &&
                                   std::abs(next - end) <= kBoundaryEpsilon;
        addSolidLine(
            clipped_from + (direction * cursor), clipped_from + (direction * next), width, color, !connected_start,
            !connected_end);
      }
      cursor = next;
    }
  }

  [[nodiscard]] static std::pair<std::size_t, qreal> dashStateAt(const std::vector<qreal>& pattern, qreal offset) {
    const qreal total = std::accumulate(pattern.cbegin(), pattern.cend(), 0.0);
    qreal phase = total > 0.0 ? std::fmod(std::max<qreal>(0.0, offset), total) : 0.0;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
      constexpr qreal kDashBoundaryEpsilon = 1e-6;
      if (phase < pattern[index] - kDashBoundaryEpsilon) {
        return {index, pattern[index] - phase};
      }
      phase = std::max<qreal>(0.0, phase - pattern[index]);
    }
    return {0, pattern.front()};
  }

  [[nodiscard]] static bool dashDrawnAt(const std::vector<qreal>& pattern, qreal offset) {
    constexpr qreal kDashBoundaryEpsilon = 1e-6;
    const auto before = dashStateAt(pattern, std::max<qreal>(0.0, offset - kDashBoundaryEpsilon)).first;
    const auto after = dashStateAt(pattern, offset + kDashBoundaryEpsilon).first;
    return (before % 2U) == 0U && (after % 2U) == 0U;
  }

  void addBevelJoin(
      const QPointF& previous, const QPointF& point, const QPointF& next, qreal width, const QColor& color) {
    const QLineF incoming(previous, point);
    const QLineF outgoing(point, next);
    if (incoming.length() <= std::numeric_limits<qreal>::epsilon() ||
        outgoing.length() <= std::numeric_limits<qreal>::epsilon()) {
      return;
    }
    const QPointF incoming_direction(incoming.dx() / incoming.length(), incoming.dy() / incoming.length());
    const QPointF outgoing_direction(outgoing.dx() / outgoing.length(), outgoing.dy() / outgoing.length());
    const qreal cross =
        (incoming_direction.x() * outgoing_direction.y()) - (incoming_direction.y() * outgoing_direction.x());
    if (std::abs(cross) <= std::numeric_limits<qreal>::epsilon()) {
      return;
    }
    if (!QRectF(QPointF(0.0, 0.0), QSizeF(logical_size_)).adjusted(-width, -width, width, width).contains(point)) {
      return;
    }
    const qreal side = cross > 0.0 ? -1.0 : 1.0;
    const qreal half_width = width * 0.5;
    const QPointF incoming_normal(-incoming_direction.y(), incoming_direction.x());
    const QPointF outgoing_normal(-outgoing_direction.y(), outgoing_direction.x());
    addTriangle(
        point, point + (incoming_normal * half_width * side), point + (outgoing_normal * half_width * side), color);
  }

  void addTriangle(const QPointF& a, const QPointF& b, const QPointF& c, const QColor& color) {
    // Enforce the ceiling per whole triangle so the buffer never holds a partial
    // primitive. All builder geometry (lines/ellipses/rects) funnels through here.
    if (vertices_.size() + 3 > kMaxGeometryVertices) {
      truncated_ = true;
      curve_limit_hit_ = true;
      return;
    }
    if (vertices_.size() + 3 > active_vertex_limit_) {
      curve_limit_hit_ = true;
      return;
    }
    addVertex(a, color);
    addVertex(b, color);
    addVertex(c, color);
  }

  void addVertex(const QPointF& point, const QColor& color) {
    if (logical_size_.width() <= 0 || logical_size_.height() <= 0) {
      return;
    }
    const QPointF ndc = ndcFromLogical(point, logical_size_);
    vertices_.push_back(
        Vertex{
            .x = static_cast<float>(ndc.x()),
            .y = static_cast<float>(ndc.y()),
            .red = static_cast<float>(color.redF()),
            .green = static_cast<float>(color.greenF()),
            .blue = static_cast<float>(color.blueF()),
            .alpha = static_cast<float>(color.alphaF()),
        });
  }

  std::vector<Vertex>& vertices_;
  QSize logical_size_;
  qreal cosmetic_width_ = 1.0;
  std::size_t active_vertex_limit_ = kMaxGeometryVertices;
  bool curve_limit_hit_ = false;
  bool truncated_ = false;
};

enum class TextLayerKind { Legend, Marker };

struct TextRasterLayer {
  QImage image;
  QRectF logical_rect;
  QPoint atlas_position;
  qsizetype alpha_pixels = 0;
  const QwtPlotItem* source_item = nullptr;
  TextLayerKind kind = TextLayerKind::Marker;
};

struct TextRasterBuild {
  std::vector<TextRasterLayer> layers;
  QRectF logical_bounds;
  qsizetype alpha_pixels = 0;
  qsizetype legend_layers = 0;
  qsizetype marker_layers = 0;
  bool truncated = false;
  bool allocation_failed = false;
};

void refreshTextRasterStats(TextRasterBuild& build) {
  build.logical_bounds = {};
  build.alpha_pixels = 0;
  build.legend_layers = 0;
  build.marker_layers = 0;
  for (const TextRasterLayer& layer : build.layers) {
    build.logical_bounds =
        build.logical_bounds.isValid() ? build.logical_bounds.united(layer.logical_rect) : layer.logical_rect;
    build.alpha_pixels += layer.alpha_pixels;
    if (layer.kind == TextLayerKind::Legend) {
      ++build.legend_layers;
    } else {
      ++build.marker_layers;
    }
  }
}

class LabelOnlyMarker final : public QwtPlotMarker {
 protected:
  void drawLines(QPainter*, const QRectF&, const QPointF&) const override {}
  void drawSymbol(QPainter*, const QRectF&, const QPointF&) const override {}
};

[[nodiscard]] QRectF markerLabelBounds(
    const QwtPlotMarker& marker, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& canvas_rect,
    const QFont& font) {
  const QPointF position(x_map.transform(marker.xValue()), y_map.transform(marker.yValue()));
  Qt::Alignment alignment = marker.labelAlignment();
  QPointF aligned_position = position;
  QSizeF symbol_offset;

  switch (marker.lineStyle()) {
    case QwtPlotMarker::VLine:
      if (alignment & Qt::AlignTop) {
        aligned_position.setY(canvas_rect.top());
        alignment &= ~Qt::AlignTop;
        alignment |= Qt::AlignBottom;
      } else if (alignment & Qt::AlignBottom) {
        aligned_position.setY(canvas_rect.bottom() - 1.0);
        alignment &= ~Qt::AlignBottom;
        alignment |= Qt::AlignTop;
      } else {
        aligned_position.setY(canvas_rect.center().y());
      }
      break;
    case QwtPlotMarker::HLine:
      if (alignment & Qt::AlignLeft) {
        aligned_position.setX(canvas_rect.left());
        alignment &= ~Qt::AlignLeft;
        alignment |= Qt::AlignRight;
      } else if (alignment & Qt::AlignRight) {
        aligned_position.setX(canvas_rect.right() - 1.0);
        alignment &= ~Qt::AlignRight;
        alignment |= Qt::AlignLeft;
      } else {
        aligned_position.setX(canvas_rect.center().x());
      }
      break;
    default:
      if (const QwtSymbol* symbol = marker.symbol(); symbol != nullptr && symbol->style() != QwtSymbol::NoSymbol) {
        symbol_offset = (symbol->size() + QSizeF(1.0, 1.0)) / 2.0;
      }
      break;
  }

  qreal half_pen_width = marker.linePen().widthF() / 2.0;
  if (half_pen_width == 0.0) {
    half_pen_width = 0.5;
  }
  const qreal x_offset = std::max(half_pen_width, symbol_offset.width());
  const qreal y_offset = std::max(half_pen_width, symbol_offset.height());
  const QSizeF text_size = marker.label().textSize(font);
  const bool vertical = marker.labelOrientation() == Qt::Vertical;

  if (alignment & Qt::AlignLeft) {
    aligned_position.rx() -= x_offset + marker.spacing() + (vertical ? text_size.height() : text_size.width());
  } else if (alignment & Qt::AlignRight) {
    aligned_position.rx() += x_offset + marker.spacing();
  } else {
    aligned_position.rx() -= (vertical ? text_size.height() : text_size.width()) / 2.0;
  }
  if (alignment & Qt::AlignTop) {
    aligned_position.ry() -= y_offset + marker.spacing() + (vertical ? 0.0 : text_size.height());
  } else if (alignment & Qt::AlignBottom) {
    aligned_position.ry() += y_offset + marker.spacing() + (vertical ? text_size.width() : 0.0);
  } else {
    aligned_position.ry() += vertical ? text_size.width() / 2.0 : -text_size.height() / 2.0;
  }

  QTransform placement;
  placement.translate(aligned_position.x(), aligned_position.y());
  if (vertical) {
    placement.rotate(-90.0);
  }
  return placement.mapRect(QRectF(QPointF(), text_size));
}

[[nodiscard]] bool prepareTextRasterScratch(QImage& scratch, const QSizeF& logical_size, qreal dpr) {
  const QSize physical_size(
      std::max(1, static_cast<int>(std::ceil(logical_size.width() * dpr))),
      std::max(1, static_cast<int>(std::ceil(logical_size.height() * dpr))));
  if (scratch.format() != QImage::Format_RGBA8888_Premultiplied || scratch.width() < physical_size.width() ||
      scratch.height() < physical_size.height()) {
    const QSize capacity(
        std::max(scratch.width(), physical_size.width()), std::max(scratch.height(), physical_size.height()));
    QImage replacement(capacity, QImage::Format_RGBA8888_Premultiplied);
    if (replacement.isNull()) {
      return false;
    }
    scratch = std::move(replacement);
  }
  scratch.setDevicePixelRatio(dpr);
  for (int y = 0; y < physical_size.height(); ++y) {
    std::memset(scratch.scanLine(y), 0, static_cast<std::size_t>(physical_size.width()) * 4U);
  }
  return true;
}

struct AlphaRasterScan {
  QRect bounds;
  qsizetype pixels = 0;
};

[[nodiscard]] AlphaRasterScan scanTextAlpha(const QImage& image, QSize active_size) {
  active_size = active_size.boundedTo(image.size());
  int min_x = active_size.width();
  int min_y = active_size.height();
  int max_x = -1;
  int max_y = -1;
  qsizetype pixels = 0;
  for (int y = 0; y < active_size.height(); ++y) {
    const uchar* row = image.constScanLine(y);
    for (int x = 0; x < active_size.width(); ++x) {
      // Format_RGBA8888_Premultiplied is byte ordered RGBA on every host.
      if (row[(x * 4) + 3] == 0) {
        continue;
      }
      ++pixels;
      min_x = std::min(min_x, x);
      min_y = std::min(min_y, y);
      max_x = std::max(max_x, x);
      max_y = std::max(max_y, y);
    }
  }
  return {
      .bounds = max_x >= min_x && max_y >= min_y ? QRect(QPoint(min_x, min_y), QPoint(max_x, max_y)) : QRect(),
      .pixels = pixels,
  };
}

[[nodiscard]] bool cropTextRaster(
    const QImage& source, QSize active_size, QRectF& logical_rect, qreal dpr, QImage& cropped, qsizetype& alpha_pixels,
    bool& allocation_failed) {
  const AlphaRasterScan scan = scanTextAlpha(source, active_size);
  if (scan.pixels == 0 || scan.bounds.isEmpty()) {
    cropped = {};
    logical_rect = {};
    return false;
  }
  alpha_pixels = scan.pixels;
  const QRect physical_rect = scan.bounds;
  cropped = source.copy(physical_rect);
  if (cropped.isNull()) {
    logical_rect = {};
    allocation_failed = true;
    return false;
  }
  cropped.setDevicePixelRatio(1.0);
  logical_rect = QRectF(
      QPointF(static_cast<qreal>(physical_rect.left()) / dpr, static_cast<qreal>(physical_rect.top()) / dpr),
      QSizeF(static_cast<qreal>(physical_rect.width()) / dpr, static_cast<qreal>(physical_rect.height()) / dpr));
  return true;
}

template <typename Draw>
void appendRecordedTextLayer(
    TextRasterBuild& result, QImage& scratch, TextLayerKind kind, const QRectF& canvas_rect, qreal dpr,
    const QFont& font, const QwtPlotItem* source_item, Draw&& draw, const QRectF& bounds_hint = {}) {
  if (result.layers.size() >= kMaxTextLayers) {
    result.truncated = true;
    return;
  }

  // Legends have no cheap bounds API, so record their existing Qwt renderer
  // once to discover the touched rectangle and replay that picture into the
  // raster. Marker labels already have an exact bounds hint and draw directly.
  // In both cases Qwt's item renderer executes only once per rebuild.
  QPicture picture;
  const bool recorded = !bounds_hint.isValid();
  if (recorded) {
    QPainter recorder(&picture);
    recorder.setFont(font);
    recorder.setClipRect(canvas_rect);
    recorder.setRenderHint(QPainter::Antialiasing, true);
    recorder.setRenderHint(QPainter::TextAntialiasing, true);
    draw(&recorder);
    recorder.end();
  }

  const QRectF logical_bounds = recorded ? QRectF(picture.boundingRect()) : bounds_hint;
  QRect recorded_bounds = logical_bounds.toAlignedRect().adjusted(-2, -2, 2, 2);
  recorded_bounds = recorded_bounds.intersected(canvas_rect.toAlignedRect());
  if (recorded_bounds.isEmpty()) {
    return;
  }

  if (!prepareTextRasterScratch(scratch, recorded_bounds.size(), dpr)) {
    result.allocation_failed = true;
    return;
  }
  QPainter raster(&scratch);
  raster.setFont(font);
  raster.setRenderHint(QPainter::Antialiasing, true);
  raster.setRenderHint(QPainter::TextAntialiasing, true);
  raster.setClipRect(QRectF(QPointF(), QSizeF(recorded_bounds.size())));
  raster.translate(-recorded_bounds.left(), -recorded_bounds.top());
  if (recorded) {
    picture.play(&raster);
  } else {
    draw(&raster);
  }
  raster.end();

  QRectF local_rect;
  QImage image;
  qsizetype alpha_pixels = 0;
  const QSize active_size(
      std::max(1, static_cast<int>(std::ceil(recorded_bounds.width() * dpr))),
      std::max(1, static_cast<int>(std::ceil(recorded_bounds.height() * dpr))));
  if (!cropTextRaster(scratch, active_size, local_rect, dpr, image, alpha_pixels, result.allocation_failed)) {
    return;
  }
  local_rect.translate(recorded_bounds.topLeft());
  result.layers.push_back({
      .image = std::move(image),
      .logical_rect = local_rect,
      .alpha_pixels = alpha_pixels,
      .source_item = source_item,
      .kind = kind,
  });
}

[[nodiscard]] TextRasterBuild buildTextRasters(
    const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio, QImage& scratch) {
  TextRasterBuild result;
  if (plot == nullptr || logical_size.isEmpty() || device_pixel_ratio <= 0.0) {
    return result;
  }
  const qreal dpr = device_pixel_ratio;
  const QRectF canvas_rect(QPointF(0.0, 0.0), QSizeF(logical_size));
  const QFont text_font = plot->canvas() != nullptr ? plot->canvas()->font() : plot->font();
  const QwtScaleMap bottom_map = plot->canvasMap(QwtPlot::xBottom);
  const QwtScaleMap left_map = plot->canvasMap(QwtPlot::yLeft);
  for (const QwtPlotItem* item : plot->itemList()) {
    if (const auto* legend = dynamic_cast<const PlotLegend*>(item); legend != nullptr && legend->isVisible()) {
      appendRecordedTextLayer(
          result, scratch, TextLayerKind::Legend, canvas_rect, dpr, text_font, item,
          [item, &bottom_map, &left_map, &canvas_rect](QPainter* painter) {
            item->draw(painter, bottom_map, left_map, canvas_rect);
          });
      continue;
    }
    const auto* marker = dynamic_cast<const QwtPlotMarker*>(item);
    if (marker == nullptr || !marker->isVisible() || marker->label().isEmpty()) {
      continue;
    }
    LabelOnlyMarker label_marker;
    label_marker.setValue(marker->value());
    label_marker.setLineStyle(marker->lineStyle());
    label_marker.setLinePen(marker->linePen());
    label_marker.setLabel(marker->label());
    label_marker.setLabelAlignment(marker->labelAlignment());
    label_marker.setLabelOrientation(marker->labelOrientation());
    label_marker.setSpacing(marker->spacing());
    if (const QwtSymbol* source_symbol = marker->symbol();
        source_symbol != nullptr && source_symbol->style() != QwtSymbol::NoSymbol) {
      auto* placement_symbol = new QwtSymbol(source_symbol->style());
      placement_symbol->setSize(source_symbol->size());
      label_marker.setSymbol(placement_symbol);
    }
    const QwtScaleMap marker_x_map = plot->canvasMap(marker->xAxis());
    const QwtScaleMap marker_y_map = plot->canvasMap(marker->yAxis());
    appendRecordedTextLayer(
        result, scratch, TextLayerKind::Marker, canvas_rect, dpr, text_font, item,
        [&label_marker, &marker_x_map, &marker_y_map, &canvas_rect](QPainter* painter) {
          label_marker.draw(painter, marker_x_map, marker_y_map, canvas_rect);
        },
        markerLabelBounds(label_marker, marker_x_map, marker_y_map, canvas_rect, text_font));
  }
  refreshTextRasterStats(result);
  return result;
}

[[nodiscard]] bool clampTextLayersToTextureLimit(TextRasterBuild& build, int max_texture_size) {
  const int payload_limit = max_texture_size - 2;  // one transparent sampler pad on each side
  if (payload_limit <= 0) {
    build.layers.clear();
    build.truncated = true;
    refreshTextRasterStats(build);
    return true;
  }
  for (TextRasterLayer& layer : build.layers) {
    if (layer.image.width() <= payload_limit && layer.image.height() <= payload_limit) {
      continue;
    }
    const QSize clipped_size(
        std::min(layer.image.width(), payload_limit), std::min(layer.image.height(), payload_limit));
    QImage clipped = layer.image.copy(QRect(QPoint(), clipped_size));
    if (clipped.isNull()) {
      build.allocation_failed = true;
      return false;
    }
    clipped.setDevicePixelRatio(1.0);
    const qreal logical_width =
        layer.logical_rect.width() * static_cast<qreal>(clipped_size.width()) / layer.image.width();
    const qreal logical_height =
        layer.logical_rect.height() * static_cast<qreal>(clipped_size.height()) / layer.image.height();
    layer.image = std::move(clipped);
    layer.logical_rect.setSize(QSizeF(logical_width, logical_height));
    layer.alpha_pixels = scanTextAlpha(layer.image, layer.image.size()).pixels;
    build.truncated = true;
  }
  refreshTextRasterStats(build);
  return true;
}

[[nodiscard]] bool packTextAtlas(TextRasterBuild& build, int max_texture_size, QImage& atlas) {
  if (!clampTextLayersToTextureLimit(build, max_texture_size)) {
    return false;
  }
  if (build.layers.empty()) {
    return true;
  }
  std::uint64_t total_area = 0;
  int widest = 0;
  for (const TextRasterLayer& layer : build.layers) {
    const int padded_width = layer.image.width() + 2;
    const int padded_height = layer.image.height() + 2;
    widest = std::max(widest, padded_width);
    total_area += static_cast<std::uint64_t>(padded_width) * padded_height;
  }

  auto place = [&build, max_texture_size](int atlas_width) {
    int x = 0;
    int y = 0;
    int row_height = 0;
    int used_width = 0;
    std::size_t packed = 0;
    for (TextRasterLayer& layer : build.layers) {
      const int padded_width = layer.image.width() + 2;
      const int padded_height = layer.image.height() + 2;
      if (x > 0 && x + padded_width > atlas_width) {
        x = 0;
        y += row_height;
        row_height = 0;
      }
      if (y + padded_height > max_texture_size) {
        break;
      }
      layer.atlas_position = QPoint(x + 1, y + 1);
      x += padded_width;
      row_height = std::max(row_height, padded_height);
      used_width = std::max(used_width, x);
      ++packed;
    }
    return std::tuple{packed, QSize(used_width, y + row_height)};
  };

  const int square_width = static_cast<int>(std::ceil(std::sqrt(static_cast<long double>(total_area))));
  int atlas_width = std::clamp(std::max(widest, square_width), widest, max_texture_size);
  auto [packed, used_size] = place(atlas_width);
  if (packed < build.layers.size() && atlas_width < max_texture_size) {
    atlas_width = max_texture_size;
    std::tie(packed, used_size) = place(atlas_width);
  }
  if (packed < build.layers.size()) {
    build.layers.resize(packed);
    build.truncated = true;
  }
  if (build.layers.empty() || used_size.isEmpty()) {
    refreshTextRasterStats(build);
    return true;
  }

  atlas = QImage(used_size, QImage::Format_RGBA8888_Premultiplied);
  if (atlas.isNull()) {
    build.allocation_failed = true;
    return false;
  }
  atlas.fill(Qt::transparent);
  QPainter painter(&atlas);
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  for (const TextRasterLayer& layer : build.layers) {
    painter.drawImage(layer.atlas_position, layer.image);
  }
  painter.end();
  refreshTextRasterStats(build);
  return true;
}

void appendTextQuad(std::vector<TextVertex>& vertices, const QRectF& rect, const QRectF& uv_rect, QSize logical_size) {
  if (!rect.isValid() || logical_size.isEmpty()) {
    return;
  }
  const QPointF top_left = ndcFromLogical(rect.topLeft(), logical_size);
  const QPointF bottom_right = ndcFromLogical(rect.bottomRight(), logical_size);
  const float left = static_cast<float>(top_left.x());
  const float right = static_cast<float>(bottom_right.x());
  const float top = static_cast<float>(top_left.y());
  const float bottom = static_cast<float>(bottom_right.y());
  const float u0 = static_cast<float>(uv_rect.left());
  const float u1 = static_cast<float>(uv_rect.right());
  const float v0 = static_cast<float>(uv_rect.top());
  const float v1 = static_cast<float>(uv_rect.bottom());
  vertices.insert(
      vertices.end(), {
                          {left, top, u0, v0},
                          {right, top, u1, v0},
                          {right, bottom, u1, v1},
                          {left, top, u0, v0},
                          {right, bottom, u1, v1},
                          {left, bottom, u0, v1},
                      });
  Q_ASSERT(vertices.size() <= kMaxTextVertices);
}

[[nodiscard]] std::size_t estimatedVerticesPerSample(const QwtPlotCurve* curve) {
  if (curve == nullptr) {
    return 1;
  }
  std::size_t cost = 6;
  switch (curve->style()) {
    case QwtPlotCurve::NoCurve:
      cost = 1;
      break;
    case QwtPlotCurve::Lines:
      // One quad per segment plus one bevel-join triangle per interior
      // sample. Square caps are folded into the endpoint quads.
      cost = 9;
      break;
    case QwtPlotCurve::Sticks:
      cost = 6;
      break;
    case QwtPlotCurve::Steps:
      // Two segments and (asymptotically) two joins per source interval.
      cost = 18;
      break;
    case QwtPlotCurve::Dots:
      // QPainter's default SquareCap turns each drawPoint into one square.
      cost = 6;
      break;
    case QwtPlotCurve::UserCurve:
      cost = 18;
      break;
  }
  if (curve->pen().style() != Qt::SolidLine && curve->pen().style() != Qt::NoPen) {
    // Dashes expand a segment into several quads. This estimate is intentionally
    // conservative; the exact builder ceiling still handles unusually long
    // dash spans.
    cost *= 4;
  }
  if (const QwtSymbol* symbol = curve->symbol(); symbol != nullptr && symbol->style() != QwtSymbol::NoSymbol) {
    if (symbol->style() == QwtSymbol::Ellipse) {
      // Filled ellipse: 12 triangles. Its pen is a 12-segment ring (24
      // triangles), or one outer disc when the requested symbol is too small
      // for a hollow center. Reserve the larger app-visible fill+ring case.
      cost += static_cast<std::size_t>(kSymbolSegments * 9);
    } else {
      cost += 6;
    }
  }
  return cost;
}

[[nodiscard]] bool isRenderableCurve(const QwtPlotCurve* curve) {
  if (curve == nullptr || !curve->isVisible() || curve->data() == nullptr || curve->dataSize() == 0) {
    return false;
  }
  if (curve->style() != QwtPlotCurve::NoCurve) {
    return true;
  }
  const QwtSymbol* symbol = curve->symbol();
  return symbol != nullptr && symbol->style() != QwtSymbol::NoSymbol;
}

void appendGrid(GeometryBuilder& geometry, const QwtPlot* plot, const QwtPlotGrid* grid) {
  if (grid == nullptr || !grid->isVisible()) {
    return;
  }
  const auto append_ticks = [&](QwtAxisId axis, bool enabled, bool minor_enabled) {
    if (!enabled) {
      return;
    }
    const QwtScaleMap map = plot->canvasMap(axis);
    const QwtScaleDiv& division = plot->axisScaleDiv(axis);
    const auto append = [&](int tick_type, const QPen& pen) {
      for (double tick : division.ticks(tick_type)) {
        const qreal pixel = map.transform(tick);
        if (QwtAxis::isXAxis(axis)) {
          geometry.addLine(
              QPointF(pixel, 0.0), QPointF(pixel, plot->canvas()->height()), pen.widthF(), pen.color(), pen.style());
        } else {
          geometry.addLine(
              QPointF(0.0, pixel), QPointF(plot->canvas()->width(), pixel), pen.widthF(), pen.color(), pen.style());
        }
      }
    };
    append(QwtScaleDiv::MajorTick, grid->majorPen());
    if (minor_enabled) {
      append(QwtScaleDiv::MediumTick, grid->minorPen());
      append(QwtScaleDiv::MinorTick, grid->minorPen());
    }
  };
  append_ticks(QwtPlot::xBottom, grid->xEnabled(), grid->xMinEnabled());
  append_ticks(QwtPlot::yLeft, grid->yEnabled(), grid->yMinEnabled());
}

struct CurveReductionStats {
  std::size_t input_samples = 0;
  std::size_t retained_samples = 0;
  std::size_t input_runs = 0;
  std::size_t dropped_runs = 0;
};

void appendSymbol(GeometryBuilder& geometry, const QPointF& point, const QwtSymbol* symbol, const QColor& fallback) {
  if (symbol == nullptr || symbol->style() == QwtSymbol::NoSymbol) {
    return;
  }
  const QSize size = symbol->size();
  const QSizeF radii(size.width() * 0.5, size.height() * 0.5);
  if (symbol->style() == QwtSymbol::Ellipse) {
    const QBrush brush = symbol->brush();
    if (brush.style() != Qt::NoBrush) {
      geometry.addEllipse(point, radii, brush.color());
    }
    const QPen symbol_pen = symbol->pen();
    if (symbol_pen.style() != Qt::NoPen && symbol_pen.color().alpha() > 0) {
      const qreal half_pen_width = geometry.effectivePenWidth(symbol_pen.widthF()) * 0.5;
      const QSizeF outer_radii(radii.width() + half_pen_width, radii.height() + half_pen_width);
      const QSizeF inner_radii(radii.width() - half_pen_width, radii.height() - half_pen_width);
      if (inner_radii.width() > 0.0 && inner_radii.height() > 0.0) {
        geometry.addEllipseRing(point, outer_radii, inner_radii, symbol_pen.color());
      } else {
        geometry.addEllipse(point, outer_radii, symbol_pen.color());
      }
    }
    return;
  }

  // PJ4 production constructs ellipse symbols only. Keep the historical
  // rectangular fallback for an unknown external Qwt symbol, but do not imply
  // full generic Qwt path/pixmap/SVG-symbol support.
  const QColor color = symbol->brush().style() != Qt::NoBrush
                           ? symbol->brush().color()
                           : (symbol->pen().style() != Qt::NoPen ? symbol->pen().color() : fallback);
  geometry.addRect(QRectF(point - QPointF(radii.width(), radii.height()), QSizeF(size)), color);
}

[[nodiscard]] CurveReductionStats appendCurve(
    GeometryBuilder& geometry, const QwtPlot* plot, const QwtPlotCurve* curve, std::size_t sample_budget) {
  if (curve == nullptr || !curve->isVisible() || curve->data() == nullptr) {
    return {};
  }
  const std::size_t count = curve->dataSize();
  if (count == 0) {
    return {};
  }
  const QwtScaleMap x_map = plot->canvasMap(curve->xAxis());
  const QwtScaleMap y_map = plot->canvasMap(curve->yAxis());
  const auto transform = [&](const QPointF& point) {
    return QPointF(x_map.transform(point.x()), y_map.transform(point.y()));
  };
  const auto mapped_sample = [&](std::size_t index) {
    const QPointF sample = curve->sample(index);
    return finitePoint(sample)
               ? transform(sample)
               : QPointF(std::numeric_limits<qreal>::quiet_NaN(), std::numeric_limits<qreal>::quiet_NaN());
  };
  PlotSampleReduction reduction = reducePlotSamples(
      count, sample_budget, static_cast<std::size_t>(std::max(1, plot->canvas()->width())), mapped_sample);
  const QPen pen = curve->pen();
  const qreal width = geometry.effectivePenWidth(pen.widthF());
  const std::vector<qreal> dash = GeometryBuilder::dashPattern(pen.style(), width);
  const QwtPlotCurve::CurveStyle style = curve->style();
  if (!dash.empty() && (style == QwtPlotCurve::Lines || style == QwtPlotCurve::Steps)) {
    const PlotSegmentLength segment_length =
        style == QwtPlotCurve::Steps
            ? PlotSegmentLength([](const QPointF& from, const QPointF& to) {
                return std::abs(to.x() - from.x()) + std::abs(to.y() - from.y());
              })
            : PlotSegmentLength([](const QPointF& from, const QPointF& to) { return QLineF(from, to).length(); });
    annotatePlotSamplePathOffsets(reduction, mapped_sample, segment_length);
  }
  struct StrokePath {
    std::vector<QPointF> points;
    std::vector<qreal> phase_offsets;
  };
  std::vector<StrokePath> paths;
  StrokePath path;
  const auto append_path_point = [&path](const QPointF& point, qreal phase_offset) {
    if (path.points.empty() || QLineF(path.points.back(), point).length() > std::numeric_limits<qreal>::epsilon()) {
      path.points.push_back(point);
      path.phase_offsets.push_back(phase_offset);
    } else {
      path.phase_offsets.back() = phase_offset;
    }
  };
  const auto finish_path = [&]() {
    if (!path.points.empty()) {
      paths.push_back(std::move(path));
      path = {};
    }
  };
  for (const ReducedPlotSample& sample : reduction.samples) {
    if (sample.starts_new_run) {
      finish_path();
    }
    const QPointF point = sample.point;
    if (style == QwtPlotCurve::Dots) {
      geometry.addRect(QRectF(point - QPointF(width * 0.5, width * 0.5), QSizeF(width, width)), pen.color());
    } else if (style == QwtPlotCurve::Sticks) {
      QPointF baseline = point;
      if (curve->orientation() == Qt::Vertical) {
        baseline.setY(y_map.transform(curve->baseline()));
      } else {
        baseline.setX(x_map.transform(curve->baseline()));
      }
      geometry.addLine(baseline, point, width, pen.color(), pen.style(), dash);
    } else if (style == QwtPlotCurve::Lines) {
      append_path_point(point, sample.source_path_offset);
    } else if (style == QwtPlotCurve::Steps) {
      if (path.points.empty()) {
        append_path_point(point, sample.source_path_offset);
        continue;
      }
      QPointF corner;
      const bool inverted = (curve->orientation() == Qt::Vertical) != curve->testCurveAttribute(QwtPlotCurve::Inverted);
      if (inverted) {
        corner = QPointF(path.points.back().x(), point.y());
      } else {
        corner = QPointF(point.x(), path.points.back().y());
      }
      const qreal corner_offset = path.phase_offsets.back() + QLineF(path.points.back(), corner).length();
      append_path_point(corner, corner_offset);
      append_path_point(point, sample.source_path_offset);
    }
  }
  finish_path();
  for (const StrokePath& curve_path : paths) {
    geometry.addPolyline(curve_path.points, width, pen.color(), pen.style(), dash, curve_path.phase_offsets);
  }
  // QwtPlotCurve::drawSeries() draws the complete curve first and then all
  // symbols. A per-sample interleave lets a later line segment cover an earlier
  // symbol on self-intersections, which diverges from the desktop painter.
  if (const QwtSymbol* symbol = curve->symbol(); symbol != nullptr && symbol->style() != QwtSymbol::NoSymbol) {
    for (const ReducedPlotSample& sample : reduction.samples) {
      appendSymbol(geometry, sample.point, symbol, pen.color());
    }
  }
  return {
      .input_samples = reduction.input_samples,
      .retained_samples = reduction.samples.size(),
      .input_runs = reduction.input_runs,
      .dropped_runs = reduction.dropped_runs,
  };
}

void appendMarker(GeometryBuilder& geometry, const QwtPlot* plot, const QwtPlotMarker* marker) {
  if (marker == nullptr || !marker->isVisible()) {
    return;
  }
  const QwtScaleMap x_map = plot->canvasMap(marker->xAxis());
  const QwtScaleMap y_map = plot->canvasMap(marker->yAxis());
  const QPointF point(x_map.transform(marker->xValue()), y_map.transform(marker->yValue()));
  const QPen line_pen = marker->linePen();
  if (marker->lineStyle() == QwtPlotMarker::VLine || marker->lineStyle() == QwtPlotMarker::Cross) {
    geometry.addLine(
        QPointF(point.x(), 0.0), QPointF(point.x(), plot->canvas()->height()), line_pen.widthF(), line_pen.color(),
        line_pen.style());
  }
  if (marker->lineStyle() == QwtPlotMarker::HLine || marker->lineStyle() == QwtPlotMarker::Cross) {
    geometry.addLine(
        QPointF(0.0, point.y()), QPointF(plot->canvas()->width(), point.y()), line_pen.widthF(), line_pen.color(),
        line_pen.style());
  }
  if (const QwtSymbol* symbol = marker->symbol(); symbol != nullptr && symbol->style() != QwtSymbol::NoSymbol) {
    appendSymbol(geometry, point, symbol, line_pen.color());
  }
}

struct PlotGeometry {
  struct ItemSpan {
    const QwtPlotItem* item = nullptr;
    std::size_t first_vertex = 0;
    std::size_t vertex_count = 0;
  };

  std::vector<ItemSpan> item_spans;
  std::size_t curve_vertices = 0;
  std::size_t input_samples = 0;
  std::size_t retained_samples = 0;
  std::size_t input_runs = 0;
  std::size_t dropped_runs = 0;
  std::size_t budget_reduced_curves = 0;
  std::size_t budget_reduction_retries = 0;
  std::size_t budget_discarded_trial_vertices = 0;
  std::size_t budget_dropped_samples = 0;
  std::size_t budget_omitted_curves = 0;
  std::size_t budget_limited_curves = 0;
  bool truncated = false;
};

[[nodiscard]] PlotGeometry buildGeometry(
    std::vector<Vertex>& sink, const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio) {
  GeometryBuilder geometry(sink, logical_size, device_pixel_ratio);
  PlotGeometry geometry_stats;
  std::size_t curve_vertices = 0;
  const QwtPlotItemList items = plot->itemList();
  std::vector<const QwtPlotCurve*> visible_curves;
  for (const QwtPlotItem* item : items) {
    if (const auto* curve = dynamic_cast<const QwtPlotCurve*>(item); isRenderableCurve(curve)) {
      visible_curves.push_back(curve);
    }
  }
  const std::size_t curve_count = visible_curves.size();
  const std::size_t frame_sample_budget = static_cast<std::size_t>(std::max(2048, plot->canvas()->width() * 4));
  for (const QwtPlotItem* item : items) {
    if (const auto* grid = dynamic_cast<const QwtPlotGrid*>(item)) {
      const std::size_t before = geometry.vertexCount();
      appendGrid(geometry, plot, grid);
      if (geometry.vertexCount() > before) {
        geometry_stats.item_spans.push_back({item, before, geometry.vertexCount() - before});
      }
    }
  }
  for (std::size_t curve_index = 0; curve_index < curve_count; ++curve_index) {
    const QwtPlotCurve* curve = visible_curves[curve_index];
    if (curve != nullptr) {
      const std::size_t density_share =
          frame_sample_budget / curve_count + (curve_index < frame_sample_budget % curve_count ? 1 : 0);
      const std::size_t vertex_share =
          kPlannedCurveVertices / curve_count + (curve_index < kPlannedCurveVertices % curve_count ? 1 : 0);
      // Floor at 2 so a visible curve always keeps at least its run endpoints:
      // an unfloored share divides to 0 once curve_count exceeds the frame
      // budget (thousands of curves), which would drop the curve to nothing
      // with no on-screen signal. The frame vertex ceiling still bounds totals.
      const std::size_t sample_budget =
          std::max<std::size_t>(2, std::min(density_share, vertex_share / estimatedVerticesPerSample(curve)));
      const std::size_t before = geometry.vertexCount();
      const bool truncated_before = geometry.truncated();
      std::size_t attempt_budget = sample_budget;
      std::size_t initial_retained_samples = 0;
      CurveReductionStats reduction;
      bool committed = false;
      bool retried = false;
      while (!committed) {
        geometry.beginCurveBudget(vertex_share);
        const CurveReductionStats attempt = appendCurve(geometry, plot, curve, attempt_budget);
        const bool limited = geometry.endCurveBudget();
        if (!limited) {
          reduction = attempt;
          committed = true;
          break;
        }

        // Never keep a front-loaded prefix. Roll the incomplete curve back,
        // lower the reducer budget, and rebuild a complete representation.
        retried = true;
        if (initial_retained_samples == 0) {
          initial_retained_samples = attempt.retained_samples;
        }
        ++geometry_stats.budget_reduction_retries;
        geometry_stats.budget_discarded_trial_vertices += geometry.vertexCount() - before;
        geometry.rewind(before, truncated_before);
        if (attempt_budget <= 2 || attempt.retained_samples <= 2) {
          reduction = attempt;
          reduction.retained_samples = 0;
          reduction.dropped_runs = reduction.input_runs;
          ++geometry_stats.budget_limited_curves;
          ++geometry_stats.budget_omitted_curves;
          break;
        }
        attempt_budget = std::max<std::size_t>(
            2, std::min(attempt_budget - 1, std::max<std::size_t>(2, attempt.retained_samples / 2)));
      }
      if (retried) {
        ++geometry_stats.budget_reduced_curves;
        geometry_stats.budget_dropped_samples += initial_retained_samples > reduction.retained_samples
                                                     ? initial_retained_samples - reduction.retained_samples
                                                     : 0;
      }
      const std::size_t curve_vertex_count = geometry.vertexCount() - before;
      if (curve_vertex_count > 0) {
        geometry_stats.item_spans.push_back({curve, before, curve_vertex_count});
      }
      curve_vertices += curve_vertex_count;
      geometry_stats.input_samples += reduction.input_samples;
      geometry_stats.retained_samples += reduction.retained_samples;
      geometry_stats.input_runs += reduction.input_runs;
      geometry_stats.dropped_runs += reduction.dropped_runs;
    }
  }
  for (const QwtPlotItem* item : items) {
    if (const auto* marker = dynamic_cast<const QwtPlotMarker*>(item)) {
      const std::size_t before = geometry.vertexCount();
      appendMarker(geometry, plot, marker);
      if (geometry.vertexCount() > before) {
        geometry_stats.item_spans.push_back({item, before, geometry.vertexCount() - before});
      }
    }
  }
  geometry_stats.curve_vertices = curve_vertices;
  geometry_stats.truncated = geometry.truncated();
  return geometry_stats;
}

// Slim ENVIRONMENT key: the view state that changes the rendered image without
// a QwtPlot::replot() (and therefore without a geometry_dirty_ set). Semantic
// changes — data, pens, items, symbols, curve styles — invalidate geometry via
// the replot() slot instead, so they are deliberately NOT hashed here. The four
// axis bounds are belt-and-braces: zoomers mutate scale divisions in place, and
// some paths reach the canvas without a full replot.
[[nodiscard]] std::uint64_t computeEnvKey(
    const QwtPlot* plot, QSize logical_size, QSize target_size, const QColor& clear_color) {
  std::uint64_t key = 0xcbf29ce484222325ULL;
  hashUInt(key, static_cast<std::uint64_t>(logical_size.width()));
  hashUInt(key, static_cast<std::uint64_t>(logical_size.height()));
  hashUInt(key, static_cast<std::uint64_t>(target_size.width()));
  hashUInt(key, static_cast<std::uint64_t>(target_size.height()));
  hashUInt(key, static_cast<std::uint64_t>(clear_color.rgba()));
  for (QwtAxisId axis : {QwtPlot::xBottom, QwtPlot::yLeft, QwtPlot::yRight, QwtPlot::xTop}) {
    const QwtScaleDiv& division = plot->axisScaleDiv(axis);
    hashDouble(key, division.lowerBound());
    hashDouble(key, division.upperBound());
  }
  return key;
}

}  // namespace

PlotRhiCanvas::PlotRhiCanvas(QwtPlot* plot, QWidget* parent) : QRhiWidget(parent), plot_(plot) {
  pjPlotRhiInitResources();
  setApi(preferredGraphicsApi());
  setSampleCount(4);
  setContentsMargins(1, 1, 1, 1);
  setAutoFillBackground(false);
  setMouseTracking(true);
  connect(this, &QRhiWidget::renderFailed, this, &PlotRhiCanvas::showRenderFailure);
  connect(this, &QRhiWidget::frameSubmitted, this, [this]() {
    QWidget* top_level = window();
    // Qt/WASM switches a raster top-level to its RHI compositor when the first
    // real plot is shown. Repaint the whole window only after that first frame
    // has actually been submitted, so the raster backing texture is fully
    // initialized before later partial QWidget updates. The refresh is
    // per-top-level: a reparent/float to a new window re-arms it. Latch BEFORE
    // scheduling the update so the update it triggers cannot re-enter this arm.
    if (composition_refresh_queued_ && last_refreshed_window_ == top_level) {
      return;
    }
    composition_refresh_queued_ = true;
    last_refreshed_window_ = top_level;
    QTimer::singleShot(0, top_level, [top_level]() { top_level->update(); });
  });
}

PlotRhiCanvas::~PlotRhiCanvas() {
  releaseResources();
}

void PlotRhiCanvas::setPlot(QwtPlot* plot) {
  if (plot_ == plot) {
    return;
  }
  plot_ = plot;
  geometry_dirty_ = true;
  env_key_valid_ = false;
  content_reported_ = false;
  update();
}

QString PlotRhiCanvas::rendererBackend() const {
  return renderer_backend_;
}

std::size_t PlotRhiCanvas::lastVertexCount() const noexcept {
  return last_vertex_count_;
}

void PlotRhiCanvas::replot() {
  // Authoritative geometry-invalidation source: QwtPlot::replot() reaches here
  // reflectively on every semantic change (data, pens, items, axes). Mark dirty
  // so the next render() rebuilds instead of reusing the retained buffer.
  geometry_dirty_ = true;
  update();
}

void PlotRhiCanvas::clearTextLayers() {
  delete text_bindings_;
  text_bindings_ = nullptr;
  delete text_texture_;
  text_texture_ = nullptr;
  text_texture_capacity_ = {};
  text_vertices_.clear();
  draw_batches_.clear();
}

void PlotRhiCanvas::releaseResources() {
  clearTextLayers();
  text_raster_scratch_ = {};
  delete text_pipeline_;
  text_pipeline_ = nullptr;
  delete text_layout_bindings_;
  text_layout_bindings_ = nullptr;
  delete text_placeholder_texture_;
  text_placeholder_texture_ = nullptr;
  delete text_sampler_;
  text_sampler_ = nullptr;
  delete text_vertex_buffer_;
  text_vertex_buffer_ = nullptr;
  text_vertex_buffer_capacity_ = 0;
  delete pipeline_;
  pipeline_ = nullptr;
  delete shader_resources_;
  shader_resources_ = nullptr;
  delete vertex_buffer_;
  vertex_buffer_ = nullptr;
  vertex_buffer_capacity_ = 0;
  rhi_ = nullptr;
  // A fresh context has no uploaded geometry: start dirty and drop the env key
  // so the first frame after re-initialize() rebuilds and re-uploads.
  geometry_dirty_ = true;
  env_key_valid_ = false;
  ready_reported_ = false;
  content_reported_ = false;
  truncation_reported_ = false;
  text_truncation_reported_ = false;
  text_failure_reported_ = false;
  composition_refresh_queued_ = false;
  last_refreshed_window_.clear();
}

bool PlotRhiCanvas::createPipeline(QRhiCommandBuffer* /*command_buffer*/) {
  if (rhi_ == nullptr || renderTarget() == nullptr) {
    return false;
  }
  const QShader vertex_shader = loadShader(u":/pj_plotting/shaders/plot_color.vert.qsb"_s);
  const QShader fragment_shader = loadShader(u":/pj_plotting/shaders/plot_color.frag.qsb"_s);
  const QShader text_vertex_shader = loadShader(u":/pj_plotting/shaders/plot_text.vert.qsb"_s);
  const QShader text_fragment_shader = loadShader(u":/pj_plotting/shaders/plot_text.frag.qsb"_s);
  if (!vertex_shader.isValid() || !fragment_shader.isValid() || !text_vertex_shader.isValid() ||
      !text_fragment_shader.isValid()) {
    qCritical("PlotRhiCanvas: failed to load plot shaders");
    return false;
  }

  auto shader_resources = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
  shader_resources->setBindings({});
  if (!shader_resources->create()) {
    return false;
  }

  constexpr qsizetype kInitialVertexBufferCapacity = 64 * 1024;
  auto vertex_buffer = std::unique_ptr<QRhiBuffer>(
      rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, kInitialVertexBufferCapacity));
  if (!vertex_buffer->create()) {
    return false;
  }

  QRhiVertexInputLayout input_layout;
  input_layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
  input_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, offsetof(Vertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4, offsetof(Vertex, red)),
  });
  auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
  pipeline->setShaderStages({
      {QRhiShaderStage::Vertex, vertex_shader},
      {QRhiShaderStage::Fragment, fragment_shader},
  });
  pipeline->setVertexInputLayout(input_layout);
  pipeline->setShaderResourceBindings(shader_resources.get());
  pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  pipeline->setCullMode(QRhiGraphicsPipeline::None);
  pipeline->setSampleCount(sampleCount());
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  pipeline->setTargetBlends({blend});
  pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!pipeline->create()) {
    return false;
  }

  auto text_sampler = std::unique_ptr<QRhiSampler>(rhi_->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  if (!text_sampler->create()) {
    return false;
  }
  auto text_placeholder_texture = std::unique_ptr<QRhiTexture>(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
  if (!text_placeholder_texture->create()) {
    return false;
  }
  auto text_layout_bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
  text_layout_bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
      0, QRhiShaderResourceBinding::FragmentStage, text_placeholder_texture.get(), text_sampler.get())});
  if (!text_layout_bindings->create()) {
    return false;
  }
  constexpr qsizetype kInitialTextVertexBufferCapacity = 16 * 1024;
  auto text_vertex_buffer = std::unique_ptr<QRhiBuffer>(
      rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, kInitialTextVertexBufferCapacity));
  if (!text_vertex_buffer->create()) {
    return false;
  }
  QRhiVertexInputLayout text_input_layout;
  text_input_layout.setBindings({QRhiVertexInputBinding(sizeof(TextVertex))});
  text_input_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, offsetof(TextVertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2, offsetof(TextVertex, u)),
  });
  auto text_pipeline = std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
  text_pipeline->setShaderStages({
      {QRhiShaderStage::Vertex, text_vertex_shader},
      {QRhiShaderStage::Fragment, text_fragment_shader},
  });
  text_pipeline->setVertexInputLayout(text_input_layout);
  text_pipeline->setShaderResourceBindings(text_layout_bindings.get());
  text_pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
  text_pipeline->setCullMode(QRhiGraphicsPipeline::None);
  text_pipeline->setSampleCount(sampleCount());
  QRhiGraphicsPipeline::TargetBlend text_blend;
  text_blend.enable = true;
  // QPainter produces premultiplied RGBA rasters.
  text_blend.srcColor = QRhiGraphicsPipeline::One;
  text_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  text_blend.srcAlpha = QRhiGraphicsPipeline::One;
  text_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  text_pipeline->setTargetBlends({text_blend});
  text_pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
  if (!text_pipeline->create()) {
    return false;
  }

  shader_resources_ = shader_resources.release();
  vertex_buffer_ = vertex_buffer.release();
  vertex_buffer_capacity_ = kInitialVertexBufferCapacity;
  pipeline_ = pipeline.release();
  text_sampler_ = text_sampler.release();
  text_placeholder_texture_ = text_placeholder_texture.release();
  text_layout_bindings_ = text_layout_bindings.release();
  text_vertex_buffer_ = text_vertex_buffer.release();
  text_vertex_buffer_capacity_ = kInitialTextVertexBufferCapacity;
  text_pipeline_ = text_pipeline.release();
  return true;
}

void PlotRhiCanvas::initialize(QRhiCommandBuffer* command_buffer) {
  if (rhi_ != rhi()) {
    releaseResources();
    rhi_ = rhi();
  }
  if (rhi_ == nullptr) {
    showRenderFailure();
    return;
  }
  renderer_backend_ = backendName(rhi_->backend());
  if ((pipeline_ == nullptr || text_pipeline_ == nullptr) && !createPipeline(command_buffer)) {
    showRenderFailure();
    return;
  }
  if (failure_label_ != nullptr) {
    failure_label_->hide();
  }
  failure_reported_ = false;
  if (!ready_reported_) {
    ready_reported_ = true;
    const QRhiDriverInfo driver = rhi_->driverInfo();
    qInfo(
        "PJ_WASM_PLOT_RHI_READY backend=%s device=%s samples=%d", qPrintable(renderer_backend_),
        driver.deviceName.constData(), sampleCount());
  }
}

void PlotRhiCanvas::render(QRhiCommandBuffer* command_buffer) {
  QwtPlot* plot = plot_;
  const QColor clear_color = plot != nullptr ? plot->canvasBackground().color() : QColor(Qt::transparent);
  bool force_pipeline_failure = false;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  force_pipeline_failure = property("pjWasmForcePipelineFailure").toBool();
#endif
  // Real GPU errors (no device, pipeline, or render target): show the failure
  // overlay + emit the PJ_WASM_PLOT_RHI_FAILED telemetry.
  if (force_pipeline_failure || rhi_ == nullptr || pipeline_ == nullptr || text_pipeline_ == nullptr ||
      renderTarget() == nullptr) {
    if (renderTarget() != nullptr) {
      command_buffer->beginPass(renderTarget(), clear_color, {1.0F, 0}, nullptr);
      command_buffer->endPass();
    }
    showRenderFailure();
    return;
  }
  if (plot == nullptr) {
    // A null plot is a transient wiring state (setPlot() runs right after
    // construction), not a GPU failure. Record a clear-only pass so the frame is
    // well-defined, without the failure banner/telemetry. Stay dirty so the
    // first frame after wiring rebuilds geometry.
    command_buffer->beginPass(renderTarget(), clear_color, {1.0F, 0}, nullptr);
    command_buffer->endPass();
    return;
  }
  const QSize target_size = renderTarget()->pixelSize();
  const qreal device_pixel_ratio = renderTargetScale(size(), target_size);
  // Qwt owns the canvas brush. QRhiWidget does not participate in Qwt's
  // QPainter background path, so read the brush back from the plot instead of
  // assuming the platform's accelerated-surface palette.
  const auto fail_rebuild = [this, command_buffer, &clear_color]() {
    // A failed allocation/upload must not leave an old text-bearing frame on
    // screen or hand QRhi an unrecorded render callback. Submit a defined clear
    // and keep the state dirty so a later frame can retry from live Qwt state.
    command_buffer->beginPass(renderTarget(), clear_color, {1.0F, 0}, nullptr);
    command_buffer->endPass();
    env_key_valid_ = false;
    showRenderFailure();
  };

  // Rebuild geometry when a semantic change flagged it dirty (via replot()) or a
  // view change moved the env key; otherwise re-draw the retained buffer without
  // rebuilding or re-uploading (PR #259 pattern). Compositor refreshes and expose
  // repaints hit neither branch and reuse the buffer — that is the whole point.
  const std::uint64_t env_key = computeEnvKey(plot, size(), target_size, clear_color);
  const bool rebuild = geometry_dirty_ || !env_key_valid_ || env_key != env_key_;

  qsizetype bytes = 0;
  qsizetype text_bytes = 0;
  QImage text_upload;
  bool retry_text_rebuild = false;
  if (rebuild) {
    vertices_.clear();
    vertices_.reserve(last_vertex_count_);
    const PlotGeometry geometry = buildGeometry(vertices_, plot, size(), device_pixel_ratio);
    last_vertex_count_ = vertices_.size();
    bytes = static_cast<qsizetype>(vertices_.size() * sizeof(Vertex));

    TextRasterBuild text_raster = buildTextRasters(plot, size(), device_pixel_ratio, text_raster_scratch_);
    text_vertices_.clear();
    std::vector<PlotGeometry::ItemSpan> text_spans;
    bool text_failed = false;
    QString text_failure_reason;
    const auto drop_text_for_frame = [&](const QString& reason) {
      if (!text_failed && !text_failure_reported_) {
        // Retry one later frame for a transient allocation failure. A repeated
        // failure remains geometry-only until another dirty source (semantic,
        // view, rebind, or lifecycle) instead of entering an unbounded loop.
        retry_text_rebuild = true;
      }
      text_failed = true;
      text_failure_reason = reason;
      text_raster.layers.clear();
      text_raster.truncated = true;
      refreshTextRasterStats(text_raster);
      text_vertices_.clear();
      text_spans.clear();
      text_bytes = 0;
      text_upload = {};
    };
    const int max_texture_size = rhi_->resourceLimit(QRhi::TextureSizeMax);
    bool force_text_failure = false;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    force_text_failure = property("pjWasmForceTextAtlasFailure").toBool();
    if (force_text_failure) {
      const int attempts = property("pjWasmTextFailureAttempts").toInt() + 1;
      setProperty("pjWasmTextFailureAttempts", attempts);
      qInfo("PJ_WASM_TEXT_FAILURE_ATTEMPT count=%d", attempts);
    }
#endif
    if (force_text_failure) {
      drop_text_for_frame(u"test-injected text atlas failure"_s);
    } else if (text_raster.allocation_failed || !packTextAtlas(text_raster, max_texture_size, text_upload)) {
      drop_text_for_frame(u"failed to allocate or pack the text atlas"_s);
    }
    if (!text_failed && !text_raster.layers.empty()) {
      const QSize required = text_upload.size();
      if (text_texture_ == nullptr || required.width() > text_texture_capacity_.width() ||
          required.height() > text_texture_capacity_.height()) {
        const QSize grown(
            std::min(max_texture_size, std::max(required.width(), std::max(64, text_texture_capacity_.width() * 2))),
            std::min(max_texture_size, std::max(required.height(), std::max(64, text_texture_capacity_.height() * 2))));
        if (grown.width() < required.width() || grown.height() < required.height()) {
          drop_text_for_frame(tr("text atlas %1x%2 exceeds backend texture limit %3")
                                  .arg(required.width())
                                  .arg(required.height())
                                  .arg(max_texture_size));
        }
        if (!text_failed) {
          auto replacement_texture = std::unique_ptr<QRhiTexture>(rhi_->newTexture(QRhiTexture::RGBA8, grown));
          if (!replacement_texture->create()) {
            drop_text_for_frame(u"failed to allocate the text atlas texture"_s);
          }
          if (!text_failed) {
            auto replacement_bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
            replacement_bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
                0, QRhiShaderResourceBinding::FragmentStage, replacement_texture.get(), text_sampler_)});
            if (!replacement_bindings->create()) {
              drop_text_for_frame(u"failed to bind the text atlas texture"_s);
            } else {
              delete text_bindings_;
              delete text_texture_;
              text_texture_ = replacement_texture.release();
              text_bindings_ = replacement_bindings.release();
              text_texture_capacity_ = grown;
            }
          }
        }
      }

      if (!text_failed) {
        for (const TextRasterLayer& layer : text_raster.layers) {
          const QRectF uv_rect(
              static_cast<qreal>(layer.atlas_position.x()) / text_texture_capacity_.width(),
              static_cast<qreal>(layer.atlas_position.y()) / text_texture_capacity_.height(),
              static_cast<qreal>(layer.image.width()) / text_texture_capacity_.width(),
              static_cast<qreal>(layer.image.height()) / text_texture_capacity_.height());
          const std::size_t before = text_vertices_.size();
          appendTextQuad(text_vertices_, layer.logical_rect, uv_rect, size());
          text_spans.push_back({layer.source_item, before, text_vertices_.size() - before});
        }
        text_bytes = static_cast<qsizetype>(text_vertices_.size() * sizeof(TextVertex));
      }

      if (!text_failed && text_bytes > text_vertex_buffer_capacity_) {
        const qsizetype new_capacity = std::max(text_bytes, text_vertex_buffer_capacity_ * 2);
        auto replacement =
            std::unique_ptr<QRhiBuffer>(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, new_capacity));
        if (!replacement->create()) {
          drop_text_for_frame(u"failed to allocate the text vertex buffer"_s);
        } else {
          delete text_vertex_buffer_;
          text_vertex_buffer_ = replacement.release();
          text_vertex_buffer_capacity_ = new_capacity;
        }
      }
      if (!text_failed) {
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
        qreal raster_scale_x = std::numeric_limits<qreal>::max();
        qreal raster_scale_y = std::numeric_limits<qreal>::max();
        for (const TextRasterLayer& layer : text_raster.layers) {
          raster_scale_x = std::min(raster_scale_x, layer.image.width() / layer.logical_rect.width());
          raster_scale_y = std::min(raster_scale_y, layer.image.height() / layer.logical_rect.height());
        }
        qInfo(
            "PJ_WASM_PLOT_TEXT_OK legend_layers=%lld marker_labels=%lld alpha_pixels=%lld vertices=%llu atlas=%dx%d "
            "rect=%.3f,%.3f,%.3fx%.3f raster_scale=%.3f,%.3f",
            static_cast<long long>(text_raster.legend_layers), static_cast<long long>(text_raster.marker_layers),
            static_cast<long long>(text_raster.alpha_pixels), static_cast<unsigned long long>(text_vertices_.size()),
            text_upload.width(), text_upload.height(), text_raster.logical_bounds.x(), text_raster.logical_bounds.y(),
            text_raster.logical_bounds.width(), text_raster.logical_bounds.height(), raster_scale_x, raster_scale_y);
#endif
      }
    }
    if (text_failed) {
      if (!text_failure_reported_) {
        text_failure_reported_ = true;
        qCWarning(lcPlotRhi) << "PlotRhiCanvas:" << text_failure_reason << "; rendering geometry without text";
      }
    } else {
      text_failure_reported_ = false;
      if (text_raster.truncated) {
        if (!text_truncation_reported_) {
          text_truncation_reported_ = true;
          qCWarning(lcPlotRhi) << "PlotRhiCanvas: text layer/atlas ceiling reached; later text items were omitted";
        }
      } else {
        text_truncation_reported_ = false;
      }
    }

    // Warn once per limit onset (not per frame) when the frame-wide ceiling or
    // an exact per-curve share drops geometry; clear the latch on a
    // within-budget frame so a later onset re-warns.
    if (geometry.truncated || geometry.budget_limited_curves > 0 || geometry.budget_reduced_curves > 0) {
      if (!truncation_reported_) {
        truncation_reported_ = true;
        if (geometry.truncated) {
          qCWarning(lcPlotRhi) << "PlotRhiCanvas: geometry vertex ceiling"
                               << static_cast<qulonglong>(kMaxGeometryVertices) << "reached; plot geometry truncated";
        } else if (geometry.budget_limited_curves > 0) {
          qCWarning(lcPlotRhi) << "PlotRhiCanvas: exact per-curve vertex shares limited"
                               << static_cast<qulonglong>(geometry.budget_limited_curves)
                               << "curve(s) even at endpoint density; incomplete prefixes were omitted";
        } else {
          qCWarning(lcPlotRhi) << "PlotRhiCanvas: reducer budgets lowered for"
                               << static_cast<qulonglong>(geometry.budget_reduced_curves)
                               << "curve(s) to avoid partial dash geometry; discarded trial vertices="
                               << static_cast<qulonglong>(geometry.budget_discarded_trial_vertices);
        }
      }
    } else {
      truncation_reported_ = false;
    }

    // Qwt paints items in increasing Z and stable attachment order. Geometry
    // and text live in separate GPU buffers, so retain an explicit batch list
    // instead of flattening every label above every marker. This matters when
    // the playback label, hover marker, and XY ride-along markers overlap.
    draw_batches_.clear();
    const auto append_batch = [this](bool text, const PlotGeometry::ItemSpan& span) {
      if (span.vertex_count == 0) {
        return;
      }
      const auto first = static_cast<std::uint32_t>(span.first_vertex);
      const auto count = static_cast<std::uint32_t>(span.vertex_count);
      if (!draw_batches_.empty()) {
        DrawBatch& previous = draw_batches_.back();
        if (previous.text == text && previous.first_vertex + previous.vertex_count == first) {
          previous.vertex_count += count;
          return;
        }
      }
      draw_batches_.push_back({.text = text, .first_vertex = first, .vertex_count = count});
    };
    for (const QwtPlotItem* item : plot->itemList()) {
      const auto geometry_span = std::find_if(
          geometry.item_spans.cbegin(), geometry.item_spans.cend(),
          [item](const PlotGeometry::ItemSpan& span) { return span.item == item; });
      if (geometry_span != geometry.item_spans.cend()) {
        append_batch(false, *geometry_span);
      }
      const auto text_span = std::find_if(
          text_spans.cbegin(), text_spans.cend(),
          [item](const PlotGeometry::ItemSpan& span) { return span.item == item; });
      if (text_span != text_spans.cend()) {
        append_batch(true, *text_span);
      }
    }
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    QByteArray batch_sequence;
    batch_sequence.reserve(static_cast<qsizetype>(draw_batches_.size()));
    for (const DrawBatch& batch : draw_batches_) {
      batch_sequence.append(batch.text ? 'T' : 'G');
    }
    qInfo(
        "PJ_WASM_PLOT_DRAW_BATCHES count=%lld sequence=%s", static_cast<long long>(draw_batches_.size()),
        batch_sequence.constData());
#endif

    // Content breadcrumb: emit on the first curve-bearing frame and again
    // whenever the plotted content changes. Format is fixed (Playwright greps).
    const QwtPlotItemList curves = plot->itemList(QwtPlotItem::Rtti_PlotCurve);
    const auto* first_curve = curves.empty() ? nullptr : dynamic_cast<const QwtPlotCurve*>(curves.front());
    if (first_curve != nullptr && first_curve->dataSize() > 0) {
      const QPointF first = first_curve->sample(0);
      const QPointF last = first_curve->sample(first_curve->dataSize() - 1);
      std::uint64_t fingerprint = 0xcbf29ce484222325ULL;
      hashUInt(fingerprint, static_cast<std::uint64_t>(first_curve->dataSize()));
      hashDouble(fingerprint, first.x());
      hashDouble(fingerprint, first.y());
      hashDouble(fingerprint, last.x());
      hashDouble(fingerprint, last.y());
      hashUInt(fingerprint, static_cast<std::uint64_t>(first_curve->pen().style()));
      hashDouble(fingerprint, first_curve->pen().widthF());
      hashUInt(fingerprint, static_cast<std::uint64_t>(geometry.curve_vertices));
      hashUInt(fingerprint, geometry.retained_samples);
      hashUInt(fingerprint, geometry.input_runs);
      hashUInt(fingerprint, geometry.dropped_runs);
      hashUInt(fingerprint, geometry.budget_reduced_curves);
      hashUInt(fingerprint, geometry.budget_reduction_retries);
      hashUInt(fingerprint, geometry.budget_discarded_trial_vertices);
      hashUInt(fingerprint, geometry.budget_dropped_samples);
      hashUInt(fingerprint, geometry.budget_omitted_curves);
      hashUInt(fingerprint, geometry.budget_limited_curves);
      hashUInt(fingerprint, geometry.truncated ? 1U : 0U);
      if (!content_reported_ || fingerprint != content_fingerprint_) {
        content_reported_ = true;
        content_fingerprint_ = fingerprint;
        qInfo(
            "PJ_WASM_PLOT_FRAME_OK curves=%d vertices=%llu curve_vertices=%llu first_curve=%s samples=%llu "
            "reduced_samples=%llu runs=%llu dropped_runs=%llu budget_limited_curves=%llu "
            "budget_reduced_curves=%llu budget_reduction_retries=%llu budget_discarded_trial_vertices=%llu "
            "budget_dropped_samples=%llu budget_omitted_curves=%llu "
            "endpoints=%.9g,%.9g..%.9g,%.9g logical=%dx%d target=%dx%d clear=%s pen=%s truncated=%d",
            curves.size(), static_cast<unsigned long long>(vertices_.size()),
            static_cast<unsigned long long>(geometry.curve_vertices), qPrintable(first_curve->title().text()),
            static_cast<unsigned long long>(first_curve->dataSize()),
            static_cast<unsigned long long>(geometry.retained_samples),
            static_cast<unsigned long long>(geometry.input_runs),
            static_cast<unsigned long long>(geometry.dropped_runs),
            static_cast<unsigned long long>(geometry.budget_limited_curves),
            static_cast<unsigned long long>(geometry.budget_reduced_curves),
            static_cast<unsigned long long>(geometry.budget_reduction_retries),
            static_cast<unsigned long long>(geometry.budget_discarded_trial_vertices),
            static_cast<unsigned long long>(geometry.budget_dropped_samples),
            static_cast<unsigned long long>(geometry.budget_omitted_curves), first.x(), first.y(), last.x(), last.y(),
            width(), height(), target_size.width(), target_size.height(), qPrintable(clear_color.name(QColor::HexArgb)),
            qPrintable(first_curve->pen().color().name(QColor::HexArgb)), geometry.truncated ? 1 : 0);
      }
    }

    if (bytes > vertex_buffer_capacity_) {
      // Never grow past the frame-wide ceiling: the builder already caps the
      // vertex count there, so this bound is exact, not just defensive.
      const qsizetype ceiling_bytes = static_cast<qsizetype>(kMaxGeometryVertices * sizeof(Vertex));
      const qsizetype new_capacity = std::min(ceiling_bytes, std::max(bytes, vertex_buffer_capacity_ * 2));
      auto replacement =
          std::unique_ptr<QRhiBuffer>(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, new_capacity));
      if (!replacement->create()) {
        fail_rebuild();
        return;
      }
      delete vertex_buffer_;
      vertex_buffer_ = replacement.release();
      vertex_buffer_capacity_ = new_capacity;
    }
    env_key_ = env_key;
    env_key_valid_ = true;
  }

  QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
  if (rebuild && bytes > 0) {
    updates->updateDynamicBuffer(vertex_buffer_, 0, bytes, vertices_.data());
  }
  if (rebuild && text_bytes > 0 && text_texture_ != nullptr && !text_upload.isNull()) {
    QRhiTextureSubresourceUploadDescription upload(text_upload);
    upload.setSourceSize(text_upload.size());
    updates->uploadTexture(text_texture_, QRhiTextureUploadDescription({0, 0, upload}));
    updates->updateDynamicBuffer(text_vertex_buffer_, 0, text_bytes, text_vertices_.data());
  }
  command_buffer->beginPass(renderTarget(), clear_color, {1.0F, 0}, updates);
  command_buffer->setViewport(
      QRhiViewport(0.0F, 0.0F, static_cast<float>(target_size.width()), static_cast<float>(target_size.height())));
  for (const DrawBatch& batch : draw_batches_) {
    if (batch.text) {
      if (text_pipeline_ == nullptr || text_bindings_ == nullptr || text_vertex_buffer_ == nullptr) {
        continue;
      }
      command_buffer->setGraphicsPipeline(text_pipeline_);
      command_buffer->setShaderResources(text_bindings_);
      const QRhiCommandBuffer::VertexInput binding(text_vertex_buffer_, 0);
      command_buffer->setVertexInput(0, 1, &binding);
    } else {
      command_buffer->setGraphicsPipeline(pipeline_);
      command_buffer->setShaderResources(shader_resources_);
      const QRhiCommandBuffer::VertexInput binding(vertex_buffer_, 0);
      command_buffer->setVertexInput(0, 1, &binding);
    }
    command_buffer->draw(batch.vertex_count, 1, batch.first_vertex);
  }
  command_buffer->endPass();
  // Allocation failures are retryable. If the retry succeeds without a QRhi
  // re-initialize() cycle, remove the overlay here rather than leaving a healthy
  // renderer hidden underneath a stale diagnostic.
  if (failure_label_ != nullptr) {
    failure_label_->hide();
  }
  failure_reported_ = false;
  // Geometry is now uploaded and drawn. A text-only allocation failure gets one
  // delayed retry while the usable geometry remains visible; repeated failures
  // wait for another semantic/view/rebind/lifecycle dirty source instead of
  // rebuilding every frame.
  geometry_dirty_ = retry_text_rebuild;
  if (retry_text_rebuild) {
    QTimer::singleShot(100, this, [this]() {
      if (geometry_dirty_) {
        update();
      }
    });
  }
}

void PlotRhiCanvas::showRenderFailure() {
  if (failure_label_ == nullptr) {
    failure_label_ = new QLabel(tr("GPU plot renderer unavailable"), this);
    failure_label_->setAlignment(Qt::AlignCenter);
    failure_label_->setStyleSheet(u"background:#5b1b1b;color:white;padding:8px;"_s);
  }
  failure_label_->setGeometry(rect());
  failure_label_->show();
  failure_label_->raise();
  if (!failure_reported_) {
    failure_reported_ = true;
    qCritical("PJ_WASM_PLOT_RHI_FAILED");
  }
}

}  // namespace PJ
