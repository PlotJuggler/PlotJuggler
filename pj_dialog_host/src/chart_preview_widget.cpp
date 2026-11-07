// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <qwt_legend.h>
#include <qwt_legend_data.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_item.h>
#include <qwt_plot_zoomer.h>
#include <qwt_scale_div.h>
#include <qwt_scale_draw.h>
#include <qwt_scale_map.h>
#include <qwt_text.h>

#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPointF>
#include <QSignalBlocker>
#include <QVariant>
#include <QVector>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <pj_plugins/host_qt/chart_preview_widget.hpp>

#include "pj_plotting/MarkerPainter.h"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {
/// Default matplotlib "tab10" palette — 10 distinct colors, used to color a
/// series when it carries no explicit hex color (Qwt has no automatic theme).
const std::vector<QColor>& kDefaultPalette() {
  static const std::vector<QColor> k_palette = {
      QColor(0x1f, 0x77, 0xb4), QColor(0xff, 0x7f, 0x0e), QColor(0x2c, 0xa0, 0x2c), QColor(0xd6, 0x27, 0x28),
      QColor(0x94, 0x67, 0xbd), QColor(0x8c, 0x56, 0x4b), QColor(0xe3, 0x77, 0xc2), QColor(0x7f, 0x7f, 0x7f),
      QColor(0xbc, 0xbd, 0x22), QColor(0x17, 0xbe, 0xcf),
  };
  return k_palette;
}

/// Fixed-notation tick labels (6 decimals, trailing zeros stripped) so the
/// preview matches the main plots. Twin of pj_plotting's PlotScaleDraw, which
/// this module cannot link (module boundary) — keep the two in sync.
class PreviewScaleDraw : public QwtScaleDraw {
 public:
  [[nodiscard]] QwtText label(double value) const override {
    const QLocale locale;
    QString str = locale.toString(value, 'f', 6);
    const QString zero = locale.zeroDigit();
    const QString point = locale.decimalPoint();
    while (str.endsWith(zero)) {
      str.chop(zero.size());
    }
    if (str.endsWith(point)) {
      str.chop(point.size());
    }
    return str;
  }
};

sdk::MarkerKind previewMarkerKind(const std::string& kind) {
  if (kind == "region") {
    return sdk::MarkerKind::kRegion;
  }
  if (kind == "value_band") {
    return sdk::MarkerKind::kValueBand;
  }
  if (kind == "label") {
    return sdk::MarkerKind::kLabel;
  }
  return sdk::MarkerKind::kEvent;
}

class PreviewMarkersItem final : public QwtPlotItem {
 public:
  explicit PreviewMarkersItem(const std::vector<ChartPreviewWidget::Marker>& markers) {
    markers_.reserve(markers.size());
    for (const auto& marker : markers) {
      QColor color(QString::fromStdString(marker.color));
      if (!color.isValid()) {
        color = markerSeverityColor(sdk::MarkerSeverity::kInfo, {});
      }
      markers_.push_back({marker, previewMarkerKind(marker.kind), color});
    }
    setZ(40.0);
    setRenderHint(QwtPlotItem::RenderAntialiased, true);
  }

  void draw(
      QPainter* painter, const QwtScaleMap& xMap, const QwtScaleMap& yMap, const QRectF& canvasRect) const override {
    const QFontMetrics fm(painter->font());
    for (const auto& prepared : markers_) {
      const auto& m = prepared.marker;
      paintMarker(
          painter, xMap, yMap, canvasRect, fm,
          MarkerPaintData{
              .kind = prepared.kind,
              .x0 = m.x0,
              .x1 = m.x1,
              .y0 = m.y0,
              .y1 = m.y1,
              .has_value = m.has_value,
              .color = prepared.color,
              .label = m.label,
          });
    }
  }

 private:
  struct PreparedMarker {
    ChartPreviewWidget::Marker marker;
    sdk::MarkerKind kind;
    QColor color;
  };
  std::vector<PreparedMarker> markers_;
};
}  // namespace

ChartPreviewWidget::ChartPreviewWidget(QWidget* parent) : QwtPlot(parent) {
  // Theme from the APPLICATION palette, never the widget's: QStyleSheetStyle
  // rewrites widget palettes under QSS, mis-detecting the theme inside styled
  // plugin dialogs (Theme.cpp keeps the app palette's Window in lockstep).
  const auto fw_theme = theme::themeFor(QGuiApplication::palette().color(QPalette::Window).lightness() >= 128);
  setCanvasBackground(theme::surface(theme::Surface::DataBackdrop, fw_theme));
  setFrameStyle(QFrame::NoFrame);
  for (const int axis : {QwtPlot::yLeft, QwtPlot::yRight, QwtPlot::xBottom, QwtPlot::xTop}) {
    setAxisScaleDraw(axis, new PreviewScaleDraw);
  }
  if (auto* c = qobject_cast<QwtPlotCanvas*>(canvas())) {
    c->setFrameStyle(QFrame::NoFrame);
    c->setLineWidth(0);
  }

  // Bottom legend with checkable entries: clicking one toggles its curve's
  // visibility (mirrors the old Qt Charts interactive legend).
  auto* legend = new QwtLegend(this);
  legend->setDefaultItemMode(QwtLegendData::Checkable);
  insertLegend(legend, QwtPlot::BottomLegend);
  QObject::connect(legend, &QwtLegend::checked, this, [this](const QVariant& info, bool on, int) {
    if (auto* item = infoToItem(info)) {
      item->setVisible(on);
      replot();
    }
  });

  // Background grid (major dashed, minor dotted) — mirrors the main plot widget
  // (PlotWidgetBase). Shown only while interactive (toggled in setZoomEnabled).
  grid_ = new QwtPlotGrid();
  grid_->enableX(true);
  grid_->enableY(true);
  grid_->enableXMin(true);
  grid_->enableYMin(true);
  grid_->setMajorPen(QPen(QColor(150, 150, 150), 0.0, Qt::DashLine));
  grid_->setMinorPen(QPen(QColor(210, 210, 210), 0.0, Qt::DotLine));
  grid_->attach(this);
  grid_->setVisible(false);

  // Rubber-band zoom (left-drag to zoom in, right-click steps out). Disabled
  // until setZoomEnabled(true); wheel zoom is handled in eventFilter().
  zoomer_ = new QwtPlotZoomer(canvas());
  zoomer_->setEnabled(false);
  // The middle button pans here; Qwt binds it to "zoom back one step" on release, which
  // would undo a rubber-band zoom at the end of every middle-drag pan.
  zoomer_->setMousePattern(QwtEventPattern::MouseSelect3, Qt::NoButton);
  QObject::connect(zoomer_, &QwtPlotZoomer::zoomed, this, [this](const QRectF&) { emitViewChanged(); });

  // Pan (middle-drag, plus Ctrl+left-drag so it coexists with the left-drag zoomer) and
  // wheel zoom live in eventFilter().
  canvas()->installEventFilter(this);
}

void ChartPreviewWidget::setSeries(const std::vector<Series>& series) {
  detachItems(QwtPlotItem::Rtti_PlotCurve, /*autoDelete=*/true);

  double x_min = std::numeric_limits<double>::max();
  double x_max = std::numeric_limits<double>::lowest();
  double y_min = std::numeric_limits<double>::max();
  double y_max = std::numeric_limits<double>::lowest();

  const auto& palette = kDefaultPalette();

  for (size_t i = 0; i < series.size(); ++i) {
    const auto& s = series[i];
    auto* curve = new QwtPlotCurve(QString::fromStdString(s.label));
    curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);

    QVector<QPointF> points;
    points.reserve(static_cast<int>(s.points.size()));
    for (const auto& [x, y] : s.points) {
      points.append(QPointF(x, y));
      x_min = std::min(x_min, x);
      x_max = std::max(x_max, x);
      y_min = std::min(y_min, y);
      y_max = std::max(y_max, y);
    }
    curve->setSamples(points);

    // Color precedence: explicit hex (if valid) wins; otherwise a palette entry
    // chosen by series index (wraps modulo the palette size).
    QColor color;
    if (!s.color.empty()) {
      color = QColor(QString::fromStdString(s.color));
    }
    if (!color.isValid()) {
      color = palette[i % palette.size()];
    }
    QPen pen(color);
    pen.setWidthF(1.5);
    curve->setPen(pen);

    curve->attach(this);
  }

  // A constant or single-point axis gets a padded interval: skipping it would leave the
  // previous data's range in place and record that as the zoom-out limit. The pad scales
  // with the value so it stays above double resolution (e.g. nanosecond timestamps).
  const auto constant_pad = [](double v) { return std::max(1.0, std::abs(v) * 0.05); };
  if (x_min <= x_max) {
    const double pad = x_min == x_max ? constant_pad(x_min) : 0.0;
    setAxisScale(QwtPlot::xBottom, x_min - pad, x_max + pad);
  }
  if (y_min <= y_max) {
    const double margin = y_min == y_max ? constant_pad(y_min) : (y_max - y_min) * 0.05;
    setAxisScale(QwtPlot::yLeft, y_min - margin, y_max + margin);
  }

  replot();
  // Make the freshly-fit view the rubber-band zoom-out base. Block the zoomer's
  // signals so this programmatic reset doesn't emit a spurious viewChanged().
  {
    const QSignalBlocker blocker(zoomer_);
    zoomer_->setZoomBase(true);
  }
}

void ChartPreviewWidget::setMarkers(const std::vector<Marker>& markers) {
  for (auto* item : marker_items_) {
    item->detach();
    delete item;
  }
  marker_items_.clear();

  if (!markers.empty()) {
    auto* item = new PreviewMarkersItem(markers);
    item->attach(this);
    marker_items_.push_back(item);
  }

  replot();
}

void ChartPreviewWidget::clearSeries() {
  detachItems(QwtPlotItem::Rtti_PlotCurve, /*autoDelete=*/true);
  for (auto* item : marker_items_) {
    item->detach();
    delete item;
  }
  marker_items_.clear();
  replot();
}

void ChartPreviewWidget::setZoomEnabled(bool enabled) {
  if (enabled == zoom_enabled_) {
    return;  // idempotent: the panel re-declares this every tick
  }
  zoom_enabled_ = enabled;
  endPan();
  zoomer_->setEnabled(enabled);
  grid_->setVisible(enabled);
  replot();
}

bool ChartPreviewWidget::eventFilter(QObject* obj, QEvent* event) {
  if (!zoom_enabled_ || obj != canvas()) {
    return QwtPlot::eventFilter(obj, event);
  }
  switch (event->type()) {
    case QEvent::Wheel: {
      const auto* wheel = static_cast<QWheelEvent*>(event);
      const int delta = wheel->angleDelta().y();
      if (delta != 0) {
        const double scale = delta > 0 ? 0.8 : 1.25;  // wheel up zooms in (shrinks the interval)
        // Zooming out stops at the fitted view (the rubber-band zoom base); zooming in never
        // clamps, so a panned view keeps its place.
        const QRectF base = zoomer_->zoomBase();
        const int axes[2] = {QwtPlot::xBottom, QwtPlot::yLeft};
        for (int axis : axes) {
          const QwtScaleDiv& div = axisScaleDiv(axis);
          const double center = (div.lowerBound() + div.upperBound()) / 2.0;
          const double half = (div.upperBound() - div.lowerBound()) / 2.0 * scale;
          const double base_lo = axis == QwtPlot::xBottom ? base.left() : base.top();
          const double base_hi = axis == QwtPlot::xBottom ? base.right() : base.bottom();
          double lo = center - half;
          double hi = center + half;
          if (scale > 1.0 && base_hi > base_lo) {
            if (hi - lo >= base_hi - base_lo) {
              lo = base_lo;
              hi = base_hi;
            } else if (lo < base_lo) {
              hi += base_lo - lo;
              lo = base_lo;
            } else if (hi > base_hi) {
              lo -= hi - base_hi;
              hi = base_hi;
            }
          }
          setAxisScale(axis, lo, hi);
        }
        replot();
        emitViewChanged();
      }
      return true;
    }
    case QEvent::MouseButtonPress: {
      const auto* mouse = static_cast<QMouseEvent*>(event);
      const bool middle = mouse->button() == Qt::MiddleButton;
      const bool ctrl_left = mouse->button() == Qt::LeftButton && (mouse->modifiers() & Qt::ControlModifier);
      if (middle || ctrl_left) {
        panning_ = true;
        pan_last_pos_ = mouse->position().toPoint();
        canvas()->setCursor(Qt::ClosedHandCursor);
        return true;
      }
      break;
    }
    case QEvent::MouseMove: {
      if (panning_) {
        const QPoint pos = static_cast<QMouseEvent*>(event)->position().toPoint();
        panBy(pos.x() - pan_last_pos_.x(), pos.y() - pan_last_pos_.y());
        pan_last_pos_ = pos;
        return true;
      }
      break;
    }
    case QEvent::MouseButtonRelease: {
      if (panning_) {
        endPan();
        emitViewChanged();
        return true;
      }
      break;
    }
    default:
      break;
  }
  return QwtPlot::eventFilter(obj, event);
}

void ChartPreviewWidget::endPan() {
  if (panning_) {
    panning_ = false;
    canvas()->unsetCursor();
  }
}

void ChartPreviewWidget::panBy(int dx, int dy) {
  if (dx == 0 && dy == 0) {
    return;
  }
  const int axes[2] = {QwtPlot::xBottom, QwtPlot::yLeft};
  for (int axis : axes) {
    const QwtScaleMap map = canvasMap(axis);
    const int shift = axis == QwtPlot::xBottom ? dx : dy;
    const double lo = map.invTransform(map.transform(axisScaleDiv(axis).lowerBound()) - shift);
    const double hi = map.invTransform(map.transform(axisScaleDiv(axis).upperBound()) - shift);
    setAxisScale(axis, lo, hi);
  }
  replot();
}

void ChartPreviewWidget::emitViewChanged() {
  if (!zoom_enabled_) {
    return;
  }
  const QwtScaleDiv& xs = axisScaleDiv(QwtPlot::xBottom);
  const QwtScaleDiv& ys = axisScaleDiv(QwtPlot::yLeft);
  emit viewChanged(xs.lowerBound(), xs.upperBound(), ys.lowerBound(), ys.upperBound());
}

}  // namespace PJ
