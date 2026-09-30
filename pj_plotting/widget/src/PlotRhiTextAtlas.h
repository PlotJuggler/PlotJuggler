#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Private to pj_plotting: the text layer of PlotRhiCanvas. Records the QPainter
// drawing of every plot item the geometry layers do not translate (the legend,
// marker labels, PlotMarkersItem, unknown QwtPlotItems), rasterises only the
// recordings that changed since the last build, and packs the rasters into one
// atlas drawn as textured quads.

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRectF>
#include <QSize>
#include <QtGlobal>
#include <cstddef>
#include <vector>

class QTransform;
class QwtPlot;
class QwtPlotItem;

namespace PJ::plot_rhi {

// One corner of a text quad: clip-space position and atlas uv.
struct TextVertex {
  float x = 0.0F;
  float y = 0.0F;
  float u = 0.0F;
  float v = 0.0F;
};

// A rasterised text layer (tight CPU raster and its logical rectangle), reused
// while the item's recorded drawing is unchanged; a null image records a layer
// that drew nothing.
struct TextCacheEntry {
  QImage image;
  QRectF logical_rect;
  qsizetype alpha_pixels = 0;
};

// Legend: the in-canvas legend. Marker: a QwtPlotMarker label. Item: any other
// plot item (PlotMarkersItem, unknown QwtPlotItems), rasterised whole.
enum class TextLayerKind { Legend, Marker, Item };

// One non-empty text layer of a build and its place in the atlas.
struct TextRasterLayer {
  QImage image;
  QRectF logical_rect;
  QPoint atlas_position;
  qsizetype alpha_pixels = 0;
  const QwtPlotItem* source_item = nullptr;
  TextLayerKind kind = TextLayerKind::Marker;
  QByteArray cache_key;
};

// Every text layer of one gather, in Qwt's item order, with totals for the
// browser's text breadcrumb.
struct TextRasterBuild {
  std::vector<TextRasterLayer> layers;
  QRectF logical_bounds;
  qsizetype alpha_pixels = 0;
  qsizetype legend_layers = 0;
  qsizetype marker_layers = 0;
  qsizetype item_layers = 0;
  // Every layer gathered (empty ones too), for the next build's lookups.
  QHash<QByteArray, TextCacheEntry> cache;
  std::size_t rasterized = 0;
  bool truncated = false;
  bool allocation_failed = false;
};

// What an uploaded atlas and its text quads were built from: an equal
// signature (canvas size, truncation, the layer keys in draw order) reuses them.
struct TextSignature {
  QSize canvas;
  bool truncated = false;
  QList<QByteArray> layer_keys;
  bool operator==(const TextSignature&) const = default;
};

// Recomputes the build's totals (bounds, alpha pixels, per-kind counts) from its layers.
void refreshTextRasterStats(TextRasterBuild& build);

// Gathers the text layers of `plot`'s visible items in item order: a layer whose
// recorded drawing is in `previous` reuses its raster, any other is rasterised
// through `scratch` (a reusable staging surface). At most kMaxTextLayers layers.
[[nodiscard]] TextRasterBuild buildTextRasters(
    const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio, QImage& scratch,
    const QHash<QByteArray, TextCacheEntry>& previous);

// Packs the build's layers into `atlas` in rows, each with a transparent
// one-pixel sampler pad, clipping any layer and dropping trailing ones that do
// not fit `max_texture_size` (marking the build truncated). False when an
// allocation failed.
[[nodiscard]] bool packTextAtlas(TextRasterBuild& build, int max_texture_size, QImage& atlas);

// Appends the two triangles drawing `uv_rect` of the atlas over the logical
// rectangle `rect`.
void appendTextQuad(
    std::vector<TextVertex>& vertices, const QRectF& rect, const QRectF& uv_rect, QSize logical_size,
    const QTransform& to_clip);

}  // namespace PJ::plot_rhi
