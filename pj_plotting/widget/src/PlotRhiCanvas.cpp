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
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QLineF>
#include <QLoggingCategory>
#include <QMatrix4x4>
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
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "pj_plotting/PlotCurve.h"
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
// when symbol expansion turns each retained sample into many vertices.
#ifdef PJ_TARGET_WASM
// 1,000,000 vertices at sizeof(Vertex) == 16 B is ~16 MB (plus at most three
// 4 B indices per vertex, ~9 MB for strokes) — a firm cap against
// exhausting the 32-bit wasm heap (a few hundred MB total, shared with
// Qt/Qwt/the datastore), while still far exceeding any legible line density.
constexpr std::size_t kMaxFrameVertices = 1'000'000;
// The browser splits one density budget (4 samples per device-pixel column)
// across every curve of the plot.
constexpr bool kPerCurveDensity = false;
#else
// Desktop: 8M vertices (128 MB of vertices and up to 96 MB of indices, on the
// CPU and the GPU each, at the cap)
// holds 50 full-density Lines curves on a 4K-wide canvas; the cap only guards
// pathological symbol expansion.
constexpr std::size_t kMaxFrameVertices = 8'000'000;
// Every curve gets the full density budget of its own: the reducer keeps each
// pixel column's X/Y extrema, so a curve drawn from 4 samples per device-pixel
// column is indistinguishable from the full series.
constexpr bool kPerCurveDensity = true;
#endif
#ifdef PJ_TARGET_WASM
// The 32-bit heap cannot hold a mapped copy of a multi-million-sample series
// (16 B per sample): map lazily on every reducer pass instead.
constexpr bool kMapSamplesOnce = false;
#else
constexpr bool kMapSamplesOnce = true;
#endif
// Mapped samples a canvas keeps between rebuilds (1 MiB): a zoomed-out view of
// a long series maps it all, and a canvas must not hold that for its lifetime.
constexpr std::size_t kRetainedMappedSamples = std::size_t{1} << 16;
// Density floor (samples) for narrow canvases.
constexpr std::size_t kMinDensitySamples = 2048;
constexpr std::size_t kTextVerticesPerLayer = 6;
constexpr std::size_t kMaxTextLayers = 256;
constexpr std::size_t kMaxTextVertices = kTextVerticesPerLayer * kMaxTextLayers;
constexpr std::size_t kMaxGeometryVertices = kMaxFrameVertices - kMaxTextVertices;
constexpr int kSymbolSegments = 12;
constexpr qreal kTau = 6.28318530717958647692;
constexpr qreal kSqrtHalf = 0.70710678118654752440;

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

// Logical widget pixels (y down) to the backend's clip space: GL-style NDC
// (y up) followed by QRhi::clipSpaceCorrMatrix(), which flips y on Vulkan. The
// shaders pass positions through unchanged, so this is the only place that
// knows the backend's conventions. z stays 0 and w 1, inside every backend's
// depth range; only the 2D part of the correction matters.
[[nodiscard]] QTransform logicalToClip(QSize logical_size, const QMatrix4x4& clip_correction) {
  if (logical_size.isEmpty()) {
    return QTransform(0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
  }
  const QTransform to_ndc(2.0 / logical_size.width(), 0.0, 0.0, -2.0 / logical_size.height(), -1.0, 1.0);
  const QTransform correction(
      clip_correction(0, 0), clip_correction(1, 0), clip_correction(0, 1), clip_correction(1, 1), clip_correction(0, 3),
      clip_correction(1, 3));
  return to_ndc * correction;
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

// QRhi's own name ("OpenGL", "Vulkan", "D3D11", "Metal"), except in the
// browser, whose breadcrumbs report WebGL as "OpenGLES2/WebGL".
[[nodiscard]] QString backendName(const QRhi& rhi) {
#ifdef PJ_TARGET_WASM
  if (rhi.backend() == QRhi::OpenGLES2) {
    return u"OpenGLES2/WebGL"_s;
  }
#endif
  return QString::fromLatin1(rhi.backendName());
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
// on push) to caller-owned PlotRhiVertex and quint32 index arrays in
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
        const QPointF end = index + 1 == segments ? to + direction * cap : (upper == 1.0 ? to : from + (delta * upper));
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

// Legend: the in-canvas legend. Marker: a QwtPlotMarker label. Item: any other
// plot item (PlotMarkersItem, unknown QwtPlotItems), rasterised whole.
enum class TextLayerKind { Legend, Marker, Item };

struct TextRasterLayer {
  QImage image;
  QRectF logical_rect;
  QPoint atlas_position;
  qsizetype alpha_pixels = 0;
  const QwtPlotItem* source_item = nullptr;
  TextLayerKind kind = TextLayerKind::Marker;
  QByteArray cache_key;
};

struct TextRasterBuild {
  std::vector<TextRasterLayer> layers;
  QRectF logical_bounds;
  qsizetype alpha_pixels = 0;
  qsizetype legend_layers = 0;
  qsizetype marker_layers = 0;
  qsizetype item_layers = 0;
  // Every layer gathered (empty ones too), for the next build's lookups.
  QHash<QByteArray, PlotRhiTextCacheEntry> cache;
  std::size_t rasterized = 0;
  bool truncated = false;
  bool allocation_failed = false;
};

void refreshTextRasterStats(TextRasterBuild& build) {
  build.logical_bounds = {};
  build.alpha_pixels = 0;
  build.legend_layers = 0;
  build.marker_layers = 0;
  build.item_layers = 0;
  for (const TextRasterLayer& layer : build.layers) {
    build.logical_bounds =
        build.logical_bounds.isValid() ? build.logical_bounds.united(layer.logical_rect) : layer.logical_rect;
    build.alpha_pixels += layer.alpha_pixels;
    switch (layer.kind) {
      case TextLayerKind::Legend:
        ++build.legend_layers;
        break;
      case TextLayerKind::Marker:
        ++build.marker_layers;
        break;
      case TextLayerKind::Item:
        ++build.item_layers;
        break;
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

// The cache key of one text layer: its recorded drawing (every command, font,
// colour and position), the rectangle it is rasterised in and the dpr.
[[nodiscard]] QByteArray textLayerKey(const QPicture& picture, const QRect& raster_bounds, qreal dpr) {
  QByteArray key(picture.data(), static_cast<qsizetype>(picture.size()));
  const std::array<qreal, 5> tail = {
      static_cast<qreal>(raster_bounds.x()), static_cast<qreal>(raster_bounds.y()),
      static_cast<qreal>(raster_bounds.width()), static_cast<qreal>(raster_bounds.height()), dpr};
  key.append(reinterpret_cast<const char*>(tail.data()), static_cast<qsizetype>(sizeof(tail)));
  return key;
}

// Records `draw` into a QPicture and appends the layer it paints: from
// `previous` when the same drawing was rasterised last time, else rasterised
// now. Every layer, empty ones included, lands in `result.cache`.
template <typename Draw>
void appendRecordedTextLayer(
    TextRasterBuild& result, const QHash<QByteArray, PlotRhiTextCacheEntry>& previous, QImage& scratch,
    TextLayerKind kind, const QRectF& canvas_rect, qreal dpr, const QFont& font, const QwtPlotItem* source_item,
    Draw&& draw, const QRectF& bounds_hint = {}) {
  if (result.layers.size() >= kMaxTextLayers) {
    result.truncated = true;
    return;
  }

  // Legends have no cheap bounds API: the recording's bounding rect gives the
  // touched rectangle. Marker labels come with an exact bounds hint.
  QPicture picture;
  QPainter recorder(&picture);
  recorder.setFont(font);
  recorder.setClipRect(canvas_rect);
  recorder.setRenderHint(QPainter::Antialiasing, true);
  recorder.setRenderHint(QPainter::TextAntialiasing, true);
  draw(&recorder);
  recorder.end();

  const QRectF logical_bounds = bounds_hint.isValid() ? bounds_hint : QRectF(picture.boundingRect());
  QRect recorded_bounds = logical_bounds.toAlignedRect().adjusted(-2, -2, 2, 2);
  recorded_bounds = recorded_bounds.intersected(canvas_rect.toAlignedRect());
  if (recorded_bounds.isEmpty()) {
    return;
  }
  QByteArray key = textLayerKey(picture, recorded_bounds, dpr);
  const auto push = [&](const PlotRhiTextCacheEntry& entry) {
    if (!entry.image.isNull()) {
      result.layers.push_back({
          .image = entry.image,
          .logical_rect = entry.logical_rect,
          .alpha_pixels = entry.alpha_pixels,
          .source_item = source_item,
          .kind = kind,
          .cache_key = key,
      });
    }
    result.cache.insert(key, entry);
  };
  if (const auto cached = previous.constFind(key); cached != previous.cend()) {
    push(*cached);
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
  picture.play(&raster);
  raster.end();
  ++result.rasterized;

  PlotRhiTextCacheEntry entry;
  const QSize active_size(
      std::max(1, static_cast<int>(std::ceil(recorded_bounds.width() * dpr))),
      std::max(1, static_cast<int>(std::ceil(recorded_bounds.height() * dpr))));
  if (cropTextRaster(
          scratch, active_size, entry.logical_rect, dpr, entry.image, entry.alpha_pixels, result.allocation_failed)) {
    entry.logical_rect.translate(recorded_bounds.topLeft());
  } else if (result.allocation_failed) {
    return;
  }
  push(entry);
}

[[nodiscard]] TextRasterBuild buildTextRasters(
    const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio, QImage& scratch,
    const QHash<QByteArray, PlotRhiTextCacheEntry>& previous) {
  TextRasterBuild result;
  if (plot == nullptr || logical_size.isEmpty() || device_pixel_ratio <= 0.0) {
    return result;
  }
  const qreal dpr = device_pixel_ratio;
  // The rect Qwt hands every item (QwtPlot::drawCanvas): the canvas's contents,
  // inside its margins. Legends and marker labels align to its edges.
  const QRectF canvas_rect = plot->canvas() != nullptr ? QRectF(plot->canvas()->contentsRect())
                                                       : QRectF(QPointF(0.0, 0.0), QSizeF(logical_size));
  const QFont text_font = plot->canvas() != nullptr ? plot->canvas()->font() : plot->font();
  const QwtScaleMap bottom_map = plot->canvasMap(QwtPlot::xBottom);
  const QwtScaleMap left_map = plot->canvasMap(QwtPlot::yLeft);
  for (const QwtPlotItem* item : plot->itemList()) {
    if (!item->isVisible() || dynamic_cast<const QwtPlotCurve*>(item) != nullptr ||
        dynamic_cast<const QwtPlotGrid*>(item) != nullptr) {
      continue;
    }
    if (dynamic_cast<const PlotLegend*>(item) != nullptr) {
      appendRecordedTextLayer(
          result, previous, scratch, TextLayerKind::Legend, canvas_rect, dpr, text_font, item,
          [item, &bottom_map, &left_map, &canvas_rect](QPainter* painter) {
            item->draw(painter, bottom_map, left_map, canvas_rect);
          });
      continue;
    }
    const auto* marker = dynamic_cast<const QwtPlotMarker*>(item);
    if (marker == nullptr) {
      // No geometry translation for this item: rasterise its own QPainter
      // drawing, in its own Z slot. Region fills make these layers large; the
      // cache keeps them until their drawing changes.
      const QwtScaleMap item_x_map = plot->canvasMap(item->xAxis());
      const QwtScaleMap item_y_map = plot->canvasMap(item->yAxis());
      appendRecordedTextLayer(
          result, previous, scratch, TextLayerKind::Item, canvas_rect, dpr, text_font, item,
          [item, &item_x_map, &item_y_map, &canvas_rect](QPainter* painter) {
            item->draw(painter, item_x_map, item_y_map, canvas_rect);
          });
      continue;
    }
    if (marker->label().isEmpty()) {
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
        result, previous, scratch, TextLayerKind::Marker, canvas_rect, dpr, text_font, item,
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

void appendTextQuad(
    std::vector<TextVertex>& vertices, const QRectF& rect, const QRectF& uv_rect, QSize logical_size,
    const QTransform& to_clip) {
  if (!rect.isValid() || logical_size.isEmpty()) {
    return;
  }
  // Atlas row 0 is v = 0 on every backend (uploaded images are not render
  // targets), so the uv corners stay tied to the logical corners.
  const QPointF top_left = to_clip.map(rect.topLeft());
  const QPointF bottom_right = to_clip.map(rect.bottomRight());
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

struct CurveReductionStats {
  std::size_t input_samples = 0;
  std::size_t retained_samples = 0;
  std::size_t input_runs = 0;
  std::size_t dropped_runs = 0;
};

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

struct PlotGeometry {
  std::vector<PlotRhiItemSpan> item_spans;
  PlotRhiItemSpan frame;
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

// The grid, the curves and the frame. `pixel_columns` is the render target's
// width in device pixels: the reducer keeps up to 4 extrema per column.
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

// The markers' lines and symbols: the tracker, hover and ride-along markers.
[[nodiscard]] std::vector<PlotRhiItemSpan> buildOverlayLayer(
    QByteArray& sink, QByteArray& index_sink, const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio,
    const QTransform& to_clip) {
  GeometryBuilder geometry(sink, index_sink, logical_size, device_pixel_ratio, to_clip);
  std::vector<PlotRhiItemSpan> spans;
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

// Slim ENVIRONMENT key: the view state that changes the rendered image without
// a QwtPlot::replot() (and therefore without a dirty flag set). Semantic
// changes — data, pens, items, symbols, curve styles — invalidate geometry via
// the replot() slot instead, so they are deliberately NOT hashed here. The four
// axis bounds are belt-and-braces: zoomers mutate scale divisions in place, and
// some paths reach the canvas without a full replot.
[[nodiscard]] std::uint64_t computeEnvKey(
    const QwtPlot* plot, QSize logical_size, QSize target_size, const QColor& clear_color, const QColor& frame_color) {
  std::uint64_t key = 0xcbf29ce484222325ULL;
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
  }
  return key;
}

void hashPen(std::uint64_t& key, const QPen& pen) {
  hashUInt(key, static_cast<std::uint64_t>(pen.color().rgba()));
  hashDouble(key, pen.widthF());
  hashUInt(key, static_cast<std::uint64_t>(pen.style()));
}

// The curve layer's STRUCTURE, which an overlay-only replot must not hide: the
// grids and curves, their visibility, order, pens, styles and symbols, and each
// curve's data object and size (a stream grows it). Same-size edits of a
// series' values are not seen here; they come with a full replot().
[[nodiscard]] std::uint64_t computeCurveKey(const QwtPlot* plot) {
  std::uint64_t key = 0xcbf29ce484222325ULL;
  for (const QwtPlotItem* item : plot->itemList()) {
    if (const auto* curve = dynamic_cast<const QwtPlotCurve*>(item)) {
      hashUInt(key, reinterpret_cast<std::uintptr_t>(curve));
      hashUInt(key, curve->isVisible() ? 1U : 0U);
      hashUInt(key, reinterpret_cast<std::uintptr_t>(curve->data()));
      hashUInt(key, curve->dataSize());
      hashPen(key, curve->pen());
      hashUInt(key, static_cast<std::uint64_t>(curve->style()));
      hashUInt(key, reinterpret_cast<std::uintptr_t>(curve->symbol()));
      if (const auto* plot_curve = dynamic_cast<const PlotCurve*>(curve)) {
        hashDouble(key, plot_curve->dotWidth());
      }
    } else if (const auto* grid = dynamic_cast<const QwtPlotGrid*>(item)) {
      hashUInt(key, reinterpret_cast<std::uintptr_t>(grid));
      hashUInt(key, grid->isVisible() ? 1U : 0U);
      hashPen(key, grid->majorPen());
      hashPen(key, grid->minorPen());
    }
  }
  return key;
}

// Grows `buffer` (a Static vertex buffer) to hold `bytes`, doubling, never past
// `ceiling`. False when the replacement cannot be created; the old buffer stays.
[[nodiscard]] bool ensureVertexBuffer(
    QRhi& rhi, QRhiBuffer*& buffer, qsizetype& capacity, qsizetype bytes,
    qsizetype ceiling = std::numeric_limits<qsizetype>::max(),
    QRhiBuffer::UsageFlags usage = QRhiBuffer::VertexBuffer) {
  if (bytes <= capacity) {
    return true;
  }
  const qsizetype new_capacity = std::min(ceiling, std::max(bytes, capacity * 2));
  auto replacement = std::unique_ptr<QRhiBuffer>(rhi.newBuffer(QRhiBuffer::Static, usage, new_capacity));
  if (!replacement->create()) {
    return false;
  }
  delete buffer;
  buffer = replacement.release();
  capacity = new_capacity;
  return true;
}

}  // namespace

PlotRhiCanvas::PlotRhiCanvas(QwtPlot* plot, QWidget* parent) : QRhiWidget(parent), plot_(plot) {
  pjPlotRhiInitResources();
  setApi(preferredGraphicsApi());
  // One sample per pixel into a colour-only target of our own: the shader
  // antialiases the geometry, and nothing uses depth or stencil.
  setAutoRenderTarget(false);
  setContentsMargins(1, 1, 1, 1);
  setAutoFillBackground(false);
  setMouseTracking(true);
  connect(this, &QRhiWidget::renderFailed, this, [this]() { reportFailure(/*emit_signal=*/false); });
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
  curves_dirty_ = true;
  overlay_dirty_ = true;
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

#ifdef PJ_RHI_TEST_HOOKS
std::size_t PlotRhiCanvas::curveLayerBuildsForTest() const noexcept {
  return curve_layer_builds_;
}

std::size_t PlotRhiCanvas::textLayerRasterizationsForTest() const noexcept {
  return text_layer_rasterizations_;
}
#endif

int PlotRhiCanvas::renderTargetBytesPerPixel() const {
  constexpr int kRgba8 = 4;
  int bytes = colorTexture() != nullptr ? kRgba8 : 0;
  if (msaaColorBuffer() != nullptr) {
    bytes += kRgba8 * msaaColorBuffer()->sampleCount() + (resolveTexture() != nullptr ? kRgba8 : 0);
  }
  if (depthStencilBuffer() != nullptr) {
    bytes += kRgba8 * depthStencilBuffer()->sampleCount();
  }
  return bytes;
}

void PlotRhiCanvas::setOverlayOnlyReplot(bool overlay_only) noexcept {
  overlay_only_replot_ = overlay_only;
}

std::size_t PlotRhiCanvas::retainedSampleScratchBytes() const noexcept {
  return mapped_scratch_.capacity() * sizeof(QPointF);
}

double PlotRhiCanvas::lastRenderCpuMs() const noexcept {
  return last_render_cpu_ms_;
}

double PlotRhiCanvas::lastGpuFrameMs() const noexcept {
  return last_gpu_frame_ms_;
}

void PlotRhiCanvas::grabPresentedFrame(std::function<void(const QImage&)> done) {
  PJ::grabPresentedFrame(*this, std::move(done));
}

void PlotRhiCanvas::replot() {
  // Authoritative geometry-invalidation source: QwtPlot::replot() reaches here
  // reflectively on every semantic change (data, pens, items, axes). Mark dirty
  // so the next render() rebuilds instead of reusing the retained buffers.
  curves_dirty_ = curves_dirty_ || !overlay_only_replot_;
  overlay_dirty_ = true;
  update();
}

void PlotRhiCanvas::clearTextLayers() {
  delete text_bindings_;
  text_bindings_ = nullptr;
  delete text_texture_;
  text_texture_ = nullptr;
  text_texture_capacity_ = {};
  text_vertices_.clear();
  text_spans_.clear();
  text_signature_ = {};
  text_cache_.clear();
  draw_batches_.clear();
}

void PlotRhiCanvas::releaseDrawResources() {
  clearTextLayers();
  text_raster_scratch_ = {};
  mapped_scratch_.clear();
  mapped_scratch_.shrink_to_fit();
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
  delete overlay_vertex_buffer_;
  overlay_vertex_buffer_ = nullptr;
  overlay_vertex_buffer_capacity_ = 0;
  delete index_buffer_;
  index_buffer_ = nullptr;
  index_buffer_capacity_ = 0;
  delete overlay_index_buffer_;
  overlay_index_buffer_ = nullptr;
  overlay_index_buffer_capacity_ = 0;
  curve_spans_.clear();
  overlay_spans_.clear();
  frame_span_ = {};
  // A fresh context has no uploaded geometry: start dirty and drop the env key
  // so the first frame after re-initialize() rebuilds and re-uploads.
  curves_dirty_ = true;
  overlay_dirty_ = true;
  env_key_valid_ = false;
  ready_reported_ = false;
  content_reported_ = false;
  truncation_reported_ = false;
  text_truncation_reported_ = false;
  text_failure_reported_ = false;
}

void PlotRhiCanvas::releaseResources() {
  noteRhiRelease(*this, rhi_);
  releaseDrawResources();
  color_target_.release();
  built_target_ = {};
  rhi_ = nullptr;
  composition_refresh_queued_ = false;
  last_refreshed_window_.clear();
}

bool PlotRhiCanvas::createPipeline(QRhiCommandBuffer* /*command_buffer*/) {
  if (rhi_ == nullptr || color_target_.renderPass() == nullptr) {
    return false;
  }
  const QShader vertex_shader = loadRhiShader(u":/pj_plotting/shaders/plot_color.vert.qsb"_s);
  const QShader fragment_shader = loadRhiShader(u":/pj_plotting/shaders/plot_color.frag.qsb"_s);
  const QShader text_vertex_shader = loadRhiShader(u":/pj_plotting/shaders/plot_text.vert.qsb"_s);
  const QShader text_fragment_shader = loadRhiShader(u":/pj_plotting/shaders/plot_text.frag.qsb"_s);
  if (!vertex_shader.isValid() || !fragment_shader.isValid() || !text_vertex_shader.isValid() ||
      !text_fragment_shader.isValid()) {
    qCritical("PlotRhiCanvas: failed to load plot shaders");
    return false;
  }

  if (!rhi_->isFeatureSupported(QRhi::ElementIndexUint)) {
    qCWarning(lcPlotRhi) << "PlotRhiCanvas: the backend has no 32-bit index buffers";
    return false;
  }
  auto shader_resources = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
  shader_resources->setBindings({});
  if (!shader_resources->create()) {
    return false;
  }

  // Static (device-local): the geometry is retained across frames and only
  // re-uploaded on a rebuild, and a Dynamic buffer would be host-visible and
  // duplicated per frame in flight on Vulkan/D3D/Metal.
  constexpr qsizetype kInitialVertexBufferCapacity = 64 * 1024;
  auto vertex_buffer = std::unique_ptr<QRhiBuffer>(
      rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, kInitialVertexBufferCapacity));
  if (!vertex_buffer->create()) {
    return false;
  }

  QRhiVertexInputLayout input_layout;
  input_layout.setBindings({QRhiVertexInputBinding(sizeof(Vertex))});
  input_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, offsetof(Vertex, x)),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::UNormByte4, offsetof(Vertex, rgba)),
      QRhiVertexInputAttribute(0, 2, QRhiVertexInputAttribute::UNormByte4, offsetof(Vertex, aux)),
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
  pipeline->setRenderPassDescriptor(color_target_.renderPass());
  if (!createRhiPipeline(*this, *pipeline, "color")) {
    return false;
  }
  shader_resources_ = shader_resources.release();
  vertex_buffer_ = vertex_buffer.release();
  vertex_buffer_capacity_ = kInitialVertexBufferCapacity;
  pipeline_ = pipeline.release();

  // The text pipeline is optional: without it legends and labels drop, the
  // curves still draw. There is no raster fallback in the browser.
  const auto drop_text = [] {
    qCWarning(lcPlotRhi) << "PlotRhiCanvas: failed to create the text pipeline; rendering geometry without text";
    return true;
  };
  auto text_sampler = std::unique_ptr<QRhiSampler>(rhi_->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  if (!text_sampler->create()) {
    return drop_text();
  }
  auto text_placeholder_texture = std::unique_ptr<QRhiTexture>(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
  if (!text_placeholder_texture->create()) {
    return drop_text();
  }
  auto text_layout_bindings = std::unique_ptr<QRhiShaderResourceBindings>(rhi_->newShaderResourceBindings());
  text_layout_bindings->setBindings({QRhiShaderResourceBinding::sampledTexture(
      0, QRhiShaderResourceBinding::FragmentStage, text_placeholder_texture.get(), text_sampler.get())});
  if (!text_layout_bindings->create()) {
    return drop_text();
  }
  constexpr qsizetype kInitialTextVertexBufferCapacity = 16 * 1024;
  auto text_vertex_buffer = std::unique_ptr<QRhiBuffer>(
      rhi_->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer, kInitialTextVertexBufferCapacity));
  if (!text_vertex_buffer->create()) {
    return drop_text();
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
  text_pipeline->setRenderPassDescriptor(color_target_.renderPass());
  if (!createRhiPipeline(*this, *text_pipeline, "text")) {
    return drop_text();
  }

  text_sampler_ = text_sampler.release();
  text_placeholder_texture_ = text_placeholder_texture.release();
  text_layout_bindings_ = text_layout_bindings.release();
  text_vertex_buffer_ = text_vertex_buffer.release();
  text_vertex_buffer_capacity_ = kInitialTextVertexBufferCapacity;
  text_pipeline_ = text_pipeline.release();
  return true;
}

void PlotRhiCanvas::initialize(QRhiCommandBuffer* command_buffer) {
  QRhi* current_rhi = rhi();
  if (current_rhi == nullptr) {
    reportFailure(/*emit_signal=*/true);
    return;
  }
  // A new QRhi (reparent to another top-level, device loss) invalidates every
  // resource; a new output format or sample count invalidates the pipelines.
  if (rhi_ != current_rhi) {
    noteRhiAdoption(*this, rhi_, current_rhi);
    releaseResources();
    rhi_ = current_rhi;
  }
  // The widget rebuilds its colour buffer in place on a resize: refresh the
  // target on it every time.
  if (!color_target_.ensure(*rhi_, colorTexture(), msaaColorBuffer(), resolveTexture(), /*refresh=*/true)) {
    reportFailure(/*emit_signal=*/true);
    return;
  }
  const RhiTargetKey target = RhiTargetKey::of(*color_target_.target(), sampleCount());
  if (target != built_target_) {
    releaseDrawResources();
    built_target_ = target;
  }
  renderer_backend_ = backendName(*rhi_);
  // After a failure this is the only retry: render() does not call it again
  // until the QRhi or the render target changes.
  if (pipeline_ == nullptr && !createPipeline(command_buffer)) {
    reportFailure(/*emit_signal=*/true);
    return;
  }
  if (failure_label_ != nullptr) {
    failure_label_->hide();
  }
  (void)failure_.set({});
  if (!ready_reported_) {
    ready_reported_ = true;
    const QRhiDriverInfo driver = rhi_->driverInfo();
    qInfo(
        "PJ_WASM_PLOT_RHI_READY backend=%s device=%s samples=%d", qPrintable(renderer_backend_),
        driver.deviceName.constData(), sampleCount());
  }
}

bool PlotRhiCanvas::rebuildTextLayers(
    const QwtPlot& plot, qreal device_pixel_ratio, const QTransform& to_clip, QImage& upload, qsizetype& bytes) {
  TextRasterBuild text_raster =
      text_pipeline_ != nullptr ? buildTextRasters(&plot, size(), device_pixel_ratio, text_raster_scratch_, text_cache_)
                                : TextRasterBuild{};
  text_layer_rasterizations_ += text_raster.rasterized;
  text_cache_ = std::move(text_raster.cache);
  bool force_text_failure = false;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  force_text_failure = property("pjWasmForceTextAtlasFailure").toBool();
  if (force_text_failure) {
    const int attempts = property("pjWasmTextFailureAttempts").toInt() + 1;
    setProperty("pjWasmTextFailureAttempts", attempts);
    qInfo("PJ_WASM_TEXT_FAILURE_ATTEMPT count=%d", attempts);
  }
#endif
  // The same layers at the same places over the same canvas: the uploaded
  // atlas and quads are still right.
  TextSignature signature{.canvas = size(), .truncated = text_raster.truncated};
  signature.layer_keys.reserve(static_cast<qsizetype>(text_raster.layers.size()));
  for (const TextRasterLayer& layer : text_raster.layers) {
    signature.layer_keys.append(layer.cache_key);
  }
  if (!force_text_failure && !text_raster.allocation_failed && signature == text_signature_) {
    // Equal keys may come from other items (a marker replaced by an identical
    // one): the spans follow this build's items.
    for (std::size_t index = 0; index < std::min(text_spans_.size(), text_raster.layers.size()); ++index) {
      text_spans_[index].item = text_raster.layers[index].source_item;
    }
    return false;
  }
  text_signature_ = {};
  text_vertices_.clear();
  text_spans_.clear();
  bytes = 0;
  upload = {};

  bool retry = false;
  bool text_failed = false;
  QString text_failure_reason;
  const auto drop_text_for_frame = [&](const QString& reason) {
    if (!text_failed && !text_failure_reported_) {
      // Retry one later frame for a transient allocation failure. A repeated
      // failure remains geometry-only until another dirty source (semantic,
      // view, rebind, or lifecycle) instead of entering an unbounded loop.
      retry = true;
    }
    text_failed = true;
    text_failure_reason = reason;
    text_raster.layers.clear();
    text_raster.truncated = true;
    refreshTextRasterStats(text_raster);
    text_vertices_.clear();
    text_spans_.clear();
    bytes = 0;
    upload = {};
  };
  const int max_texture_size = rhi_->resourceLimit(QRhi::TextureSizeMax);
  if (force_text_failure) {
    drop_text_for_frame(u"test-injected text atlas failure"_s);
  } else if (text_raster.allocation_failed || !packTextAtlas(text_raster, max_texture_size, upload)) {
    drop_text_for_frame(u"failed to allocate or pack the text atlas"_s);
  }
  if (!text_failed && !text_raster.layers.empty()) {
    const QSize required = upload.size();
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
        appendTextQuad(text_vertices_, layer.logical_rect, uv_rect, size(), to_clip);
        text_spans_.push_back({layer.source_item, before, text_vertices_.size() - before});
      }
      bytes = static_cast<qsizetype>(text_vertices_.size() * sizeof(TextVertex));
      if (!ensureVertexBuffer(*rhi_, text_vertex_buffer_, text_vertex_buffer_capacity_, bytes)) {
        drop_text_for_frame(u"failed to allocate the text vertex buffer"_s);
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
          upload.width(), upload.height(), text_raster.logical_bounds.x(), text_raster.logical_bounds.y(),
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
    text_signature_ = signature;
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
  return retry;
}

void PlotRhiCanvas::render(QRhiCommandBuffer* command_buffer) {
  QElapsedTimer cpu_timer;
  cpu_timer.start();
  // QRhiWidget does not re-run initialize() for every new QRhi (a grab of a
  // hidden widget creates one), so never draw with resources of another QRhi.
  // A new colour buffer always gets its initialize().
  if (rhi_ != rhi()) {
    initialize(command_buffer);
  }
  QwtPlot* plot = plot_;
  const QColor clear_color = plot != nullptr ? plot->canvasBackground().color() : QColor(Qt::transparent);
  bool force_pipeline_failure = false;
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
  force_pipeline_failure = property("pjWasmForcePipelineFailure").toBool();
#endif
  // Failed state (no device, pipeline, or render target): a clear-only pass.
  QRhiTextureRenderTarget* render_target = color_target_.target();
  if (force_pipeline_failure || rhi_ == nullptr || pipeline_ == nullptr || render_target == nullptr) {
    if (render_target != nullptr) {
      command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, nullptr);
      command_buffer->endPass();
    }
    reportFailure(/*emit_signal=*/true);
    return;
  }
  if (plot == nullptr) {
    // A null plot is a transient wiring state (setPlot() runs right after
    // construction), not a GPU failure. Record a clear-only pass so the frame is
    // well-defined, without the failure banner/telemetry. Stay dirty so the
    // first frame after wiring rebuilds geometry.
    command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, nullptr);
    command_buffer->endPass();
    return;
  }
  const QSize target_size = render_target->pixelSize();
  const qreal device_pixel_ratio = renderTargetScale(size(), target_size);
  // Qwt owns the canvas brush. QRhiWidget does not participate in Qwt's
  // QPainter background path, so read the brush back from the plot instead of
  // assuming the platform's accelerated-surface palette.
  const auto fail_rebuild = [this, command_buffer, render_target, &clear_color]() {
    // A failed allocation/upload must not leave an old text-bearing frame on
    // screen or hand QRhi an unrecorded render callback. Submit a defined clear
    // and keep the state dirty so a later frame can retry from live Qwt state.
    command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, nullptr);
    command_buffer->endPass();
    env_key_valid_ = false;
    reportFailure(/*emit_signal=*/true);
  };

  // Rebuild the curve layer when a full replot flagged it, the view (env key)
  // or the curve structure changed; the overlay and text on any rebuild or an
  // overlay-only replot. Otherwise re-draw the retained buffers without
  // rebuilding or re-uploading: compositor refreshes and expose repaints hit
  // neither branch.
  const QColor frame_color = palette().color(QPalette::WindowText);
  const std::uint64_t env_key = computeEnvKey(plot, size(), target_size, clear_color, frame_color);
  const std::uint64_t curve_key = computeCurveKey(plot);
  const bool rebuild_curves = curves_dirty_ || !env_key_valid_ || env_key != env_key_ || curve_key != curve_key_;
  const bool rebuild_overlay = rebuild_curves || overlay_dirty_;
  const QTransform to_clip = logicalToClip(size(), rhi_->clipSpaceCorrMatrix());

  qsizetype bytes = 0;
  qsizetype overlay_bytes = 0;
  qsizetype text_bytes = 0;
  QImage text_upload;
  bool retry_text_rebuild = false;
  if (rebuild_curves) {
    vertices_.resize(0);  // keeps the allocation
    indices_.resize(0);
    const PlotGeometry geometry = buildCurveLayer(
        vertices_, indices_, plot, size(), device_pixel_ratio,
        static_cast<std::size_t>(std::max(1, target_size.width())), to_clip,
        kMapSamplesOnce ? &mapped_scratch_ : nullptr, frame_color);
    if (mapped_scratch_.capacity() > kRetainedMappedSamples) {
      mapped_scratch_.clear();
      mapped_scratch_.shrink_to_fit();
    }
    ++curve_layer_builds_;
    last_vertex_count_ = static_cast<std::size_t>(vertices_.size()) / sizeof(Vertex);
    bytes = vertices_.size() + indices_.size();
    curve_spans_ = geometry.item_spans;
    frame_span_ = geometry.frame;

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
                               << "curve(s) to keep every curve complete; discarded trial vertices="
                               << static_cast<qulonglong>(geometry.budget_discarded_trial_vertices);
        }
      }
    } else {
      truncation_reported_ = false;
    }

#ifdef PJ_TARGET_WASM
    // Content breadcrumb: emit on the first curve-bearing frame and again
    // whenever the plotted content changes. Format is fixed (Playwright greps).
    // Browser-only: a streaming desktop plot would log it on every frame.
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
            curves.size(), static_cast<unsigned long long>(last_vertex_count_),
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
#endif

    // Never grow past the frame-wide ceilings: the builder already caps the
    // vertex count there, and a primitive has at most three indices per vertex.
    if (!ensureVertexBuffer(
            *rhi_, vertex_buffer_, vertex_buffer_capacity_, vertices_.size(),
            static_cast<qsizetype>(kMaxGeometryVertices * sizeof(Vertex))) ||
        !ensureVertexBuffer(
            *rhi_, index_buffer_, index_buffer_capacity_, indices_.size(),
            static_cast<qsizetype>(3 * kMaxGeometryVertices * sizeof(quint32)), QRhiBuffer::IndexBuffer)) {
      fail_rebuild();
      return;
    }
  }

  if (rebuild_overlay) {
    overlay_vertices_.resize(0);
    overlay_indices_.resize(0);
    overlay_spans_ = buildOverlayLayer(overlay_vertices_, overlay_indices_, plot, size(), device_pixel_ratio, to_clip);
    overlay_bytes = overlay_vertices_.size() + overlay_indices_.size();
    if (!ensureVertexBuffer(
            *rhi_, overlay_vertex_buffer_, overlay_vertex_buffer_capacity_, overlay_vertices_.size(),
            static_cast<qsizetype>(kMaxGeometryVertices * sizeof(Vertex))) ||
        !ensureVertexBuffer(
            *rhi_, overlay_index_buffer_, overlay_index_buffer_capacity_, overlay_indices_.size(),
            static_cast<qsizetype>(3 * kMaxGeometryVertices * sizeof(quint32)), QRhiBuffer::IndexBuffer)) {
      fail_rebuild();
      return;
    }
    retry_text_rebuild = rebuildTextLayers(*plot, device_pixel_ratio, to_clip, text_upload, text_bytes);

    // Qwt paints items in increasing Z and stable attachment order. The layers
    // live in separate GPU buffers, so retain an explicit batch list instead of
    // flattening every label above every marker. This matters when the
    // playback label, hover marker, and XY ride-along markers overlap.
    draw_batches_.clear();
    const auto append_batch = [this](Layer layer, const PlotRhiItemSpan& span) {
      if (span.count == 0) {
        return;
      }
      const auto first = static_cast<std::uint32_t>(span.first);
      const auto count = static_cast<std::uint32_t>(span.count);
      if (!draw_batches_.empty()) {
        DrawBatch& previous = draw_batches_.back();
        if (previous.layer == layer && previous.first + previous.count == first) {
          previous.count += count;
          return;
        }
      }
      draw_batches_.push_back({.layer = layer, .first = first, .count = count});
    };
    const auto append_item = [&append_batch](Layer layer, const std::vector<PlotRhiItemSpan>& spans, const void* item) {
      const auto span = std::find_if(
          spans.cbegin(), spans.cend(), [item](const PlotRhiItemSpan& candidate) { return candidate.item == item; });
      if (span != spans.cend()) {
        append_batch(layer, *span);
      }
    };
    for (const QwtPlotItem* item : plot->itemList()) {
      append_item(Layer::Curves, curve_spans_, item);
      append_item(Layer::Overlay, overlay_spans_, item);
      append_item(Layer::Text, text_spans_, item);
    }
    append_batch(Layer::Curves, frame_span_);
#ifdef PJ_WASM_ENABLE_INGRESS_PROBE
    QByteArray batch_sequence;
    batch_sequence.reserve(static_cast<qsizetype>(draw_batches_.size()));
    for (const DrawBatch& batch : draw_batches_) {
      batch_sequence.append(batch.layer == Layer::Text ? 'T' : 'G');
    }
    qInfo(
        "PJ_WASM_PLOT_DRAW_BATCHES count=%lld sequence=%s", static_cast<long long>(draw_batches_.size()),
        batch_sequence.constData());
#endif
  }
  if (rebuild_curves) {
    env_key_ = env_key;
    curve_key_ = curve_key;
    env_key_valid_ = true;
  }

  QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
  if (rebuild_curves && bytes > 0) {
    updates->uploadStaticBuffer(vertex_buffer_, 0, vertices_);
    updates->uploadStaticBuffer(index_buffer_, 0, indices_);
  }
  if (rebuild_overlay && overlay_bytes > 0) {
    updates->uploadStaticBuffer(overlay_vertex_buffer_, 0, overlay_vertices_);
    updates->uploadStaticBuffer(overlay_index_buffer_, 0, overlay_indices_);
  }
  if (text_bytes > 0 && text_texture_ != nullptr && !text_upload.isNull()) {
    QRhiTextureSubresourceUploadDescription upload(text_upload);
    upload.setSourceSize(text_upload.size());
    updates->uploadTexture(text_texture_, QRhiTextureUploadDescription({0, 0, upload}));
    updates->uploadStaticBuffer(text_vertex_buffer_, 0, text_bytes, text_vertices_.data());
  }
  command_buffer->beginPass(render_target, clear_color, {1.0F, 0}, updates);
  const QRhiViewport viewport(
      0.0F, 0.0F, static_cast<float>(target_size.width()), static_cast<float>(target_size.height()));
  for (const DrawBatch& batch : draw_batches_) {
    if (batch.layer == Layer::Text) {
      if (text_pipeline_ == nullptr || text_bindings_ == nullptr || text_vertex_buffer_ == nullptr) {
        continue;
      }
      command_buffer->setGraphicsPipeline(text_pipeline_);
      command_buffer->setShaderResources(text_bindings_);
      const QRhiCommandBuffer::VertexInput binding(text_vertex_buffer_, 0);
      command_buffer->setVertexInput(0, 1, &binding);
      // After the pipeline: QRhi records the viewport as dynamic state of the
      // bound pipeline, and Vulkan draws nothing without it.
      command_buffer->setViewport(viewport);
      command_buffer->draw(batch.count, 1, batch.first);
      continue;
    }
    const bool overlay = batch.layer == Layer::Overlay;
    QRhiBuffer* vertices = overlay ? overlay_vertex_buffer_ : vertex_buffer_;
    QRhiBuffer* indices = overlay ? overlay_index_buffer_ : index_buffer_;
    if (vertices == nullptr || indices == nullptr) {
      continue;
    }
    command_buffer->setGraphicsPipeline(pipeline_);
    command_buffer->setShaderResources(shader_resources_);
    const QRhiCommandBuffer::VertexInput binding(vertices, 0);
    command_buffer->setVertexInput(0, 1, &binding, indices, 0, QRhiCommandBuffer::IndexUInt32);
    command_buffer->setViewport(viewport);
    command_buffer->drawIndexed(batch.count, 1, batch.first);
  }
  command_buffer->endPass();
  // Allocation failures are retryable. If the retry succeeds without a QRhi
  // re-initialize() cycle, remove the overlay here rather than leaving a healthy
  // renderer hidden underneath a stale diagnostic.
  if (failure_label_ != nullptr) {
    failure_label_->hide();
  }
  (void)failure_.set({});
  last_render_cpu_ms_ = static_cast<double>(cpu_timer.nsecsElapsed()) / 1e6;
  last_gpu_frame_ms_ = command_buffer->lastCompletedGpuTime() * 1000.0;
  // Geometry is now uploaded and drawn. A text-only allocation failure gets one
  // delayed retry while the usable geometry remains visible; repeated failures
  // wait for another semantic/view/rebind/lifecycle dirty source instead of
  // rebuilding every frame.
  curves_dirty_ = false;
  overlay_dirty_ = retry_text_rebuild;
  if (retry_text_rebuild) {
    QTimer::singleShot(100, this, [this]() {
      if (overlay_dirty_) {
        update();
      }
    });
  }
}

void PlotRhiCanvas::reportFailure(bool emit_signal) {
  if (failure_label_ == nullptr) {
    failure_label_ = new QLabel(tr("GPU plot renderer unavailable"), this);
    failure_label_->setAlignment(Qt::AlignCenter);
    failure_label_->setStyleSheet(u"background:#5b1b1b;color:white;padding:8px;"_s);
  }
  failure_label_->setGeometry(rect());
  failure_label_->show();
  failure_label_->raise();
  if (failure_.set(failure_label_->text()) != RhiFailureLatch::Change::kEntered) {
    return;
  }
#ifdef PJ_TARGET_WASM
  qCritical("PJ_WASM_PLOT_RHI_FAILED");
#else
  qCWarning(lcPlotRhi) << "PlotRhiCanvas:" << failure_.reason();
#endif
  if (emit_signal) {
    emit renderFailed();
  }
}

}  // namespace PJ
