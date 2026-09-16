#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QColor>
#include <QRectF>
#include <string_view>

#include "pj_base/builtin/plot_markers.hpp"

class QFontMetrics;
class QPainter;
class QwtScaleMap;

namespace PJ {

/// Marker coordinates in the plot's x/y axis units. The caller owns time-frame
/// conversion; the painter owns only the common visual representation.
struct MarkerPaintData {
  sdk::MarkerKind kind = sdk::MarkerKind::kEvent;
  double x0 = 0.0;
  double x1 = 0.0;
  double y0 = 0.0;
  double y1 = 0.0;
  bool has_value = false;
  QColor color;
  std::string_view label;
};

[[nodiscard]] QColor markerSeverityColor(sdk::MarkerSeverity severity, const sdk::ColorRGBA& override_color);

/// Draw one marker with the same shape, label placement and clipping in every chart.
void paintMarker(
    QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect,
    const QFontMetrics& fm, const MarkerPaintData& marker);

}  // namespace PJ
