// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotWidgetBase.h"

#include <qwt_axis.h>
#include <qwt_plot.h>
#include <qwt_plot_canvas.h>
#include <qwt_plot_curve.h>
#include <qwt_plot_grid.h>
#include <qwt_plot_layout.h>
#include <qwt_plot_marker.h>

#include "pj_plotting/PlotCurve.h"
#ifndef PJ_TARGET_WASM
#include <qwt_plot_opengl_canvas.h>
#else
#include "pj_plotting/PlotRhiCanvas.h"
#endif
#include <qwt_scale_engine.h>
#include <qwt_scale_map.h>
#include <qwt_scale_widget.h>
#include <qwt_symbol.h>

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPalette>
#include <QPen>
#include <QSettings>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

#include "pj_plotting/DatastoreCurveAdapter.h"
#include "pj_plotting/PlotLegend.h"
#include "pj_plotting/PlotMagnifier.h"
#include "pj_plotting/PlotPanner.h"
#include "pj_plotting/PlotScaleDraw.h"
#include "pj_plotting/PlotZoomer.h"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {
namespace {

[[nodiscard]] bool isUsableRect(const QRectF& rect) noexcept {
  return rect.width() >= 0.0 && rect.height() >= 0.0 && std::isfinite(rect.left()) && std::isfinite(rect.right()) &&
         std::isfinite(rect.top()) && std::isfinite(rect.bottom());
}

// The Data Backdrop fill for the active theme. Theme detection uses the synced
// application palette's Window lightness (Theme::syncApplicationPalette keeps it
// in lockstep with the theme tokens), so this must be read AFTER a theme applies.
[[nodiscard]] QColor dataBackdropForCurrentTheme() {
  const bool is_light = QGuiApplication::palette().color(QPalette::Window).lightness() >= 128;
  return PJ::theme::surface(PJ::theme::Surface::DataBackdrop, PJ::theme::themeFor(is_light));
}

[[nodiscard]] QColor colorFromIndex(int index) {
  static const std::array<QColor, 8> k_colors = {
      QColor("#1f77b4"), QColor("#d62728"), QColor("#1ac938"), QColor("#ff7f0e"),
      QColor("#f14cc1"), QColor("#9467bd"), QColor("#17becf"), QColor("#bcbd22"),
  };
  return k_colors[static_cast<std::size_t>(index) % k_colors.size()];
}

// Session-wide override for the OpenGL canvas choice, set by
// PlotWidgetBase::setOpenGlDisabledOverride (the --disable-opengl CLI flag).
// Read once per plot at construction; only written at startup.
bool g_opengl_disabled_override = false;

}  // namespace

double lineWidthValue(LineWidth line_width) noexcept {
  constexpr std::array<double, 4> kLineWidths = {1.0, 1.5, 2.0, 3.0};
  return 1.4 * kLineWidths[static_cast<std::size_t>(line_width)];
}

double dotWidthValue(LineWidth line_width) noexcept {
  return (lineWidthValue(line_width) * 1.5) + 1.0;
}

class PlotWidgetBase::QwtPlotPimpl : public QwtPlot {
 public:
  QwtPlotPimpl(
      PlotWidgetBase* parent_widget, QWidget* canvas_widget, std::function<void(const QRectF&)> resized_callback,
      std::function<void(QEvent*)> event_callback)
      : QwtPlot(nullptr),
        resized_callback(std::move(resized_callback)),
        event_callback(std::move(event_callback)),
        parent(parent_widget) {
    setCanvas(canvas_widget);
    for (const int axis : {QwtPlot::yLeft, QwtPlot::yRight, QwtPlot::xBottom, QwtPlot::xTop}) {
      setAxisScaleDraw(axis, new PlotScaleDraw);
    }
    const auto fw_theme = theme::appTheme();
    legend = new PlotLegend(this);
    grid = new QwtPlotGrid();
    grid->enableX(false);
    grid->enableXMin(false);
    grid->enableY(false);
    grid->enableYMin(false);
    grid->setMajorPen(
        QPen(theme::outline(theme::OutlineRole::Gridline, theme::OutlineState::Rest, fw_theme), 0.0, Qt::DashLine));
    grid->setMinorPen(
        QPen(theme::outline(theme::OutlineRole::Gridline, theme::OutlineState::Rest, fw_theme), 0.0, Qt::DotLine));
    grid->attach(this);
    magnifier = new PlotMagnifier(canvas_widget);
    panner1 = new PlotPanner(canvas_widget);
    panner2 = new PlotPanner(canvas_widget);
    zoomer = new PlotZoomer(canvas_widget);

    zoomer->setRubberBandPen(
        QPen(theme::outline(theme::OutlineRole::Interactive, theme::OutlineState::Focused, fw_theme), 1, Qt::DotLine));
    zoomer->setTrackerPen(
        QPen(theme::onSurface(theme::Surface::DataBackdrop, theme::Emphasis::Default, fw_theme), 1, Qt::DotLine));
    zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::NoModifier);

    magnifier->setAxisEnabled(QwtPlot::xTop, false);
    magnifier->setAxisEnabled(QwtPlot::yRight, false);
    // Plots default to time-series (xy_mode_ == false); enable the ≥2 ns X zoom
    // floor up front so a plot that never calls setModeXY is still clamped.
    magnifier->setTimeXAxis(true);
    magnifier->setZoomInKey(Qt::Key_Plus, Qt::ControlModifier);
    magnifier->setZoomOutKey(Qt::Key_Minus, Qt::ControlModifier);
    magnifier->setMouseButton(Qt::NoButton);

    panner1->setMouseButton(Qt::LeftButton, Qt::ControlModifier);
    panner2->setMouseButton(Qt::MiddleButton, Qt::NoModifier);

    connect(zoomer, &PlotZoomer::zoomed, this, [this](const QRectF& rect) { this->resized_callback(rect); });
    // No replot here: PlotMagnifier::rescale() replots before emitting so the
    // canvas maps are already committed when this callback runs.
    connect(magnifier, &PlotMagnifier::rescaled, this, [this](const QRectF& rect) { this->resized_callback(rect); });
    connect(panner1, &PlotPanner::rescaled, this, [this](const QRectF& rect) { this->resized_callback(rect); });
    connect(panner2, &PlotPanner::rescaled, this, [this](const QRectF& rect) { this->resized_callback(rect); });

    axisWidget(QwtPlot::xBottom)->installEventFilter(parent);
    axisWidget(QwtPlot::yLeft)->installEventFilter(parent);
    canvas()->installEventFilter(parent);
  }

  ~QwtPlotPimpl() override {
    axisWidget(QwtPlot::xBottom)->removeEventFilter(parent);
    axisWidget(QwtPlot::yLeft)->removeEventFilter(parent);
    canvas()->removeEventFilter(parent);
    setCanvas(nullptr);
  }

  [[nodiscard]] QRectF canvasBoundingRect() const {
    QRectF rect;
    rect.setBottom(canvasMap(QwtPlot::yLeft).s1());
    rect.setTop(canvasMap(QwtPlot::yLeft).s2());
    rect.setLeft(canvasMap(QwtPlot::xBottom).s1());
    rect.setRight(canvasMap(QwtPlot::xBottom).s2());
    return rect;
  }

  void resizeEvent(QResizeEvent* event) override {
    QwtPlot::resizeEvent(event);
    // Window geometry is not a viewport gesture. XY plots still need their
    // aspect ratio corrected, but that correction must not create history.
    if (parent->isXYPlot() && parent->keepRatioXY()) {
      parent->applyRectKeepingRatio(canvasBoundingRect());
      replot();
    }
  }

  void dragEnterEvent(QDragEnterEvent* event) override {
    event_callback(event);
  }
  void dragLeaveEvent(QDragLeaveEvent* event) override {
    event_callback(event);
  }
  void dropEvent(QDropEvent* event) override {
    event_callback(event);
  }

  PlotLegend* legend = nullptr;
  QwtPlotGrid* grid = nullptr;
  PlotMagnifier* magnifier = nullptr;
  PlotPanner* panner1 = nullptr;
  PlotPanner* panner2 = nullptr;
  PlotZoomer* zoomer = nullptr;
  std::function<void(const QRectF&)> resized_callback;
  std::function<void(QEvent*)> event_callback;
  PlotWidgetBase* parent = nullptr;
  std::list<CurveInfo> curve_list;
  std::optional<CurveStyle> overridden_curve_style;
  CurveStyle default_curve_style = kLines;
  bool zoom_enabled = true;
};

PlotWidgetBase::PlotWidgetBase(QWidget* parent) : QWidget(parent) {
  auto on_view_resized = [this](const QRectF& rect) { emit viewResized(rect); };
  auto on_event = [this](QEvent* event) {
    if (auto* drag_enter = dynamic_cast<QDragEnterEvent*>(event)) {
      emit dragEnterSignal(drag_enter);
    } else if (auto* drag_leave = dynamic_cast<QDragLeaveEvent*>(event)) {
      emit dragLeaveSignal(drag_leave);
    } else if (auto* drop = dynamic_cast<QDropEvent*>(event)) {
      emit dropSignal(drop);
    }
  };

  // QwtPlotCanvas uses a backing-store paint path that ignores QSS background
  // rules, so the canvas needs a solid palette colour. The Main Plot is the
  // Data Backdrop surface (ui_framework.md § Data Backdrop); read it from the
  // framework for the current theme. changeEvent() re-applies it when the theme
  // is applied after construction or toggled at runtime.
  const QColor canvas_bg = dataBackdropForCurrentTheme();

  QWidget* abs_canvas = nullptr;
#ifdef PJ_TARGET_WASM
  // QOpenGLWidget cannot provide its context-sharing contract on WebGL. Keep
  // the existing Qwt canvas and preferences byte-for-byte on desktop, while
  // the browser submits plot geometry through QRhi's OpenGL/WebGL backend.
  auto* rhi_canvas = new PlotRhiCanvas();
  rhi_canvas->setPalette(canvas_bg);
  abs_canvas = rhi_canvas;
#else
  const bool use_opengl = !g_opengl_disabled_override && QSettings().value("Preferences::use_opengl", true).toBool();
  if (use_opengl) {
    auto* canvas = new QwtPlotOpenGLCanvas();
    // Drop Qwt's own backing-store FBO: it is a persistent-content GL resource
    // that outlives context recreation (ADS float/re-dock) and GPU resets, and
    // QOpenGLWidget's internal FBO already restores content on re-expose. Its
    // only saving was skipping a redraw on rare non-replot repaints (focus
    // activation); without it every canvas paint re-renders from live state.
    canvas->setPaintAttribute(QwtPlotOpenGLCanvas::BackingStore, false);
    canvas->setFrameStyle(QFrame::Box | QFrame::Plain);
    canvas->setLineWidth(1);
    canvas->setPalette(canvas_bg);
    abs_canvas = canvas;
  } else {
    auto* canvas = new QwtPlotCanvas();
    canvas->setFrameStyle(QFrame::Box | QFrame::Plain);
    canvas->setLineWidth(1);
    canvas->setPalette(canvas_bg);
    canvas->setPaintAttribute(QwtPlotCanvas::BackingStore, true);
    abs_canvas = canvas;
  }
#endif
  abs_canvas->setObjectName("qwtCanvas");

  plot_ = new QwtPlotPimpl(this, abs_canvas, on_view_resized, on_event);
#ifdef PJ_TARGET_WASM
  // The canvas is constructed before the plot exists (QwtPlot adopts its canvas
  // during its own ctor), so bind the source plot now instead of resolving it
  // from parentWidget() on every frame.
  rhi_canvas->setPlot(plot_);
#endif

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(
      PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None), PJ::theme::space(theme::Space::None),
      PJ::theme::space(theme::Space::None));
  layout->addWidget(plot_);

  // Charts fill their container edge-to-edge; breathing room lives INSIDE the
  // chart. QwtPlot lays out within contentsRect() and the QSS `QwtPlot` rule
  // paints the Data Backdrop across the full widget rect, so this margin reads
  // as chart surface — never as the dock backdrop ringing the plot.
  const int chart_pad = PJ::theme::space(theme::Space::Comfortable);
  plot_->setContentsMargins(chart_pad, chart_pad, chart_pad, chart_pad);

  plot_->setMinimumSize(100, 100);
  plot_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  plot_->canvas()->setMouseTracking(true);
  // Opaque background: a transparent canvas forces a per-paint alpha-composite of the whole
  // canvas (slow in software raster) instead of a fast opaque blit. canvas_bg matches the palette set above.
  plot_->setCanvasBackground(canvas_bg);
  plot_->setAxisAutoScale(QwtPlot::yLeft, true);
  plot_->setAxisAutoScale(QwtPlot::xBottom, true);
  plot_->axisScaleEngine(QwtPlot::xBottom)->setAttribute(QwtScaleEngine::Floating, true);
  plot_->plotLayout()->setAlignCanvasToScales(true);
  plot_->setAxisScale(QwtPlot::xBottom, 0.0, 1.0);
  plot_->setAxisScale(QwtPlot::yLeft, 0.0, 1.0);
}

PlotWidgetBase::~PlotWidgetBase() {
  delete plot_;
  plot_ = nullptr;
}

void PlotWidgetBase::refreshCanvasBackground() {
  QwtPlot* plot = qwtPlot();
  QWidget* canvas = (plot != nullptr) ? plot->canvas() : nullptr;
  if (canvas == nullptr) {
    return;
  }
  canvas->setPalette(dataBackdropForCurrentTheme());
  canvas->update();
}

void PlotWidgetBase::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  // QGuiApplication::setPalette (Theme::syncApplicationPalette) delivers
  // ApplicationPaletteChange to every widget; re-derive the Data Backdrop then.
  if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange) {
    refreshCanvasBackground();
  }
}

PlotWidgetBase::CurveInfo* PlotWidgetBase::addCurve(
    const QString& name, QwtSeriesData<QPointF>* series, QColor color, const QString& display_name) {
  if (series == nullptr || curveFromTitle(name) != nullptr) {
    delete series;
    return nullptr;
  }

  auto* curve = new PlotCurve(display_name.isEmpty() ? name : display_name);
  curve->setPaintAttribute(QwtPlotCurve::ClipPolygons, true);
  curve->setPaintAttribute(QwtPlotCurve::FilterPointsAggressive, true);
  curve->setData(series);
  curve->setPen(color == Qt::transparent ? nextColor() : color);
  setStyle(curve, curveStyle());
  curve->setRenderHint(QwtPlotItem::RenderAntialiased, true);
  curve->attach(qwtPlot());

  // Per-curve tracker marker: the dot that rides an XY curve at the playback
  // cursor (PlotWidget's XY tracker moves + shows it). Fill matches the curve so
  // multi-curve XY plots stay legible (all markers show at once — a single
  // shared color would make them unattributable); hidden until the XY tracker
  // reveals it. The outline uses the data-backdrop ink so the dot separates
  // from the identically-colored curve underneath.
  auto* marker = new QwtPlotMarker;
  marker->attach(qwtPlot());
  marker->setVisible(false);
  const auto fw_theme = theme::appTheme();
  const QColor marker_fill = curve->pen().color();
  marker->setSymbol(new QwtSymbol(
      QwtSymbol::Ellipse, marker_fill,
      QPen(theme::onSurface(theme::Surface::DataBackdrop, theme::Emphasis::Default, fw_theme)), QSize(8, 8)));

  plot_->curve_list.push_back(CurveInfo{.source_name = name, .curve = curve, .marker = marker});
  emit curveListChanged();
  return &plot_->curve_list.back();
}

void PlotWidgetBase::removeCurve(const QString& title) {
  auto it = std::find_if(plot_->curve_list.begin(), plot_->curve_list.end(), [&title](const CurveInfo& info) {
    return info.curve != nullptr && (info.curve->title().text() == title || info.source_name == title);
  });
  if (it == plot_->curve_list.end()) {
    return;
  }

  it->curve->detach();
  delete it->curve;
  it->marker->detach();
  delete it->marker;
  plot_->curve_list.erase(it);
  emit curveListChanged();
}

const std::list<PlotWidgetBase::CurveInfo>& PlotWidgetBase::curveList() const noexcept {
  return plot_->curve_list;
}

std::list<PlotWidgetBase::CurveInfo>& PlotWidgetBase::curveList() noexcept {
  return plot_->curve_list;
}

bool PlotWidgetBase::isEmpty() const noexcept {
  return plot_->curve_list.empty();
}

std::map<QString, QColor> PlotWidgetBase::curveColors() const {
  std::map<QString, QColor> colors;
  for (const auto& info : plot_->curve_list) {
    // Key by source_name (the catalog key), not the display title, so consumers can
    // match against CurveDescriptor::name.
    colors.insert({info.source_name, info.curve->pen().color()});
  }
  return colors;
}

PlotWidgetBase::CurveInfo* PlotWidgetBase::curveFromTitle(const QString& title) {
  for (auto& info : plot_->curve_list) {
    if (info.curve->title().text() == title || info.source_name == title) {
      return &info;
    }
  }
  return nullptr;
}

void PlotWidgetBase::resetZoom() {
  updateMaximumZoomArea();
  applyRectToAxes(maxZoomRect());
  replot();
}

void PlotWidgetBase::applyRectToAxes(const QRectF& rect) {
  plot_->setAxisScale(QwtPlot::yLeft, std::min(rect.bottom(), rect.top()), std::max(rect.bottom(), rect.top()));
  plot_->setAxisScale(QwtPlot::xBottom, std::min(rect.left(), rect.right()), std::max(rect.left(), rect.right()));
  plot_->updateAxes();
}

Range<double> PlotWidgetBase::getVisualizationRangeX() const {
  double left = std::numeric_limits<double>::max();
  double right = std::numeric_limits<double>::lowest();

  for (const auto& info : curveList()) {
    if (info.curve == nullptr || !info.curve->isVisible() || info.curve->data() == nullptr) {
      continue;
    }
    const QRectF rect = info.curve->data()->boundingRect();
    if (!isUsableRect(rect)) {
      continue;
    }
    left = std::min(left, rect.left());
    right = std::max(right, rect.right());
  }

  if (left > right) {
    left = 0.0;
    right = 0.0;
  }

  if (isXYPlot() && std::abs(right - left) > std::numeric_limits<double>::epsilon()) {
    const double margin = (right - left) * 0.025;
    left -= margin;
    right += margin;
  }
  return Range<double>{.min = left, .max = right};
}

Range<double> PlotWidgetBase::getVisualizationRangeY(Range<double> range_x) const {
  double bottom = std::numeric_limits<double>::max();
  double top = std::numeric_limits<double>::lowest();

  for (const auto& info : curveList()) {
    if (info.curve == nullptr || !info.curve->isVisible() || info.curve->data() == nullptr) {
      continue;
    }

    if (const auto* adapter = dynamic_cast<const DatastoreCurveAdapter*>(info.curve->data())) {
      const auto y_range = adapter->visibleYRange(range_x);
      if (y_range.has_value()) {
        bottom = std::min(bottom, y_range->min);
        top = std::max(top, y_range->max);
      }
      continue;
    }

    const QRectF rect = info.curve->data()->boundingRect();
    if (!isUsableRect(rect)) {
      continue;
    }
    bottom = std::min(bottom, rect.top());
    top = std::max(top, rect.bottom());
  }

  if (bottom > top) {
    bottom = -1.0;
    top = 1.0;
  }

  // A curve (or set of curves) whose samples all share one value collapses to a
  // zero-height range, which Qwt renders as an unreadable flat axis. The 2.5%
  // proportional margin is 0 in that case, so fall back to a fixed ±0.1 pad that
  // centers the constant value with visible headroom.
  double margin = (top - bottom) * 0.025;
  if (margin == 0.0) {
    margin = 0.1;
  }
  return Range<double>{.min = bottom - margin, .max = top + margin};
}

void PlotWidgetBase::setModeXY(bool enable) {
  // XY (scatter) plots use the plot-level curve style like any other plot — the
  // Curve Style toolbar controls them; the mode is not tied to a forced style.
  xy_mode_ = enable;
  // A time-series plot's X is absolute time; enable the magnifier's ≥2 ns zoom
  // floor for it. An XY plot's X is a data value with no ns quantization, so no
  // clamp there.
  if (plot_ != nullptr && plot_->magnifier != nullptr) {
    plot_->magnifier->setTimeXAxis(!enable);
  }
}

bool PlotWidgetBase::isXYPlot() const noexcept {
  return xy_mode_;
}

void PlotWidgetBase::setLegendSize(int size) {
  QFont font = plot_->legend->font();
  font.setPointSize(size);
  plot_->legend->setFont(font);
  replot();
}

void PlotWidgetBase::setLegendAlignment(Qt::Alignment alignment) {
  plot_->legend->setAlignmentInCanvas(alignment);
#ifdef PJ_TARGET_WASM
  // The retained QRhi buffers need an explicit dirty signal after the item
  // mutation. Preserve the native Qwt repaint behavior exactly as before.
  replot();
#endif
}

void PlotWidgetBase::setLegendVisible(bool visible) {
  plot_->legend->setVisible(visible);
  replot();
}

bool PlotWidgetBase::legendVisible() const noexcept {
  return plot_->legend->isVisible();
}

void PlotWidgetBase::setGridVisible(bool visible) {
  plot_->grid->enableX(visible);
  plot_->grid->enableXMin(visible);
  plot_->grid->enableY(visible);
  plot_->grid->enableYMin(visible);
  replot();
}

bool PlotWidgetBase::gridVisible() const noexcept {
  return plot_->grid->xEnabled();
}

void PlotWidgetBase::setCanvasAlignedToScales(bool aligned) {
  plot_->plotLayout()->setAlignCanvasToScales(aligned);
  // The internal chart padding belongs to the default (dock-hosted) mode only;
  // embedded charts sit flush against the surrounding chrome.
  const int chart_pad = aligned ? PJ::theme::space(theme::Space::Comfortable) : PJ::theme::space(theme::Space::None);
  plot_->setContentsMargins(chart_pad, chart_pad, chart_pad, chart_pad);
  plot_->updateLayout();
}

void PlotWidgetBase::setZoomEnabled(bool enabled) {
  plot_->zoom_enabled = enabled;
  plot_->zoomer->setEnabled(enabled);
  plot_->magnifier->setEnabled(enabled);
  plot_->panner1->setEnabled(enabled);
  plot_->panner2->setEnabled(enabled);
}

bool PlotWidgetBase::isZoomEnabled() const noexcept {
  return plot_->zoom_enabled;
}

void PlotWidgetBase::setSwapZoomPan(bool swapped) {
  if (swapped) {
    plot_->zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::ControlModifier);
    plot_->panner1->setMouseButton(Qt::LeftButton, Qt::NoModifier);
  } else {
    plot_->zoomer->setMousePattern(QwtEventPattern::MouseSelect1, Qt::LeftButton, Qt::NoModifier);
    plot_->panner1->setMouseButton(Qt::LeftButton, Qt::ControlModifier);
  }
}

QRectF PlotWidgetBase::currentBoundingRect() const {
  return plot_->canvasBoundingRect();
}

QRectF PlotWidgetBase::maxZoomRect() const noexcept {
  return max_zoom_rect_;
}

bool PlotWidgetBase::keepRatioXY() const noexcept {
  return keep_aspect_ratio_;
}

void PlotWidgetBase::setKeepRatioXY(bool active) {
  keep_aspect_ratio_ = active;
  plot_->zoomer->keepAspectRatio(isXYPlot() && active);
  if (!isXYPlot()) {
    return;
  }
  // Reshape current view; otherwise the toggle only takes effect on the next
  // drag-zoom. OFF re-fits to data bounds (loses zoom by design).
  if (active) {
    applyRectKeepingRatio(currentBoundingRect());
    replot();
  } else {
    // resetZoom() refreshes max bounds first: data may have changed since the
    // last reset, so the cached max_zoom_rect_ could be stale.
    resetZoom();
  }
}

void PlotWidgetBase::applyRectKeepingRatio(QRectF rect) {
  if (isXYPlot() && keep_aspect_ratio_) {
    plot_->zoomer->applyKeepAspectRatio(rect);
  }
  applyRectToAxes(rect);
}

void PlotWidgetBase::setAcceptDrops(bool accept) {
  plot_->setAcceptDrops(accept);
  plot_->canvas()->setAcceptDrops(accept);
}

void PlotWidgetBase::overrideCurvesStyle(std::optional<CurveStyle> style) {
  if (plot_->overridden_curve_style == style) {
    return;
  }
  plot_->overridden_curve_style = style;
  updateCurvesStyle();
}

std::optional<PlotWidgetBase::CurveStyle> PlotWidgetBase::overriddenCurvesStyle() const noexcept {
  return plot_->overridden_curve_style;
}

void PlotWidgetBase::setDefaultStyle(CurveStyle default_style) {
  plot_->default_curve_style = default_style;
  updateCurvesStyle();
}

PlotWidgetBase::CurveStyle PlotWidgetBase::defaultCurveStyle() const noexcept {
  return plot_->default_curve_style;
}

PlotWidgetBase::CurveStyle PlotWidgetBase::curveStyle() const noexcept {
  return plot_->overridden_curve_style.value_or(plot_->default_curve_style);
}

void PlotWidgetBase::updateCurvesStyle() {
  for (auto& info : plot_->curve_list) {
    setStyle(info.curve, curveStyle());
  }
  replot();
}

void PlotWidgetBase::setLineWidth(LineWidth width) {
  line_width_ = width;
  // Re-apply the current style at the new width so every curve gets the
  // style-correct pen (Lines use lineWidthValue, Dots use the larger
  // dotWidthValue, Lines+Dots symbols are resized) — the same treatment a new
  // curve gets from addCurve(). A plain pen-width loop would miss that.
  updateCurvesStyle();
}

void PlotWidgetBase::replot() {
  if (plot_->zoomer != nullptr) {
    plot_->zoomer->setZoomBase(false);
  }
  plot_->replot();
}

void PlotWidgetBase::removeAllCurves() {
  for (auto& info : plot_->curve_list) {
    info.curve->detach();
    delete info.curve;
    info.marker->detach();
    delete info.marker;
  }
  plot_->curve_list.clear();
  emit curveListChanged();
  replot();
}

void PlotWidgetBase::setStyle(QwtPlotCurve* curve, CurveStyle style) {
  const double width = style == kDots ? dotWidthValue(lineWidth()) : lineWidthValue(lineWidth());
  curve->setPen(curve->pen().color(), width);
  applyStyleToCurve(curve, style);
}

void PlotWidgetBase::applyStyleToCurve(QwtPlotCurve* curve, CurveStyle style) {
  // kLinesAndDots draws plain Lines plus dots at each sample. On desktop the
  // dots are PlotCurve's batched drawDots pass; on WASM PlotRhiCanvas builds
  // its dot geometry from an explicit symbol instead (it never paints
  // per-point through QPainter, so the symbol path is harmless there).
  if (auto* plot_curve = dynamic_cast<PlotCurve*>(curve); plot_curve != nullptr) {
    plot_curve->setDotWidth(0.0);
  }
  switch (style) {
    case kLines:
      curve->setStyle(QwtPlotCurve::Lines);
      curve->setSymbol(nullptr);
      break;
    case kLinesAndDots: {
      curve->setStyle(QwtPlotCurve::Lines);
#ifdef PJ_TARGET_WASM
      const QColor color = curve->pen().color();
      const int dot_size = static_cast<int>(std::round(dotWidthValue(lineWidth())));
      curve->setSymbol(new QwtSymbol(QwtSymbol::Ellipse, color, QPen(color), QSize(dot_size, dot_size)));
#else
      if (auto* plot_curve = dynamic_cast<PlotCurve*>(curve); plot_curve != nullptr) {
        plot_curve->setDotWidth(dotWidthValue(lineWidth()));
      }
      curve->setSymbol(nullptr);
#endif
      break;
    }
    case kDots:
      curve->setStyle(QwtPlotCurve::Dots);
      curve->setSymbol(nullptr);
      break;
    case kSticks:
      curve->setStyle(QwtPlotCurve::Sticks);
      curve->setSymbol(nullptr);
      break;
    case kSteps:
      curve->setStyle(QwtPlotCurve::Steps);
      curve->setCurveAttribute(QwtPlotCurve::Inverted, false);
      curve->setSymbol(nullptr);
      break;
    case kStepsInverted:
      curve->setStyle(QwtPlotCurve::Steps);
      curve->setCurveAttribute(QwtPlotCurve::Inverted, true);
      curve->setSymbol(nullptr);
      break;
  }
}

QColor PlotWidgetBase::nextColor() {
  return colorFromIndex(next_color_index_++);
}

QColor PlotWidgetBase::paletteColor(int index) {
  return colorFromIndex(index);
}

void PlotWidgetBase::setOpenGlDisabledOverride(bool disabled) {
  g_opengl_disabled_override = disabled;
}

QWidget* PlotWidgetBase::canvasWidget() const {
  return plot_ != nullptr ? plot_->canvas() : nullptr;
}

QwtPlot* PlotWidgetBase::qwtPlot() {
  return plot_;
}

void PlotWidgetBase::installHoverFilter(QObject* filter) {
  if (filter == nullptr || plot_ == nullptr) {
    return;
  }
  if (QWidget* canvas = plot_->canvas(); canvas != nullptr) {
    canvas->installEventFilter(filter);
  }
  for (int axis : {QwtAxis::YLeft, QwtAxis::YRight, QwtAxis::XBottom, QwtAxis::XTop}) {
    if (plot_->isAxisVisible(axis)) {
      if (auto* widget = plot_->axisWidget(axis); widget != nullptr) {
        widget->installEventFilter(filter);
      }
    }
  }
}

const QwtPlot* PlotWidgetBase::qwtPlot() const {
  return plot_;
}

PlotLegend* PlotWidgetBase::legend() {
  return plot_->legend;
}

PlotZoomer* PlotWidgetBase::zoomer() {
  return plot_->zoomer;
}

PlotMagnifier* PlotWidgetBase::magnifier() {
  return plot_->magnifier;
}

PlotPanner* PlotWidgetBase::panner1() {
  return plot_->panner1;
}

PlotPanner* PlotWidgetBase::panner2() {
  return plot_->panner2;
}

void PlotWidgetBase::updateMaximumZoomArea() {
  QRectF max_rect;
  const Range<double> range_x = getVisualizationRangeX();
  max_rect.setLeft(range_x.min);
  max_rect.setRight(range_x.max);

  const Range<double> range_y = getVisualizationRangeY(range_x);
  max_rect.setBottom(range_y.min);
  max_rect.setTop(range_y.max);

  if (isXYPlot() && keep_aspect_ratio_) {
    const QRectF canvas_rect = plot_->canvas()->contentsRect();
    const double canvas_ratio = std::abs(canvas_rect.width() / canvas_rect.height());
    const double data_ratio = std::abs(max_rect.width() / max_rect.height());
    if (data_ratio < canvas_ratio) {
      const double new_width = std::abs(max_rect.height() * canvas_ratio);
      const double increment = new_width - max_rect.width();
      max_rect.setWidth(new_width);
      max_rect.moveLeft(max_rect.left() - 0.5 * increment);
    } else {
      const double new_height = -(max_rect.width() / canvas_ratio);
      const double increment = std::abs(new_height - max_rect.height());
      max_rect.setHeight(new_height);
      max_rect.moveTop(max_rect.top() + 0.5 * increment);
    }
  }

  magnifier()->setAxisLimits(QwtPlot::xBottom, max_rect.left(), max_rect.right());
  magnifier()->setAxisLimits(QwtPlot::yLeft, max_rect.bottom(), max_rect.top());
  zoomer()->keepAspectRatio(isXYPlot() && keep_aspect_ratio_);
  max_zoom_rect_ = max_rect;
}

bool PlotWidgetBase::eventFilter(QObject* obj, QEvent* event) {
  if (event->type() == QEvent::Destroy) {
    return false;
  }

  QwtScaleWidget* bottom_axis = plot_->axisWidget(QwtPlot::xBottom);
  QwtScaleWidget* left_axis = plot_->axisWidget(QwtPlot::yLeft);

  if ((obj == bottom_axis || obj == left_axis) && !(isXYPlot() && keepRatioXY()) && event->type() == QEvent::Wheel) {
    auto* wheel_event = static_cast<QWheelEvent*>(event);
    magnifier()->setDefaultMode(obj == bottom_axis ? PlotMagnifier::kXAxis : PlotMagnifier::kYAxis);
    magnifier()->widgetWheelEvent(wheel_event);
  }

  if (obj != plot_->canvas()) {
    return false;
  }

  if (event->type() == QEvent::DragEnter) {
    emit dragEnterSignal(static_cast<QDragEnterEvent*>(event));
    return false;
  }

  if (event->type() == QEvent::DragLeave) {
    emit dragLeaveSignal(static_cast<QDragLeaveEvent*>(event));
    return false;
  }

  if (event->type() == QEvent::Drop) {
    emit dropSignal(static_cast<QDropEvent*>(event));
    return false;
  }

  if (event->type() == QEvent::Wheel) {
    auto* wheel_event = static_cast<QWheelEvent*>(event);
    magnifier()->setDefaultMode(PlotMagnifier::kBothAxes);

    const bool ctrl_modifier = wheel_event->modifiers() == Qt::ControlModifier;
    const QRectF legend_rect = legend()->geometry(plot_->canvas()->rect());
    if (ctrl_modifier && legend()->isVisible() && legend_rect.contains(wheel_event->position())) {
      const int previous_size = legend()->font().pointSize();
      int new_size = previous_size;
      if (wheel_event->angleDelta().y() > 0) {
        new_size = std::min(13, previous_size + 1);
      } else if (wheel_event->angleDelta().y() < 0) {
        new_size = std::max(7, previous_size - 1);
      }
      if (new_size != previous_size) {
        setLegendSize(new_size);
        emit legendSizeChanged(new_size);
      }
      return true;
    }
    return false;
  }

  if (event->type() == QEvent::MouseButtonPress) {
    auto* mouse_event = static_cast<QMouseEvent*>(event);
    if (mouse_event->button() == Qt::LeftButton && mouse_event->modifiers() == Qt::NoModifier) {
      const QwtPlotItem* clicked_item = legend()->processMousePressEvent(mouse_event);
      if (clicked_item == nullptr) {
        return false;
      }
      for (auto& info : curveList()) {
        if (clicked_item == info.curve) {
          info.curve->setVisible(!info.curve->isVisible());
          resetZoom();
          replot();
          return true;
        }
      }
    }
  }

  return false;
}

}  // namespace PJ
