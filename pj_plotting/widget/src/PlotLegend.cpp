// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotLegend.h"

#include <qwt_graphic.h>
#include <qwt_legend_data.h>
#include <qwt_plot_curve.h>
#include <qwt_text.h>

#include <QByteArray>
#include <QMarginsF>
#include <QMouseEvent>
#include <QPainter>
#include <QString>
#include <cmath>
#include <unordered_map>
using namespace Qt::StringLiterals;

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {

// Opt-in diagnostics for the intermittent "legend/canvas text disappears"
// bug: set PJ_PLOT_TEXT_DEBUG=1 to have every legend entry report, on any
// state change, which of the four failure modes (if any) is active. When the
// text vanishes while this logs "EMPTY=0 CLIPPED=0 LOWCONTRAST=0" with a valid
// title/colour, the cause is a canvas text-rendering failure, not legend
// logic. Change-detected (not per-frame) so it does not spam the 60 Hz replot.
bool plotTextDebugEnabled() {
  static const bool enabled = qEnvironmentVariableIsSet("PJ_PLOT_TEXT_DEBUG");
  return enabled;
}

double relativeLuminance(const QColor& c) {
  return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF();
}

void logLegendEntry(
    const void* plot, const QString& title, bool empty, const QRectF& item_rect, int title_offset,
    const QColor& text_color, const QColor& canvas_bg) {
  if (!plotTextDebugEnabled()) {
    return;
  }
  const double text_avail = item_rect.width() - title_offset;
  const bool clipped = text_avail <= 2.0;
  const bool low_contrast =
      text_color.isValid() && std::abs(relativeLuminance(text_color) - relativeLuminance(canvas_bg)) < 0.12;

  const QString sig = u"EMPTY=%1 CLIPPED=%2(avail=%3) LOWCONTRAST=%4 text=%5 bg=%6"_s.arg(empty)
                          .arg(clipped)
                          .arg(text_avail, 0, 'f', 1)
                          .arg(low_contrast)
                          .arg(text_color.isValid() ? text_color.name() : u"(none)"_s, canvas_bg.name());

  static std::unordered_map<QString, QString> last_sig;
  const QString key = u"%1|%2"_s.arg(reinterpret_cast<quintptr>(plot)).arg(title);
  auto it = last_sig.find(key);
  if (it != last_sig.end() && it->second == sig) {
    return;  // unchanged since last paint — stay quiet
  }
  last_sig[key] = sig;
  qWarning(
      "[PJ_PLOT_TEXT_DEBUG] legend '%s' rect=%.0fx%.0f off=%d :: %s", qUtf8Printable(title), item_rect.width(),
      item_rect.height(), title_offset, qUtf8Printable(sig));
}

}  // namespace

PlotLegend::PlotLegend(QwtPlot* parent) : parent_plot_(parent) {
  setRenderHint(QwtPlotItem::RenderAntialiased);
  setMaxColumns(1);
  setAlignmentInCanvas(Qt::Alignment(Qt::AlignTop | Qt::AlignRight));
  setBackgroundMode(QwtPlotLegendItem::BackgroundMode::LegendBackground);
  setBorderRadius(theme::radius(theme::Radius::Square));
  setMargin(2);
  setSpacing(1);
  setItemMargin(2);

  QFont legend_font = font();
  legend_font.setPointSize(9);
  setFont(legend_font);
  setVisible(true);
  attach(parent);
}

QRectF PlotLegend::hideButtonRect() const {
  constexpr int kSize = 5;
  const QRect canvas_rect = parent_plot_->canvas()->rect();
  if (alignmentInCanvas() & Qt::AlignRight) {
    return QRectF(geometry(canvas_rect).topRight() + QPoint(-kSize, -kSize), QSize(kSize * 2, kSize * 2));
  }
  return QRectF(geometry(canvas_rect).topLeft() + QPoint(-kSize, -kSize), QSize(kSize * 2, kSize * 2));
}

void PlotLegend::draw(QPainter* painter, const QwtScaleMap& x_map, const QwtScaleMap& y_map, const QRectF& rect) const {
  if (!collapsed_) {
    QwtPlotLegendItem::draw(painter, x_map, y_map, rect);
  }

  QRectF icon_rect = hideButtonRect();
  if (!isVisible() || plotItems().empty()) {
    return;
  }

  painter->save();
  const auto fw_theme = theme::appTheme();
  const QColor color = parent_plot_->canvas()->palette().windowText().color();
  painter->setPen(color);
  painter->setBrush(
      QBrush(theme::interaction(theme::Variant::Neutral, theme::State::Nominal, fw_theme), Qt::SolidPattern));
  painter->drawEllipse(icon_rect);

  if (collapsed_) {
    icon_rect -= QMarginsF(3, 3, 3, 3);
    painter->setBrush(QBrush(color, Qt::SolidPattern));
    painter->drawEllipse(icon_rect);
  }
  painter->restore();
}

void PlotLegend::drawLegendData(
    QPainter* painter, const QwtPlotItem* plot_item, const QwtLegendData& data, const QRectF& rect) const {
  const int item_margin = margin();
  const QRectF item_rect = rect.toRect().adjusted(item_margin, item_margin, -item_margin, -item_margin);
  painter->setClipRect(item_rect, Qt::IntersectClip);

  int title_offset = 0;
  constexpr qreal kDotDiameter = 8.0;
  constexpr qreal kDotGap = 4.0;
  if (const auto* curve = dynamic_cast<const QwtPlotCurve*>(plot_item); curve != nullptr) {
    QColor dot_color = curve->pen().color();
    if (!plot_item->isVisible()) {
      dot_color.setAlphaF(0.45);
    }
    const QRectF dot_rect(item_rect.left(), item_rect.center().y() - (kDotDiameter / 2.0), kDotDiameter, kDotDiameter);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(Qt::NoPen);
    painter->setBrush(dot_color);
    painter->drawEllipse(dot_rect);
    painter->restore();
    title_offset += static_cast<int>(kDotDiameter + kDotGap);
  } else if (const QwtGraphic graphic = data.icon(); !graphic.isEmpty()) {
    QRectF icon_rect(item_rect.topLeft(), graphic.defaultSize());
    icon_rect.moveCenter(QPoint(icon_rect.center().x(), rect.center().y()));
    if (plot_item->isVisible()) {
      graphic.render(painter, icon_rect, Qt::KeepAspectRatio);
    }
    title_offset += static_cast<int>(icon_rect.width()) + spacing();
  }

  const QwtText text = data.title();
  const QColor canvas_bg = parent_plot_->canvas()->palette().window().color();
  if (text.isEmpty()) {
    logLegendEntry(parent_plot_, QString(), /*empty=*/true, item_rect, title_offset, QColor(), canvas_bg);
    return;
  }

  QPen pen = textPen();
  const auto fw_theme = theme::appTheme();
  const QColor text_color = plot_item->isVisible()
                                ? parent_plot_->canvas()->palette().windowText().color()
                                : theme::onSurface(theme::Surface::DataBackdrop, theme::Emphasis::Disabled, fw_theme);
  pen.setColor(text_color);
  logLegendEntry(parent_plot_, text.text(), /*empty=*/false, item_rect, title_offset, text_color, canvas_bg);
  painter->setPen(pen);
  painter->setFont(font());
  text.draw(painter, item_rect.adjusted(title_offset, 0, 0, 0));
}

void PlotLegend::drawBackground(QPainter* painter, const QRectF& rect) const {
  painter->save();
  const auto fw_theme = theme::appTheme();
  QPen pen = textPen();
  const QColor border = theme::outline(theme::OutlineRole::Default, theme::OutlineState::Rest, fw_theme);
  pen.setColor(border);
  painter->setPen(pen);
  const QColor background = theme::surface(theme::Surface::Backdrop, fw_theme);
  painter->setBrush(background);
  const double radius = borderRadius();
  painter->drawRoundedRect(rect, radius, radius);
  painter->restore();
}

const QwtPlotItem* PlotLegend::itemAt(const QPoint& pos) const {
  const QRect canvas_rect = parent_plot_->canvas()->rect();
  if (collapsed_ || !isVisible() || !geometry(canvas_rect).contains(pos)) {
    return nullptr;
  }
  for (auto* item : plotItems()) {
    const auto geometries = legendGeometries(item);
    if (!geometries.empty() && geometries.first().contains(pos)) {
      return item;
    }
  }
  return nullptr;
}

const QwtPlotItem* PlotLegend::processMousePressEvent(QMouseEvent* mouse_event) {
  const QRect canvas_rect = parent_plot_->canvas()->rect();
  const QPoint press_point = mouse_event->pos();

  if (!isVisible() || mouse_event->modifiers() != Qt::NoModifier) {
    return nullptr;
  }

  if ((hideButtonRect() + QMargins(2, 2, 2, 2)).contains(press_point)) {
    collapsed_ = !collapsed_;
    parent_plot_->replot();
    return nullptr;
  }

  return itemAt(press_point);
}

}  // namespace PJ
