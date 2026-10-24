// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "PlotRhiTextAtlas.h"

#include <qwt_plot.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_item.h>
#include <qwt_plot_marker.h>
#include <qwt_scale_map.h>
#include <qwt_symbol.h>
#include <qwt_text.h>

#include <QFont>
#include <QPainter>
#include <QPicture>
#include <QTransform>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <utility>

#include "PlotRhiGeometry.h"
#include "pj_plotting/PlotLegend.h"

namespace PJ::plot_rhi {

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

namespace {

// A marker that paints only its label: the geometry layers draw its line and
// symbol.
class LabelOnlyMarker final : public QwtPlotMarker {
 protected:
  void drawLines(QPainter*, const QRectF&, const QPointF&) const override {}
  void drawSymbol(QPainter*, const QRectF&, const QPointF&) const override {}
};

// Where QwtPlotMarker::drawLabel() puts `marker`'s label, in canvas pixels: the
// exact bounds hint for its recording.
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

// Grows `scratch` to hold `logical_size` at `dpr` (never shrinks it) and clears
// that active rectangle. False when the allocation failed.
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

// The tight bounds of a raster's non-transparent pixels, and their count.
struct AlphaRasterScan {
  QRect bounds;
  qsizetype pixels = 0;
};

// Scans the top-left `active_size` of an RGBA8888_Premultiplied image.
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

// Copies the non-transparent part of `source`'s active rectangle into
// `cropped` (device pixels, dpr 1) and sets its logical rectangle. False when
// nothing was drawn or, with `allocation_failed` set, the copy failed.
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
    TextRasterBuild& result, const QHash<QByteArray, TextCacheEntry>& previous, QImage& scratch, TextLayerKind kind,
    const QRectF& canvas_rect, qreal dpr, const QFont& font, const QwtPlotItem* source_item, Draw&& draw,
    const QRectF& bounds_hint = {}) {
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
  const auto push = [&](const TextCacheEntry& entry) {
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

  TextCacheEntry entry;
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

}  // namespace

[[nodiscard]] TextRasterBuild buildTextRasters(
    const QwtPlot* plot, QSize logical_size, qreal device_pixel_ratio, QImage& scratch,
    const QHash<QByteArray, TextCacheEntry>& previous) {
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

namespace {

// Clips every layer to the largest texture the backend allows, less the
// sampler pad, marking the build truncated when one was clipped. False when an
// allocation failed.
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

}  // namespace

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

}  // namespace PJ::plot_rhi
