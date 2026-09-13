// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_plotting/PlotFocusOverlay.h"

#include <DockAreaWidget.h>
#include <DockContainerWidget.h>

#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QRect>
#include <algorithm>
#include <cmath>

#include "pj_plotting/DockWidget.h"
#include "pj_plotting/PlotWidget.h"
#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {
// Gap between the area's bounding rect and the frame, so the cue never lands
// on the 1-px splitter handles / container edges around the area.
constexpr int kFrameGapPx = 1;

// Paint a frame just INSIDE the area's bounding rect (in container coords),
// inset by kFrameGapPx plus half the pen (the pen straddles its path), so the
// whole stroke stays within the area and clear of the separators.
void paintFrame(QPainter& painter, const QRect& rect_in_container, const QColor& color, int thickness) {
  if (!rect_in_container.isValid()) {
    return;
  }
  QPen pen(color, thickness);
  pen.setJoinStyle(Qt::MiterJoin);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  const int inset = kFrameGapPx + thickness / 2;
  painter.drawRect(rect_in_container.adjusted(inset, inset, -inset, -inset));
}

// The watermark's face: the theme's heading family, upright and bold, well
// above body size — a plain engineering stamp, not a signature. Derived from
// the token rather than a named family so it stays coherent with the app font
// on every platform.
QFont watermarkFont(theme::Theme token_theme) {
  const theme::TypeSpec spec = theme::type(theme::TextRole::Heading, token_theme);
  QFont wm_font(spec.family);
  wm_font.setPixelSize(std::max(1, static_cast<int>(std::lround(spec.size * 1.8))));
  wm_font.setWeight(QFont::Bold);
  wm_font.setItalic(false);
  wm_font.setLetterSpacing(QFont::PercentageSpacing, 104.0);
  return wm_font;
}

// Paints `text` inside the bottom-right corner of `canvas_rect` (container
// coords): plain translucent ink, no fill, so it reads as a watermark on the
// plot itself and never covers an axis. No-op for empty text.
// Style resolved once per paint, then stamped on every canvas.
struct WatermarkStyle {
  int margin = 0;
  QColor ink;
  QFont font;
};

WatermarkStyle watermarkStyle(theme::Theme token_theme) {
  // Muted on-plot ink at ~37 % opacity: visible on both canvas themes, faint
  // enough that gridlines and curves stay legible underneath it.
  QColor ink = theme::onSurface(theme::Surface::DataBackdrop, theme::Emphasis::Muted, token_theme);
  ink.setAlphaF(0.37);
  return {
      .margin = theme::space(theme::Space::Comfortable, token_theme), .ink = ink, .font = watermarkFont(token_theme)};
}

void paintWatermark(QPainter& painter, const QRect& canvas_rect, const QString& text, const WatermarkStyle& style) {
  if (text.isEmpty() || !canvas_rect.isValid()) {
    return;
  }
  const QRect text_rect = canvas_rect.adjusted(style.margin, style.margin, -style.margin, -style.margin);
  if (!text_rect.isValid()) {
    return;
  }
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);
  painter.setPen(style.ink);
  painter.setFont(style.font);
  painter.drawText(text_rect, Qt::AlignRight | Qt::AlignBottom, text);
  painter.restore();
}
}  // namespace

PlotFocusOverlay::PlotFocusOverlay(ads::CDockContainerWidget& container) : QWidget(&container), container_(&container) {
  setAttribute(Qt::WA_TransparentForMouseEvents, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setFocusPolicy(Qt::NoFocus);
  container_->installEventFilter(this);
  syncGeometryToContainer();
  raise();
}

bool PlotFocusOverlay::ownsArea(ads::CDockAreaWidget* area) const {
  return area != nullptr && container_->isAncestorOf(area);
}

void PlotFocusOverlay::setFocusedArea(ads::CDockAreaWidget* area) {
  // Reject areas from a different container (e.g. floating dock window).
  // mapTo() against a non-ancestor would silently paint at garbage coords.
  ads::CDockAreaWidget* const next = ownsArea(area) ? area : nullptr;
  if (focused_area_ == next) {
    return;
  }
  focused_area_ = next;
  update();
}

void PlotFocusOverlay::setHoveredArea(ads::CDockAreaWidget* area) {
  ads::CDockAreaWidget* const next = ownsArea(area) ? area : nullptr;
  if (hovered_area_ == next) {
    return;
  }
  hovered_area_ = next;
  update();
}

void PlotFocusOverlay::setWatermarkText(const QString& text) {
  if (watermark_ == text) {
    return;
  }
  watermark_ = text;
  update();
}

void PlotFocusOverlay::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);

  const auto token_theme = theme::appTheme();
  // One mark per plot canvas in the tab, so every view the model composed
  // carries it, and it sits inside the plot area rather than over the axes.
  if (!watermark_.isEmpty()) {
    const WatermarkStyle style = watermarkStyle(token_theme);
    for (int i = 0; i < container_->dockAreaCount(); ++i) {
      ads::CDockAreaWidget* area = container_->dockArea(i);
      if (area == nullptr || !area->isVisible()) {
        continue;
      }
      auto* dock = qobject_cast<DockWidget*>(area->currentDockWidget());
      PlotWidget* plot = dock != nullptr ? dock->plotWidget() : nullptr;
      QWidget* canvas = plot != nullptr ? plot->canvasWidget() : nullptr;
      if (canvas == nullptr) {
        continue;
      }
      const QRect canvas_rect(canvas->mapTo(container_, QPoint(0, 0)), canvas->size());
      paintWatermark(painter, canvas_rect, watermark_, style);
    }
  }

  auto rect_for = [this](ads::CDockAreaWidget* area) -> QRect {
    if (area == nullptr) {
      return {};
    }
    const QPoint top_left = area->mapTo(container_, QPoint(0, 0));
    return {top_left, area->size()};
  };

  // Hover only paints when it differs from focus (focus wins under cursor).
  // Focus uses the accent CHECKED tone — the same colour as checked buttons —
  // so the active dock reads as "selected" in both themes without the harsher
  // focus-ring ink, and distinctly from a merely hovered one.
  const QColor hover_color = theme::surface(PJ::theme::Surface::Separation, token_theme);
  const QColor focus_color = theme::interaction(theme::Variant::Accent, theme::State::Checked, token_theme);
  // Focus is drawn slightly thicker than hover so the active dock reads at a
  // glance even when both frames are on screen.
  if (hovered_area_ != nullptr && hovered_area_ != focused_area_) {
    paintFrame(painter, rect_for(hovered_area_), hover_color, /*thickness=*/1);
  }
  if (focused_area_ != nullptr) {
    paintFrame(painter, rect_for(focused_area_), focus_color, /*thickness=*/2);
  }
}

bool PlotFocusOverlay::eventFilter(QObject* watched, QEvent* event) {
  if (watched == container_) {
    const QEvent::Type type = event->type();
    if (type == QEvent::Resize || type == QEvent::LayoutRequest) {
      syncGeometryToContainer();
    } else if (type == QEvent::ChildAdded || type == QEvent::ChildPolished) {
      // ADS may add new top-level QSplitters as direct siblings, which
      // would otherwise stack above us. Bounce back to the top.
      raise();
      update();
    }
  }
  return QWidget::eventFilter(watched, event);
}

void PlotFocusOverlay::syncGeometryToContainer() {
  setGeometry(container_->rect());
  raise();
  update();
}

}  // namespace PJ
