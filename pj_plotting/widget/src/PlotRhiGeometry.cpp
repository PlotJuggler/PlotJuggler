// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PlotRhiGeometry.h"

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
#include <qwt_transform.h>

#include <QLineF>
#include <QPen>
#include <QRectF>
#include <QString>
#include <QTransform>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <span>
#include <typeinfo>
#include <utility>

#include "pj_plotting/PlotCurve.h"
#include "pj_plotting/PlotSampleReducer.h"
#include "pj_plotting/RevisionedSeries.h"

using namespace Qt::StringLiterals;

namespace PJ::plot_rhi {
namespace {

constexpr int kSymbolSegments = 12;
constexpr qreal kTau = 6.28318530717958647692;
constexpr qreal kSqrtHalf = 0.70710678118654752440;

// Both coordinates finite (no NaN gap, no infinity).
[[nodiscard]] bool finitePoint(const QPointF& point) {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

// Logs each distinct unsupported feature once per process: the canvas keeps
// drawing an approximation, so the log is the only signal.
void warnUnsupportedOnce(const QString& what) {
  static std::set<QString> reported;
  if (reported.insert(what).second) {
    qCWarning(lcPlotRhi).noquote() << "PlotRhiCanvas: unsupported" << what;
  }
}

// Appends indexed triangles (in logical widget pixels, converted to clip space
// on push) to caller-owned Vertex and quint32 index arrays in
// QByteArrays, whose allocations are reused across frames; the destructor trims
// them to what was written. Primitives share their corner vertices: a solid
// polyline is one indexed strip, joins included.
//
// Every primitive carries plot_color.frag's coverage attributes: strokes and
// fills reach half a device pixel past their true edges, where the shader ramps
// coverage to zero over one device pixel. A stroke narrower than one device
// pixel is drawn one pixel wide at proportionally lower alpha, as Qt's
// antialiased raster engine draws it. Dashes are cut in the shader from the
// path distance, so a dashed segment is one quad however many dashes it holds.
class GeometryBuilder {
 public:
  GeometryBuilder(
      QByteArray& sink, QByteArray& index_sink, QSize logical_size, qreal device_pixel_ratio, const QTransform& to_clip)
      : sink_(sink),
        index_sink_(index_sink),
        logical_size_(logical_size),
        clip_xx_(to_clip.m11()),
        clip_xy_(to_clip.m21()),
        clip_dx_(to_clip.dx()),
        clip_yx_(to_clip.m12()),
        clip_yy_(to_clip.m22()),
        clip_dy_(to_clip.dy()),
        dpr_(std::max<qreal>(std::numeric_limits<qreal>::epsilon(), device_pixel_ratio)),
        fringe_(0.5 / dpr_) {
    sink_.resize(std::max<qsizetype>(sink_.capacity(), 4096 * static_cast<qsizetype>(sizeof(Vertex))));
    capacity_ = static_cast<std::size_t>(sink_.size()) / sizeof(Vertex);
    data_ = sink_.data();
    index_sink_.resize(std::max<qsizetype>(index_sink_.capacity(), 8192 * static_cast<qsizetype>(sizeof(quint32))));
    index_capacity_ = static_cast<std::size_t>(index_sink_.size()) / sizeof(quint32);
    indices_ = reinterpret_cast<quint32*>(index_sink_.data());
  }

  ~GeometryBuilder() {
    sink_.resize(static_cast<qsizetype>(count_ * sizeof(Vertex)));
    index_sink_.resize(static_cast<qsizetype>(index_count_ * sizeof(quint32)));
  }

  GeometryBuilder(const GeometryBuilder&) = delete;
  GeometryBuilder& operator=(const GeometryBuilder&) = delete;

  // A zero width is Qt's cosmetic pen: one device pixel.
  [[nodiscard]] qreal effectivePenWidth(qreal width) const noexcept {
    return width > 0.0 ? width : 1.0 / dpr_;
  }

  [[nodiscard]] std::size_t vertexCount() const noexcept {
    return count_;
  }

  // Item spans are index ranges.
  [[nodiscard]] std::size_t indexCount() const noexcept {
    return index_count_;
  }

  // True once the frame-wide vertex ceiling (kMaxFrameVertices) was hit and
  // further geometry was dropped. render() surfaces this as a throttled warning.
  [[nodiscard]] bool truncated() const noexcept {
    return truncated_;
  }

  // Give one curve an exact share of the frame budget. Hitting this sub-limit
  // stops only that curve; endCurveBudget() re-arms the builder for the next
  // curve, preventing an early pathological curve from starving every later item.
  void beginCurveBudget(std::size_t vertex_budget) {
    active_vertex_limit_ = std::min(kMaxGeometryVertices, count_ + vertex_budget);
    curve_limit_hit_ = false;
  }

  [[nodiscard]] bool endCurveBudget() {
    const bool limited = curve_limit_hit_;
    active_vertex_limit_ = kMaxGeometryVertices;
    curve_limit_hit_ = false;
    return limited;
  }

  void rewind(std::size_t vertex_count, std::size_t index_count, bool truncated) {
    count_ = std::min(vertex_count, count_);
    index_count_ = std::min(index_count, index_count_);
    active_vertex_limit_ = kMaxGeometryVertices;
    curve_limit_hit_ = false;
    truncated_ = truncated;
  }

  // One-off line (grid, marker, stick, symbol stroke). A horizontal or vertical
  // one is placed on whole pixels first (snapToPixels()), as Qwt does.
  void addLine(QPointF from, QPointF to, qreal width, const QColor& color, Qt::PenStyle style = Qt::SolidLine) {
    if (!finitePoint(from) || !finitePoint(to) || style == Qt::NoPen || !color.isValid() || color.alpha() == 0) {
      return;
    }
    if (from.x() == to.x()) {
      from.setX(snapToPixels(from.x(), width));
      to.setX(from.x());
    } else if (from.y() == to.y()) {
      from.setY(snapToPixels(from.y(), width));
      to.setY(from.y());
    }
    if (QLineF(from, to).length() <= std::numeric_limits<qreal>::epsilon()) {
      const qreal point_width = effectivePenWidth(width);
      addRect(QRectF(from - QPointF(point_width * 0.5, point_width * 0.5), QSizeF(point_width, point_width)), color);
      return;
    }
    line_points_.assign({from, to});
    addPolyline(line_points_, width, color, style);
  }

  // `phase_offsets`, when given (one per point), is each point's distance along
  // the source path in logical pixels: a reduced curve's dashes keep the phase
  // of the samples the reducer skipped.
  void addPolyline(
      const std::vector<QPointF>& points, qreal width, const QColor& color, Qt::PenStyle style,
      const std::vector<qreal>& phase_offsets = {}) {
    if (curve_limit_hit_ || points.size() < 2 || style == Qt::NoPen || !color.isValid() || color.alpha() == 0 ||
        std::any_of(points.cbegin(), points.cend(), [](const QPointF& point) { return !finitePoint(point); })) {
      return;
    }
    const std::vector<QPointF>* stroke_points = &points;
    const std::vector<qreal>* stroke_offsets = phase_offsets.size() == points.size() ? &phase_offsets : nullptr;
    std::vector<QPointF>& compact_points = compact_points_;
    std::vector<qreal>& compact_offsets = compact_offsets_;
    compact_points.clear();
    compact_offsets.clear();
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
    // QCosmeticStroker applies a cosmetic pen's pattern per pixel with no caps;
    // a wide pen's square caps lengthen every dash by half the width.
    const bool cosmetic = width <= 0.0;
    width = effectivePenWidth(width);
    QColor stroke_color = color;
    if (width * dpr_ < 1.0) {
      stroke_color.setAlphaF(static_cast<float>(color.alphaF() * width * dpr_));
      width = 1.0 / dpr_;
    }
    setColor(stroke_color);
    if (dashPattern(style).empty()) {
      addSolidPolyline(*stroke_points, width);
    } else {
      addDashedPolyline(*stroke_points, stroke_offsets, width, style, !cosmetic);
    }
  }

  void addEllipse(const QPointF& center, const QSizeF& radii, const QColor& color) {
    if (curve_limit_hit_ || !finitePoint(center) || radii.width() <= 0.0 || radii.height() <= 0.0) {
      return;
    }
    ellipse_ring_.resize(kSymbolSegments);
    for (int index = 0; index < kSymbolSegments; ++index) {
      const qreal angle = kTau * static_cast<qreal>(index) / kSymbolSegments;
      ellipse_ring_[static_cast<std::size_t>(index)] =
          QPointF(center.x() + std::cos(angle) * radii.width(), center.y() + std::sin(angle) * radii.height());
    }
    addFan(center, ellipse_ring_, color);
  }

  // A stroked ellipse outline between two radii, antialiased on both rims.
  void addEllipseRing(
      const QPointF& center, const QSizeF& outer_radii, const QSizeF& inner_radii, const QColor& color) {
    if (curve_limit_hit_ || !finitePoint(center) || outer_radii.width() <= 0.0 || outer_radii.height() <= 0.0 ||
        inner_radii.width() <= 0.0 || inner_radii.height() <= 0.0 || inner_radii.width() >= outer_radii.width() ||
        inner_radii.height() >= outer_radii.height() || !color.isValid() || color.alpha() == 0) {
      return;
    }
    if (inner_radii.width() <= fringe_ || inner_radii.height() <= fringe_) {
      addEllipse(center, outer_radii, color);  // no hole left once the inner rim is antialiased
      return;
    }
    const QSizeF outer(outer_radii.width() + fringe_, outer_radii.height() + fringe_);
    const QSizeF inner(inner_radii.width() - fringe_, inner_radii.height() - fringe_);
    const auto at = [&center](const QSizeF& radii, int index) {
      const qreal angle = kTau * static_cast<qreal>(index) / kSymbolSegments;
      return QPointF(center.x() + std::cos(angle) * radii.width(), center.y() + std::sin(angle) * radii.height());
    };
    if (!reserve(2 * kSymbolSegments, 6 * kSymbolSegments)) {
      return;
    }
    setColor(color);
    code_ = kSolidCode;
    const quint32 first = addVertex(at(outer, 0), kOuter);
    addVertex(at(inner, 0), kInner);
    for (int index = 0; index < kSymbolSegments; ++index) {
      const quint32 outer_a = first + (2U * static_cast<quint32>(index));
      const quint32 outer_b = index + 1 < kSymbolSegments ? addVertex(at(outer, index + 1), kOuter) : first;
      if (index + 1 < kSymbolSegments) {
        addVertex(at(inner, index + 1), kInner);
      }
      addTriangle(outer_a, outer_a + 1, outer_b);
      addTriangle(outer_a + 1, outer_b + 1, outer_b);
    }
  }

  // Fills a convex polygon that is star-shaped around `center` (every QwtSymbol
  // outline is) as a triangle fan, its rim pushed out by the antialiasing
  // fringe along each edge's normal.
  void addFan(const QPointF& center, const std::vector<QPointF>& ring, const QColor& color) {
    if (curve_limit_hit_ || ring.size() < 3 || !finitePoint(center) || !color.isValid() || color.alpha() == 0) {
      return;
    }
    const std::size_t count = ring.size();
    std::vector<QPointF>& normals = fan_normals_;
    normals.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
      const QPointF edge = ring[(index + 1) % count] - ring[index];
      const qreal length = std::sqrt(QPointF::dotProduct(edge, edge));
      QPointF normal = length > 0.0 ? QPointF(-edge.y() / length, edge.x() / length) : QPointF();
      const QPointF middle = (ring[index] + ring[(index + 1) % count]) * 0.5;
      if (QPointF::dotProduct(normal, middle - center) < 0.0) {
        normal = -normal;
      }
      normals[index] = normal;
    }
    std::vector<QPointF>& rim = fan_rim_;
    rim.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
      const QPointF& before = normals[(index + count - 1) % count];
      const QPointF& after = normals[index];
      const qreal miter = 1.0 + QPointF::dotProduct(before, after);
      rim[index] = ring[index] + (miter > 0.1 ? (before + after) * (fringe_ / miter) : after * fringe_);
    }
    if (!reserve(count + 1, 3 * count)) {
      return;
    }
    setColor(color);
    code_ = kSolidCode;
    const quint32 centre = addVertex(center, kCentre);
    for (const QPointF& corner : rim) {
      addVertex(corner, kOuter);
    }
    for (std::size_t index = 0; index < count; ++index) {
      addTriangle(
          centre, centre + 1 + static_cast<quint32>(index), centre + 1 + static_cast<quint32>((index + 1) % count));
    }
  }

  // An axis-aligned filled rectangle, antialiased on all four sides: the across
  // coordinate runs down its height, the second coordinate across its width.
  void addRect(const QRectF& rect, const QColor& color) {
    if (curve_limit_hit_ || !rect.isValid() || !finitePoint(rect.topLeft()) || !finitePoint(rect.bottomRight()) ||
        !color.isValid() || color.alpha() == 0 || !rect.intersects(QRectF(QPointF(0.0, 0.0), QSizeF(logical_size_))) ||
        !reserve(4, 6)) {
      return;
    }
    const QRectF box = rect.adjusted(-fringe_, -fringe_, fringe_, fringe_);
    setColor(color);
    code_ = kBoxCode;
    constexpr std::uint16_t kLeft = 0;
    constexpr std::uint16_t kRight = 0xFFFF;
    const quint32 top_left = addVertex(box.topLeft(), kInner, kLeft);
    addVertex(box.bottomLeft(), kOuter, kLeft);
    addVertex(box.bottomRight(), kOuter, kRight);
    addVertex(box.topRight(), kInner, kRight);
    addTriangle(top_left, top_left + 1, top_left + 2);
    addTriangle(top_left, top_left + 2, top_left + 3);
  }

  // Qwt's placement of an axis-aligned line (QwtPainter rounding alignment): the
  // coordinate rounds to a whole logical pixel, then an odd device width centres
  // on the device pixel holding it and an even one on the nearest pixel edge, so
  // a 1 px line covers exactly one pixel row or column at any dpr.
  [[nodiscard]] qreal snapToPixels(qreal coordinate, qreal width) const {
    const qreal device_width = std::max<qreal>(1.0, effectivePenWidth(width) * dpr_);
    const bool odd = (std::llround(device_width) % 2) == 1;
    const qreal device = std::round(coordinate) * dpr_;
    return (odd ? std::floor(device) + 0.5 : std::round(device)) / dpr_;
  }

  // Qt's predefined patterns in pen widths (on, off, ...); empty for a solid or
  // custom pen, which draws solid.
  [[nodiscard]] static std::span<const qreal> dashPattern(Qt::PenStyle style) {
    static constexpr std::array<qreal, 2> kDash = {4.0, 2.0};
    static constexpr std::array<qreal, 2> kDot = {1.0, 2.0};
    static constexpr std::array<qreal, 4> kDashDot = {4.0, 2.0, 1.0, 2.0};
    static constexpr std::array<qreal, 6> kDashDotDot = {4.0, 2.0, 1.0, 2.0, 1.0, 2.0};
    switch (style) {
      case Qt::DashLine:
        return kDash;
      case Qt::DotLine:
        return kDot;
      case Qt::DashDotLine:
        return kDashDot;
      case Qt::DashDotDotLine:
        return kDashDotDot;
      default:
        return {};
    }
  }

  // Scratch for a symbol outline (appendSymbol), reused across symbols.
  [[nodiscard]] std::vector<QPointF>& symbolOutline() {
    symbol_outline_.clear();
    return symbol_outline_;
  }

 private:
  // plot_color.frag's style codes: 1-4 the dashed styles (+5 when capped).
  [[nodiscard]] static int dashCode(Qt::PenStyle style) {
    switch (style) {
      case Qt::DashLine:
        return 1;
      case Qt::DotLine:
        return 2;
      case Qt::DashDotLine:
        return 3;
      case Qt::DashDotDotLine:
        return 4;
      default:
        return 0;
    }
  }
  static constexpr std::uint8_t kSolidCode = 0;
  static constexpr std::uint8_t kBoxCode = 10;
  // The across coordinate: -1, 0 and 1 (the rim or the far side of a stroke).
  static constexpr std::uint8_t kInner = 0;
  static constexpr std::uint8_t kCentre = 128;
  static constexpr std::uint8_t kOuter = 255;
  // Resolution of a dash distance in the 16-bit second coordinate (1/64 pen
  // width), and the longest quad that fits it: a longer clipped segment is
  // drawn as several quads.
  static constexpr qreal kDashSteps = 64.0;
  static constexpr qreal kMaxDashQuadUnits = 960.0;

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

  // One stroke quad; `offset` is the unit normal times the half width plus the
  // fringe, `dash_start`/`dash_end` the encoded path distance at each end.
  void addStrokeQuad(
      const QPointF& start, const QPointF& end, const QPointF& offset, std::uint16_t dash_start = 0,
      std::uint16_t dash_end = 0) {
    if (!reserve(4, 6)) {
      return;
    }
    const quint32 start_pair = addPair(start, offset, dash_start);
    addQuad(start_pair, addPair(end, offset, dash_end));
  }

  // The two corners of a stroke end: `point` minus and plus `offset` (inner
  // then outer across). Returns the first one's index.
  quint32 addPair(const QPointF& point, const QPointF& offset, std::uint16_t dash = 0) {
    const quint32 first = addVertex(point - offset, kInner, dash);
    addVertex(point + offset, kOuter, dash);
    return first;
  }

  // The quad between two corner pairs.
  void addQuad(quint32 start_pair, quint32 end_pair) {
    addTriangle(start_pair, end_pair, end_pair + 1);
    addTriangle(start_pair, end_pair + 1, start_pair + 1);
  }

  // The bevel filling the outside of a turn from `incoming` to `outgoing` (unit
  // directions) at `point`; `half` includes the fringe. Always solid.
  void addBevel(const QPointF& point, const QPointF& incoming, const QPointF& outgoing, qreal half) {
    const qreal cross = (incoming.x() * outgoing.y()) - (incoming.y() * outgoing.x());
    if (std::abs(cross) <= std::numeric_limits<qreal>::epsilon() || !reserve(3, 3)) {
      return;
    }
    const qreal side = (cross > 0.0 ? -1.0 : 1.0) * half;
    const std::uint8_t saved_code = code_;
    code_ = kSolidCode;
    const quint32 centre = addVertex(point, kCentre);
    addVertex(point + QPointF(-incoming.y(), incoming.x()) * side, kOuter);
    addVertex(point + QPointF(-outgoing.y(), outgoing.x()) * side, kOuter);
    addTriangle(centre, centre + 1, centre + 2);
    code_ = saved_code;
  }

  // Hot path for every solid curve: one direction per segment, reused by the
  // segment quad and the bevel join after it. Consecutive points are distinct
  // (addPolyline compacts duplicates). Square caps at the two ends.
  //
  // An indexed strip: each segment is a quad between two corner pairs, and a
  // turn's bevel is the triangle from the join point to the two outer corners,
  // which it shares with the quads. The outgoing pair is the next quad's start
  // pair; a straight join shares one pair, a collinear reversal shares none.
  void addSolidPolyline(const std::vector<QPointF>& points, qreal width) {
    const std::size_t segments = points.size() - 1;
    segment_directions_.resize(segments);
    for (std::size_t index = 0; index < segments; ++index) {
      const QPointF delta = points[index + 1] - points[index];
      segment_directions_[index] = delta / std::sqrt(QPointF::dotProduct(delta, delta));
    }
    const QRectF bounds(QPointF(0.0, 0.0), QSizeF(logical_size_));
    const QRectF join_bounds = bounds.adjusted(-width, -width, width, width);
    const qreal cap = width * 0.5;
    const qreal half = cap + fringe_;
    code_ = kSolidCode;
    constexpr quint32 kNone = std::numeric_limits<quint32>::max();
    // The corner pair a segment starting unclipped at its first point reuses.
    quint32 joined_start = kNone;
    for (std::size_t index = 0; index < segments; ++index) {
      if (curve_limit_hit_) {
        return;
      }
      const QPointF& from = points[index];
      const QPointF& to = points[index + 1];
      const QPointF& direction = segment_directions_[index];
      const QPointF offset = QPointF(-direction.y(), direction.x()) * half;
      qreal lower = 0.0;
      qreal upper = 1.0;
      quint32 end_pair = kNone;
      if (((bounds.contains(from) && bounds.contains(to)) || clipLine(from, to, lower, upper)) && reserve(4, 6)) {
        const QPointF delta = to - from;
        const quint32 start_pair =
            index > 0 && lower == 0.0 && joined_start != kNone
                ? joined_start
                : addPair(from + (delta * lower) - (index == 0 ? direction * cap : QPointF()), offset);
        // A clipped end stays at the canvas edge: an off-canvas `to` can lie
        // beyond the float range of the clip coordinates.
        const QPointF end =
            upper < 1.0 ? from + (delta * upper) : (index + 1 == segments ? to + (direction * cap) : to);
        end_pair = addPair(end, offset);
        addQuad(start_pair, end_pair);
      }
      const bool joined_at_end = end_pair != kNone && upper == 1.0;
      joined_start = kNone;
      if (index + 1 >= segments || !join_bounds.contains(to)) {
        continue;
      }
      const QPointF& next = segment_directions_[index + 1];
      const qreal cross = (direction.x() * next.y()) - (direction.y() * next.x());
      if (std::abs(cross) <= std::numeric_limits<qreal>::epsilon()) {
        // Straight on shares the pair. A reversal flips the normal: the next
        // segment starts a pair of its own (a bevel there has no area).
        const bool straight = QPointF::dotProduct(direction, next) > 0.0;
        joined_start = straight && joined_at_end ? end_pair : kNone;
        continue;
      }
      if (!reserve(5, 3)) {
        return;
      }
      const quint32 incoming = joined_at_end ? end_pair : addPair(to, offset);
      joined_start = addPair(to, QPointF(-next.y(), next.x()) * half);
      // Pairs are (point - normal, point + normal): the outer side of a left
      // turn (cross > 0) is the first corner.
      const quint32 outer = cross > 0.0 ? 0U : 1U;
      addTriangle(addVertex(to, kCentre), incoming + outer, joined_start + outer);
    }
  }

  // One quad per visible segment carrying the path distance in pen widths; the
  // shader cuts the dashes. Each segment's distance is reduced modulo the
  // pattern period in double precision first, so a huge source phase stays
  // exact in the 16-bit attribute. A turn gets its bevel only where a dash
  // runs through it.
  void addDashedPolyline(
      const std::vector<QPointF>& points, const std::vector<qreal>* offsets, qreal width, Qt::PenStyle style,
      bool capped) {
    const std::span<const qreal> pattern = dashPattern(style);
    const qreal period = std::accumulate(pattern.begin(), pattern.end(), 0.0);
    const qreal cap = width * 0.5;
    const qreal half = cap + fringe_;
    const auto code = static_cast<std::uint8_t>(dashCode(style) + (capped ? 5 : 0));
    const auto encode = [](qreal units) {
      return static_cast<std::uint16_t>(std::clamp<qreal>(std::round(units * kDashSteps), 0.0, 65535.0));
    };
    const QRectF join_bounds = QRectF(QPointF(0.0, 0.0), QSizeF(logical_size_)).adjusted(-width, -width, width, width);
    const auto phase = [period](qreal units) {
      return std::isfinite(units) ? std::fmod(std::max<qreal>(0.0, units), period) : 0.0;
    };
    const std::size_t segments = points.size() - 1;
    qreal path_offset = 0.0;
    for (std::size_t index = 0; index < segments; ++index) {
      if (curve_limit_hit_) {
        return;
      }
      const QPointF& from = points[index];
      const QPointF& to = points[index + 1];
      const qreal length = QLineF(from, to).length();
      const qreal offset = offsets != nullptr ? (*offsets)[index] : path_offset;
      path_offset += length;
      const QPointF direction = (to - from) / length;
      qreal lower = 0.0;
      qreal upper = 1.0;
      if (clipLine(from, to, lower, upper) && upper > lower) {
        const QPointF start = from + ((to - from) * lower);
        // One period ahead, so the start cap's half width stays positive.
        qreal dash_start = period + std::fmod(phase(offset / width) + phase(lower * length / width), period);
        const qreal units = (upper - lower) * length / width;
        const bool start_cap = capped && index == 0 && lower == 0.0;
        const bool end_cap = capped && index + 1 == segments && upper == 1.0;
        const QPointF normal = QPointF(-direction.y(), direction.x()) * half;
        code_ = code;
        for (qreal done = 0.0; done < units && !curve_limit_hit_; done += kMaxDashQuadUnits) {
          const qreal step = std::min(kMaxDashQuadUnits, units - done);
          QPointF quad_start = start + direction * (done * width);
          QPointF quad_end = start + direction * ((done + step) * width);
          qreal quad_dash = dash_start;
          qreal quad_dash_end = dash_start + step;
          if (start_cap && done == 0.0) {
            quad_start -= direction * cap;
            quad_dash -= 0.5;
          }
          if (end_cap && done + step >= units) {
            quad_end += direction * cap;
            quad_dash_end += 0.5;
          }
          addStrokeQuad(quad_start, quad_end, normal, encode(quad_dash), encode(quad_dash_end));
          dash_start = period + std::fmod(dash_start + step, period);
        }
      }
      if (index + 1 < segments && join_bounds.contains(to)) {
        const qreal next_offset = offsets != nullptr ? (*offsets)[index + 1] : offset + length;
        if (dashDrawnAt(pattern, (offset + length) / width) && dashDrawnAt(pattern, next_offset / width)) {
          const QPointF next = points[index + 2] - to;
          addBevel(to, direction, next / std::sqrt(QPointF::dotProduct(next, next)), half);
        }
      }
    }
  }

  [[nodiscard]] static bool dashDrawnAt(std::span<const qreal> pattern, qreal offset) {
    const qreal total = std::accumulate(pattern.begin(), pattern.end(), 0.0);
    const qreal phase = std::isfinite(offset) ? std::fmod(std::max<qreal>(0.0, offset), total) : 0.0;
    qreal start = 0.0;
    for (std::size_t index = 0; index < pattern.size(); index += 2) {
      if (phase >= start && phase <= start + pattern[index]) {
        return true;
      }
      start += pattern[index] + pattern[index + 1];
    }
    return false;
  }

  // Room for `count` more vertices and `index_count` more indices under the
  // frame ceiling and the active curve share; whole primitives only, so the
  // buffers never hold a partial one.
  [[nodiscard]] bool reserve(std::size_t count, std::size_t index_count) {
    if (count_ + count > kMaxGeometryVertices) {
      truncated_ = true;
      curve_limit_hit_ = true;
      return false;
    }
    if (count_ + count > active_vertex_limit_) {
      curve_limit_hit_ = true;
      return false;
    }
    if (logical_size_.width() <= 0 || logical_size_.height() <= 0) {
      return false;
    }
    if (count_ + count > capacity_) {
      capacity_ = std::max(count_ + count, capacity_ * 2);
      sink_.resize(static_cast<qsizetype>(capacity_ * sizeof(Vertex)));
      data_ = sink_.data();
    }
    if (index_count_ + index_count > index_capacity_) {
      index_capacity_ = std::max(index_count_ + index_count, index_capacity_ * 2);
      index_sink_.resize(static_cast<qsizetype>(index_capacity_ * sizeof(quint32)));
      indices_ = reinterpret_cast<quint32*>(index_sink_.data());
    }
    return true;
  }

  // Hot path (millions of vertices per rebuild): convert the colour once per
  // run of same-coloured primitives and apply the affine clip transform inline.
  void setColor(const QColor& color) {
    if (color != packed_source_) {
      packed_source_ = color;
      const QRgb rgba = color.rgba();
      packed_color_ = {
          static_cast<std::uint8_t>(qRed(rgba)), static_cast<std::uint8_t>(qGreen(rgba)),
          static_cast<std::uint8_t>(qBlue(rgba)), static_cast<std::uint8_t>(qAlpha(rgba))};
    }
  }

  // After reserve(): room is guaranteed. Returns the vertex's index.
  quint32 addVertex(const QPointF& point, std::uint8_t across, std::uint16_t second = 0) {
    const Vertex vertex{
        .x = static_cast<float>(clip_xx_ * point.x() + clip_xy_ * point.y() + clip_dx_),
        .y = static_cast<float>(clip_yx_ * point.x() + clip_yy_ * point.y() + clip_dy_),
        .rgba = packed_color_,
        .aux = {across, code_, static_cast<std::uint8_t>(second >> 8U), static_cast<std::uint8_t>(second & 0xFFU)},
    };
    std::memcpy(data_ + (count_ * sizeof(Vertex)), &vertex, sizeof(Vertex));
    return static_cast<quint32>(count_++);
  }

  void addTriangle(quint32 a, quint32 b, quint32 c) {
    indices_[index_count_] = a;
    indices_[index_count_ + 1] = b;
    indices_[index_count_ + 2] = c;
    index_count_ += 3;
  }

  QByteArray& sink_;
  QByteArray& index_sink_;
  char* data_ = nullptr;
  quint32* indices_ = nullptr;
  std::size_t count_ = 0;
  std::size_t capacity_ = 0;
  std::size_t index_count_ = 0;
  std::size_t index_capacity_ = 0;
  QSize logical_size_;
  // The clip transform's coefficients (x' = xx x + xy y + dx, y' = yx x + yy y + dy).
  qreal clip_xx_ = 0.0;
  qreal clip_xy_ = 0.0;
  qreal clip_dx_ = 0.0;
  qreal clip_yx_ = 0.0;
  qreal clip_yy_ = 0.0;
  qreal clip_dy_ = 0.0;
  qreal dpr_ = 1.0;
  // Half a device pixel in logical pixels: how far geometry reaches past a true edge.
  qreal fringe_ = 0.5;
  // Scratch reused across primitives: addSolidPolyline's directions, a
  // compacted polyline, addLine's two points, an ellipse's ring, a fan's edge
  // normals and rim, and a symbol outline. Each has one user at a time.
  std::vector<QPointF> segment_directions_;
  std::vector<QPointF> compact_points_;
  std::vector<qreal> compact_offsets_;
  std::vector<QPointF> line_points_;
  std::vector<QPointF> ellipse_ring_;
  std::vector<QPointF> fan_normals_;
  std::vector<QPointF> fan_rim_;
  std::vector<QPointF> symbol_outline_;
  QColor packed_source_;
  std::array<std::uint8_t, 4> packed_color_{};
  std::uint8_t code_ = kSolidCode;
  std::size_t active_vertex_limit_ = kMaxGeometryVertices;
  bool curve_limit_hit_ = false;
  bool truncated_ = false;
};

// Vertices, not indices: the frame and curve budgets count vertices.
[[nodiscard]] std::size_t estimatedVerticesPerSample(const QwtPlotCurve* curve) {
  if (curve == nullptr) {
    return 1;
  }
  std::size_t cost = 4;
  switch (curve->style()) {
    case QwtPlotCurve::NoCurve:
      cost = 1;
      break;
    case QwtPlotCurve::Lines:
      // Per sample the end corner pair of one segment and the start pair of
      // the next (shared on a straight join), dashed or not (the shader cuts
      // the dashes; a dashed turn adds a 3-vertex bevel).
      cost = 5;
      break;
    case QwtPlotCurve::Sticks:
      cost = 4;
      break;
    case QwtPlotCurve::Steps:
      // Two segments and (asymptotically) two joins per source interval.
      cost = 10;
      break;
    case QwtPlotCurve::Dots:
      // QPainter's default SquareCap turns each drawPoint into one square.
      cost = 4;
      break;
    case QwtPlotCurve::UserCurve:
      cost = 10;
      break;
  }
  if (const QwtSymbol* symbol = curve->symbol(); symbol != nullptr && symbol->style() != QwtSymbol::NoSymbol) {
    // The larger app-visible case: a filled 12-segment ellipse (13 vertices)
    // and its pen ring (24), or a polygon fan and its closed outline stroke.
    cost += static_cast<std::size_t>(kSymbolSegments * 4);
  }
  if (const auto* plot_curve = dynamic_cast<const PlotCurve*>(curve);
      plot_curve != nullptr && curve->style() == QwtPlotCurve::Lines && plot_curve->dotWidth() > 0.0) {
    cost += 4;
  }
  return cost;
}

// A visible curve with data and something to draw: a line style or a symbol.
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

// The grid's enabled major and minor ticks on the bottom and left axes.
void appendGrid(GeometryBuilder& geometry, const QwtPlot* plot, const QwtPlotGrid* grid) {
  if (grid == nullptr || !grid->isVisible()) {
    return;
  }
  // Qwt's extent (QwtPlotGrid::drawLines): the canvas contents, from the first
  // to the last pixel row and column, so the dash phase starts where Qwt's does.
  const QRectF contents(plot->canvas()->contentsRect());
  const qreal left = contents.left();
  const qreal right = contents.right() - 1.0;
  const qreal top = contents.top();
  const qreal bottom = contents.bottom() - 1.0;
  const auto append_ticks = [&](QwtAxisId axis, bool enabled, bool minor_enabled) {
    if (!enabled) {
      return;
    }
    const QwtScaleMap map = plot->canvasMap(axis);
    const QwtScaleDiv& division = plot->axisScaleDiv(axis);
    const auto append = [&](int tick_type, const QPen& pen) {
      for (double tick : division.ticks(tick_type)) {
        const qreal pixel = map.transform(tick);
        const qreal aligned = std::round(pixel);  // the whole pixel Qwt keeps or drops
        if (QwtAxis::isXAxis(axis)) {
          if (aligned >= left && aligned <= right) {
            geometry.addLine(QPointF(pixel, top), QPointF(pixel, bottom), pen.widthF(), pen.color(), pen.style());
          }
        } else if (aligned >= top && aligned <= bottom) {
          geometry.addLine(QPointF(left, pixel), QPointF(right, pixel), pen.widthF(), pen.color(), pen.style());
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

// What the reducer kept of one curve, for the budget retries and the browser
// breadcrumb.
struct CurveReductionStats {
  std::size_t input_samples = 0;
  std::size_t retained_samples = 0;
  std::size_t input_runs = 0;
  std::size_t dropped_runs = 0;
};

// One QwtSymbol at `point`; `fallback` colours an unsupported style drawn as a
// square when the symbol has neither brush nor pen.
void appendSymbol(GeometryBuilder& geometry, QPointF point, const QwtSymbol* symbol, const QColor& fallback) {
  if (symbol == nullptr || symbol->style() == QwtSymbol::NoSymbol) {
    return;
  }
  // Qwt draws these aliased, on whole pixels (qwt_symbol.cpp): crisp here too.
  const bool aliased = symbol->style() == QwtSymbol::Rect || symbol->style() == QwtSymbol::Cross ||
                       symbol->style() == QwtSymbol::HLine || symbol->style() == QwtSymbol::VLine;
  if (aliased) {
    point = QPointF(std::round(point.x()), std::round(point.y()));
  }
  const QSize size = symbol->size();
  const QSizeF radii(size.width() * 0.5, size.height() * 0.5);
  const QPen symbol_pen = symbol->pen();
  const bool has_pen = symbol_pen.style() != Qt::NoPen && symbol_pen.color().alpha() > 0;
  if (symbol->style() == QwtSymbol::Ellipse) {
    const QBrush brush = symbol->brush();
    if (brush.style() != Qt::NoBrush) {
      geometry.addEllipse(point, radii, brush.color());
    }
    if (has_pen) {
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

  // The outlines follow qwt_symbol.cpp; brush fills, pen strokes.
  const qreal x = point.x();
  const qreal y = point.y();
  const qreal left = x - radii.width();
  const qreal right = x + radii.width();
  const qreal top = y - radii.height();
  const qreal bottom = y + radii.height();
  const auto stroke = [&](QPointF from, QPointF to) {
    if (has_pen) {
      geometry.addLine(from, to, symbol_pen.widthF(), symbol_pen.color(), symbol_pen.style());
    }
  };
  std::vector<QPointF>& outline = geometry.symbolOutline();
  switch (symbol->style()) {
    case QwtSymbol::Rect: {
      // The outline on pixel centres or edges, as an aliased QPainter::drawRect.
      const qreal pen_width = symbol_pen.widthF();
      outline = {
          {geometry.snapToPixels(left, pen_width), geometry.snapToPixels(top, pen_width)},
          {geometry.snapToPixels(right, pen_width), geometry.snapToPixels(top, pen_width)},
          {geometry.snapToPixels(right, pen_width), geometry.snapToPixels(bottom, pen_width)},
          {geometry.snapToPixels(left, pen_width), geometry.snapToPixels(bottom, pen_width)}};
      break;
    }
    case QwtSymbol::Diamond:
      outline = {{x, top}, {right, y}, {x, bottom}, {left, y}};
      break;
    case QwtSymbol::Triangle:
    case QwtSymbol::UTriangle:
      outline = {{x, top}, {right, bottom}, {left, bottom}};
      break;
    case QwtSymbol::DTriangle:
      outline = {{x, bottom}, {left, top}, {right, top}};
      break;
    case QwtSymbol::LTriangle:
      outline = {{left, y}, {right, top}, {right, bottom}};
      break;
    case QwtSymbol::RTriangle:
      outline = {{right, y}, {left, bottom}, {left, top}};
      break;
    case QwtSymbol::Hexagon: {
      const qreal quarter = size.height() * 0.25;
      outline = {{x, top},    {right, top + quarter},   {right, bottom - quarter},
                 {x, bottom}, {left, bottom - quarter}, {left, top + quarter}};
      break;
    }
    case QwtSymbol::Star1: {
      const qreal diagonal_x = radii.width() * kSqrtHalf;
      const qreal diagonal_y = radii.height() * kSqrtHalf;
      stroke({x - diagonal_x, y - diagonal_y}, {x + diagonal_x, y + diagonal_y});
      stroke({x - diagonal_x, y + diagonal_y}, {x + diagonal_x, y - diagonal_y});
      [[fallthrough]];
    }
    case QwtSymbol::Cross:
      stroke({left, y}, {right, y});
      stroke({x, top}, {x, bottom});
      return;
    case QwtSymbol::XCross:
      stroke({left, top}, {right, bottom});
      stroke({left, bottom}, {right, top});
      return;
    case QwtSymbol::HLine:
      stroke({left, y}, {right, y});
      return;
    case QwtSymbol::VLine:
      stroke({x, top}, {x, bottom});
      return;
    default: {
      // Star2, path, pixmap, graphic, SVG and user symbols: a filled square
      // of the symbol size keeps the sample visible.
      warnUnsupportedOnce(u"QwtSymbol style %1 (drawn as a square)"_s.arg(static_cast<int>(symbol->style())));
      const QColor color =
          symbol->brush().style() != Qt::NoBrush ? symbol->brush().color() : (has_pen ? symbol_pen.color() : fallback);
      geometry.addRect(QRectF(point - QPointF(radii.width(), radii.height()), QSizeF(size)), color);
      return;
    }
  }
  if (symbol->brush().style() != Qt::NoBrush) {
    geometry.addFan(point, outline, symbol->brush().color());
  }
  if (has_pen) {
    outline.push_back(outline.front());
    geometry.addPolyline(outline, symbol_pen.widthF(), symbol_pen.color(), symbol_pen.style());
  }
}

// One curve reduced to `sample_budget` samples, drawn in its style, then its
// "Lines and Dots" dots, then its symbols (Qwt's order). `mapped_scratch`, when
// given, holds the mapped samples (desktop); null maps lazily (browser).
[[nodiscard]] CurveReductionStats appendCurve(
    GeometryBuilder& geometry, const QwtPlot* plot, const QwtPlotCurve* curve, std::size_t sample_budget,
    std::size_t pixel_columns, std::vector<QPointF>* mapped_scratch) {
  if (curve == nullptr || !curve->isVisible() || curve->data() == nullptr) {
    return {};
  }
  const std::size_t count = curve->dataSize();
  if (count == 0) {
    return {};
  }
  if (curve->style() == QwtPlotCurve::UserCurve) {
    warnUnsupportedOnce(u"QwtPlotCurve::UserCurve style (only its symbols are drawn)"_s);
  }
  if (curve->brush().style() != Qt::NoBrush) {
    warnUnsupportedOnce(u"QwtPlotCurve area brush (the fill is not drawn)"_s);
  }
  if (curve->testCurveAttribute(QwtPlotCurve::Fitted)) {
    warnUnsupportedOnce(u"QwtPlotCurve::Fitted (drawn unfitted)"_s);
  }
  const QwtScaleMap x_map = plot->canvasMap(curve->xAxis());
  const QwtScaleMap y_map = plot->canvasMap(curve->yAxis());
  const auto transform = [&](const QPointF& point) {
    return QPointF(x_map.transform(point.x()), y_map.transform(point.y()));
  };
  const auto map_sample = [&](std::size_t index) {
    const QPointF sample = curve->sample(index);
    return finitePoint(sample)
               ? transform(sample)
               : QPointF(std::numeric_limits<qreal>::quiet_NaN(), std::numeric_limits<qreal>::quiet_NaN());
  };
  // The reducer scans the series several times. Mapping it once into the
  // caller's scratch (reused across curves) replaces a virtual sample() + two
  // scale transforms per sample per pass with a vector read.
  PlotSampleAccessor mapped_sample = map_sample;
  std::size_t visible_count = count;
  if (mapped_scratch != nullptr) {
    mapped_scratch->clear();
    // A time series (finite, x non-decreasing) keeps only its visible index
    // range plus a margin for edge symbols and dots, and one sample beyond it
    // on each side so the edge segments still cross the canvas border: only
    // that range is stored, while one pass checks the whole series is sorted.
    // Dashed pens keep the whole series: their phase is measured from the first sample.
    const Qt::PenStyle pen_style = curve->pen().style();
    bool sorted = pen_style == Qt::SolidLine || pen_style == Qt::NoPen;
    if (sorted) {
      constexpr qreal kCullMargin = 32.0;
      const qreal right = plot->canvas()->width() + kCullMargin;
      qreal previous_x = -std::numeric_limits<qreal>::infinity();
      QPointF before_view;  // the last sample left of the view
      bool storing = true;
      for (std::size_t index = 0; index < count && sorted; ++index) {
        const QPointF point = map_sample(index);
        sorted = finitePoint(point) && point.x() >= previous_x;
        previous_x = point.x();
        if (!sorted || !storing) {
          continue;
        }
        if (point.x() < -kCullMargin) {
          before_view = point;
          continue;
        }
        if (mapped_scratch->empty() && index > 0) {
          mapped_scratch->push_back(before_view);
        }
        mapped_scratch->push_back(point);
        storing = point.x() <= right;
      }
      if (sorted && mapped_scratch->empty()) {
        mapped_scratch->push_back(before_view);  // everything is left of the view: keep the last sample
      }
    }
    if (!sorted) {
      mapped_scratch->clear();
      mapped_scratch->reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        mapped_scratch->push_back(map_sample(index));
      }
    }
    visible_count = mapped_scratch->size();
    mapped_sample = [mapped_scratch](std::size_t index) { return (*mapped_scratch)[index]; };
  }
  if (visible_count == 0) {
    return {};
  }
  PlotSampleReduction reduction =
      mapped_scratch != nullptr
          ? reducePlotSamples(std::span<const QPointF>(*mapped_scratch), sample_budget, pixel_columns)
          : reducePlotSamples(visible_count, sample_budget, pixel_columns, mapped_sample);
  const QPen pen = curve->pen();
  const qreal width = geometry.effectivePenWidth(pen.widthF());
  const bool dashed = !GeometryBuilder::dashPattern(pen.style()).empty();
  const QwtPlotCurve::CurveStyle style = curve->style();
  if (dashed && (style == QwtPlotCurve::Lines || style == QwtPlotCurve::Steps)) {
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
      geometry.addLine(baseline, point, pen.widthF(), pen.color(), pen.style());
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
    geometry.addPolyline(curve_path.points, pen.widthF(), pen.color(), pen.style(), curve_path.phase_offsets);
  }
  // PlotCurve's "Lines and Dots" pass: square dots of the dot width over the
  // line, as PlotCurve::drawCurve's SquareCap drawPoints batch paints them.
  if (const auto* plot_curve = dynamic_cast<const PlotCurve*>(curve);
      plot_curve != nullptr && style == QwtPlotCurve::Lines && plot_curve->dotWidth() > 0.0) {
    const qreal dot = plot_curve->dotWidth();
    for (const ReducedPlotSample& sample : reduction.samples) {
      geometry.addRect(QRectF(sample.point - QPointF(dot * 0.5, dot * 0.5), QSizeF(dot, dot)), pen.color());
    }
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

// A marker's lines across the canvas contents and its symbol; its label is text.
void appendMarker(GeometryBuilder& geometry, const QwtPlot* plot, const QwtPlotMarker* marker) {
  if (marker == nullptr || !marker->isVisible()) {
    return;
  }
  const QwtScaleMap x_map = plot->canvasMap(marker->xAxis());
  const QwtScaleMap y_map = plot->canvasMap(marker->yAxis());
  const QPointF point(x_map.transform(marker->xValue()), y_map.transform(marker->yValue()));
  const QPen line_pen = marker->linePen();
  // Qwt's extent (QwtPlotMarker::drawLines): the canvas contents.
  const QRectF contents(plot->canvas()->contentsRect());
  if (marker->lineStyle() == QwtPlotMarker::VLine || marker->lineStyle() == QwtPlotMarker::Cross) {
    geometry.addLine(
        QPointF(point.x(), contents.top()), QPointF(point.x(), contents.bottom() - 1.0), line_pen.widthF(),
        line_pen.color(), line_pen.style());
  }
  if (marker->lineStyle() == QwtPlotMarker::HLine || marker->lineStyle() == QwtPlotMarker::Cross) {
    geometry.addLine(
        QPointF(contents.left(), point.y()), QPointF(contents.right() - 1.0, point.y()), line_pen.widthF(),
        line_pen.color(), line_pen.style());
  }
  if (const QwtSymbol* symbol = marker->symbol(); symbol != nullptr && symbol->style() != QwtSymbol::NoSymbol) {
    appendSymbol(geometry, point, symbol, line_pen.color());
  }
}

// The canvas frame the Qwt canvases draw (QFrame::Box | QFrame::Plain, line
// width 1, the palette's WindowText): the outermost logical pixel ring, over
// every item.
void appendFrame(GeometryBuilder& geometry, QSize logical_size, const QColor& color) {
  const qreal width = logical_size.width();
  const qreal height = logical_size.height();
  geometry.addRect(QRectF(0.0, 0.0, width, 1.0), color);
  geometry.addRect(QRectF(0.0, height - 1.0, width, 1.0), color);
  geometry.addRect(QRectF(0.0, 1.0, 1.0, height - 2.0), color);
  geometry.addRect(QRectF(width - 1.0, 1.0, 1.0, height - 2.0), color);
}

}  // namespace

[[nodiscard]] PlotGeometry buildCurveLayer(
    QByteArray& sink, QByteArray& index_sink, const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio,
    std::size_t pixel_columns, const QTransform& to_clip, std::vector<QPointF>* mapped_scratch,
    const QColor& frame_color) {
  GeometryBuilder geometry(sink, index_sink, logical_size, device_pixel_ratio, to_clip);
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
  const std::size_t density_budget = std::max(kMinDensitySamples, pixel_columns * 4);
  const std::size_t frame_sample_budget =
      kPerCurveDensity ? density_budget * std::max<std::size_t>(1, curve_count) : density_budget;
  for (const QwtPlotItem* item : items) {
    if (const auto* grid = dynamic_cast<const QwtPlotGrid*>(item)) {
      const std::size_t before = geometry.indexCount();
      appendGrid(geometry, plot, grid);
      if (geometry.indexCount() > before) {
        geometry_stats.item_spans.push_back({item, before, geometry.indexCount() - before});
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
      const std::size_t indices_before = geometry.indexCount();
      const bool truncated_before = geometry.truncated();
      std::size_t attempt_budget = sample_budget;
      std::size_t initial_retained_samples = 0;
      CurveReductionStats reduction;
      bool committed = false;
      bool retried = false;
      while (!committed) {
        geometry.beginCurveBudget(vertex_share);
        const CurveReductionStats attempt =
            appendCurve(geometry, plot, curve, attempt_budget, pixel_columns, mapped_scratch);
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
        geometry.rewind(before, indices_before, truncated_before);
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
      if (geometry.indexCount() > indices_before) {
        geometry_stats.item_spans.push_back({curve, indices_before, geometry.indexCount() - indices_before});
      }
      curve_vertices += curve_vertex_count;
      geometry_stats.input_samples += reduction.input_samples;
      geometry_stats.retained_samples += reduction.retained_samples;
      geometry_stats.input_runs += reduction.input_runs;
      geometry_stats.dropped_runs += reduction.dropped_runs;
    }
  }
  const std::size_t frame_start = geometry.indexCount();
  appendFrame(geometry, logical_size, frame_color);
  geometry_stats.frame = {nullptr, frame_start, geometry.indexCount() - frame_start};
  geometry_stats.curve_vertices = curve_vertices;
  geometry_stats.truncated = geometry.truncated();
  return geometry_stats;
}

[[nodiscard]] std::vector<ItemSpan> buildOverlayLayer(
    QByteArray& sink, QByteArray& index_sink, const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio,
    const QTransform& to_clip) {
  GeometryBuilder geometry(sink, index_sink, logical_size, device_pixel_ratio, to_clip);
  std::vector<ItemSpan> spans;
  for (const QwtPlotItem* item : plot->itemList()) {
    if (const auto* marker = dynamic_cast<const QwtPlotMarker*>(item)) {
      const std::size_t before = geometry.indexCount();
      appendMarker(geometry, plot, marker);
      if (geometry.indexCount() > before) {
        spans.push_back({item, before, geometry.indexCount() - before});
      }
    }
  }
  return spans;
}

namespace {

// Hashes a scale map's transformation by identity: exact type plus parameters.
// False for a transformation of any other type (a subclass included), whose
// mapping cannot be fingerprinted reliably.
[[nodiscard]] bool hashTransformation(std::uint64_t& key, const QwtTransform* transformation) {
  if (transformation == nullptr || typeid(*transformation) == typeid(QwtNullTransform)) {
    hashUInt(key, 0U);
    return true;
  }
  if (typeid(*transformation) == typeid(QwtLogTransform)) {
    hashUInt(key, 1U);
    return true;
  }
  if (typeid(*transformation) == typeid(QwtPowerTransform)) {
    // The exponent is private; 2^exponent is strictly monotonic in it.
    hashUInt(key, 2U);
    hashDouble(key, transformation->transform(2.0));
    return true;
  }
  return false;
}

}  // namespace

[[nodiscard]] std::optional<std::uint64_t> computeEnvKey(
    const QwtPlot* plot, QSize logical_size, QSize target_size, const QColor& clear_color, const QColor& frame_color) {
  std::uint64_t key = PJ::kFnv1aOffsetBasis;
  hashUInt(key, static_cast<std::uint64_t>(logical_size.width()));
  hashUInt(key, static_cast<std::uint64_t>(logical_size.height()));
  hashUInt(key, static_cast<std::uint64_t>(target_size.width()));
  hashUInt(key, static_cast<std::uint64_t>(target_size.height()));
  hashUInt(key, static_cast<std::uint64_t>(clear_color.rgba()));
  hashUInt(key, static_cast<std::uint64_t>(frame_color.rgba()));
  for (QwtAxisId axis : {QwtPlot::xBottom, QwtPlot::yLeft, QwtPlot::yRight, QwtPlot::xTop}) {
    const QwtScaleDiv& division = plot->axisScaleDiv(axis);
    hashDouble(key, division.lowerBound());
    hashDouble(key, division.upperBound());
    // The grid's ticks, and the map's transformation (a log axis maps the same
    // bounds elsewhere): what a replot of unchanged curves may still have moved.
    // Each list is prefixed by its type and length, so ticks moving between
    // lists (a major tick becoming a medium one) change the key.
    for (int tick_type : {QwtScaleDiv::MinorTick, QwtScaleDiv::MediumTick, QwtScaleDiv::MajorTick}) {
      const QList<double> ticks = division.ticks(tick_type);
      hashUInt(key, static_cast<std::uint64_t>(tick_type));
      hashUInt(key, static_cast<std::uint64_t>(ticks.size()));
      for (double tick : ticks) {
        hashDouble(key, tick);
      }
    }
    const QwtScaleMap map = plot->canvasMap(axis);
    hashDouble(key, map.p1());
    hashDouble(key, map.p2());
    if (!hashTransformation(key, map.transformation())) {
      return std::nullopt;
    }
  }
  return key;
}

namespace {

// Colour, width and style: what a pen changes in the geometry.
void hashPen(std::uint64_t& key, const QPen& pen) {
  hashUInt(key, static_cast<std::uint64_t>(pen.color().rgba()));
  hashDouble(key, pen.widthF());
  hashUInt(key, static_cast<std::uint64_t>(pen.style()));
}

}  // namespace

[[nodiscard]] CurveKeys computeCurveKeys(const QwtPlot* plot) {
  CurveKeys keys;
  std::uint64_t& key = keys.structure;
  for (const QwtPlotItem* item : plot->itemList()) {
    if (const auto* curve = dynamic_cast<const QwtPlotCurve*>(item)) {
      hashUInt(key, reinterpret_cast<std::uintptr_t>(curve));
      hashUInt(key, curve->isVisible() ? 1U : 0U);
      hashUInt(key, reinterpret_cast<std::uintptr_t>(curve->data()));
      hashUInt(key, curve->dataSize());
      hashPen(key, curve->pen());
      hashUInt(key, static_cast<std::uint64_t>(curve->brush().style()));
      hashUInt(key, static_cast<std::uint64_t>(curve->brush().color().rgba()));
      hashUInt(key, static_cast<std::uint64_t>(curve->style()));
      hashUInt(key, static_cast<std::uint64_t>(curve->xAxis()) << 8U | static_cast<std::uint64_t>(curve->yAxis()));
      hashDouble(key, curve->baseline());
      hashUInt(key, static_cast<std::uint64_t>(curve->orientation()));
      hashUInt(key, curve->testCurveAttribute(QwtPlotCurve::Inverted) ? 1U : 0U);
      if (const QwtSymbol* symbol = curve->symbol(); symbol != nullptr) {
        hashUInt(key, static_cast<std::uint64_t>(symbol->style()));
        hashUInt(
            key, static_cast<std::uint64_t>(symbol->size().width()) << 32U |
                     static_cast<std::uint32_t>(symbol->size().height()));
        hashPen(key, symbol->pen());
        hashUInt(key, static_cast<std::uint64_t>(symbol->brush().color().rgba()));
        hashUInt(key, static_cast<std::uint64_t>(symbol->brush().style()));
      } else {
        hashUInt(key, 0U);
      }
      if (const auto* plot_curve = dynamic_cast<const PlotCurve*>(curve)) {
        hashDouble(key, plot_curve->dotWidth());
      }
      if (keys.content.has_value() && isRenderableCurve(curve)) {
        if (const auto* series = dynamic_cast<const RevisionedSeries*>(curve->data()); series != nullptr) {
          hashUInt(*keys.content, series->contentRevision());
        } else {
          keys.content.reset();
        }
      }
    } else if (const auto* grid = dynamic_cast<const QwtPlotGrid*>(item)) {
      hashUInt(key, reinterpret_cast<std::uintptr_t>(grid));
      hashUInt(key, grid->isVisible() ? 1U : 0U);
      hashPen(key, grid->majorPen());
      hashPen(key, grid->minorPen());
      hashUInt(
          key, (grid->xEnabled() ? 1U : 0U) | (grid->yEnabled() ? 2U : 0U) | (grid->xMinEnabled() ? 4U : 0U) |
                   (grid->yMinEnabled() ? 8U : 0U));
    }
  }
  return keys;
}

}  // namespace PJ::plot_rhi
