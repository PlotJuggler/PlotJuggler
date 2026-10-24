#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Private to pj_plotting: the geometry layers of PlotRhiCanvas. Translates the
// visible Qwt grids, curves and markers into bounded, antialiased indexed
// triangles in clip space, and computes the keys that tell the canvas when its
// retained curve layer is stale.

#include <pj_widgets/Fnv1a.h>

#include <QByteArray>
#include <QColor>
#include <QLoggingCategory>
#include <QPointF>
#include <QSize>
#include <QtGlobal>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class QTransform;
class QwtPlot;

namespace PJ {

Q_DECLARE_LOGGING_CATEGORY(lcPlotRhi)

namespace plot_rhi {

// Interleaved geometry vertex (16 B), laid out to match the vertex input
// attributes wired in PlotRhiCanvas::createPipeline: Float2 clip-space
// position, UNormByte4 straight-alpha RGBA, and UNormByte4 coverage inputs for
// plot_color.frag: the position across the primitive (-1..1 as 0..255), the
// style code, and a 16-bit second coordinate (high, low byte): a dashed
// stroke's path distance in 64ths of a pen width, or a box's second axis.
struct Vertex {
  float x = 0.0F;
  float y = 0.0F;
  std::array<std::uint8_t, 4> rgba{};
  std::array<std::uint8_t, 4> aux{};
};

// One plot item's range in a layer: indices in a geometry layer, vertices in the
// text layer (item null for the frame).
struct ItemSpan {
  const void* item = nullptr;
  std::size_t first = 0;
  std::size_t count = 0;
};

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

// Reducer planning deliberately leaves one quarter of the hard ceiling for
// grids, markers, dash expansion, and estimator error. GeometryBuilder remains
// the final, exact safety boundary.
constexpr std::size_t kPlannedCurveVertices = (kMaxFrameVertices * 3) / 4;

// Fold a double into a running FNV-1a fingerprint via its raw bit pattern, so
// equal plots hash equal without touching the payload arrays twice.
inline void hashDouble(std::uint64_t& state, double value) {
  state = PJ::fnv1aMixBits(state, value);
}

inline void hashUInt(std::uint64_t& state, std::uint64_t value) {
  state = PJ::fnv1aMix(state, value);
}

// What one curve-layer build produced: the item spans (index ranges) and the
// frame's, plus the reducer and budget counters behind the limit warnings and
// the browser's content breadcrumb.
struct PlotGeometry {
  std::vector<ItemSpan> item_spans;
  ItemSpan frame;
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
    const QColor& frame_color);

// The markers' lines and symbols: the tracker, hover and ride-along markers.
// Returns one index span per marker that drew anything.
[[nodiscard]] std::vector<ItemSpan> buildOverlayLayer(
    QByteArray& sink, QByteArray& index_sink, const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio,
    const QTransform& to_clip);

// Slim ENVIRONMENT key: the view state that changes the rendered image without
// a QwtPlot::replot() (and therefore without a dirty flag set). Semantic
// changes — data, pens, items, symbols, curve styles — invalidate geometry via
// the replot() slot instead, so they are deliberately NOT hashed here. The four
// axis bounds are belt-and-braces: zoomers mutate scale divisions in place, and
// some paths reach the canvas without a full replot. nullopt when an axis uses a
// transformation hashTransformation() cannot identify: only a rebuild is safe.
[[nodiscard]] std::optional<std::uint64_t> computeEnvKey(
    const QwtPlot* plot, QSize logical_size, QSize target_size, const QColor& clear_color, const QColor& frame_color);

// The curve layer's keys, from one pass over the plot's items.
struct CurveKeys {
  // The STRUCTURE, which an overlay-only replot must not hide: the grids and
  // curves, their visibility, order, pens, brushes, styles and symbols, and each
  // curve's data object and size (a stream grows it). Same-size edits of a
  // series' values are not seen here; they come with a full replot().
  std::uint64_t structure = PJ::kFnv1aOffsetBasis;
  // The CONTENT: each renderable curve's RevisionedSeries::contentRevision()
  // (unique in the process, so it identifies the data object too). nullopt when a
  // curve's data has no revision (generic Qwt series): its samples may have
  // changed in place, so only a rebuild is safe.
  std::optional<std::uint64_t> content = PJ::kFnv1aOffsetBasis;
};

[[nodiscard]] CurveKeys computeCurveKeys(const QwtPlot* plot);

}  // namespace plot_rhi
}  // namespace PJ
