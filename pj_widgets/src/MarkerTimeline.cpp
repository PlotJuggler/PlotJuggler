// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
//
// Editable multi-marker timeline. See MarkerTimeline.h.

#include <pj_widgets/MarkerTimeline.h>

#include <QContextMenuEvent>
#include <QHideEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>

#include "pj_widgets/OwnedMenu.h"

namespace PJ {

namespace {

// Geometry + palette mirror RangeSlider so the two controls read as one family.
const int kHandleWidth = 8;
const int kTrackHeight = 24;
const int kMargin = 1;
// Smallest region span (step units) — keeps a region's body grabbable so it can
// never collapse to two coincident, unmovable edge handles.
const int kMinRegionWidth = 8;

const QColor kGrooveBorder(0xB0, 0xB0, 0xBF);  // border_default
const QColor kRegionFill(0xC2, 0xDC, 0xFF);    // PJLightBlue (region body)
const QColor kHandle(0xFF, 0xAE, 0xFF);        // PJLightPurple (resting handle)
const QColor kHandleActive(0xCC, 0x00, 0xCC);  // PJPurple (hovered / dragged)
const QColor kHandleBorder(0xCC, 0x00, 0xCC);  // PJPurple (handle border)
const QColor kDisabledInk(0x80, 0x80, 0x80);

}  // namespace

MarkerTimeline::MarkerTimeline(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
}

QSize MarkerTimeline::minimumSizeHint() const {
  return {kHandleWidth * 6, kTrackHeight + 6};
}

// --- domain -----------------------------------------------------------------

void MarkerTimeline::setRange(int minimum, int maximum) {
  minimum_ = minimum;
  maximum_ = std::max(maximum, minimum + 1);
  for (Mark& m : marks_) {
    m.start = clampValue(m.start);
    m.end = clampValue(m.end);
    if (m.kind == Kind::kRegion && m.end < m.start) {
      std::swap(m.start, m.end);
    }
  }
  update();
}

void MarkerTimeline::setTimeSpan(qint64 min_ns, qint64 max_ns) {
  min_ns_ = min_ns;
  max_ns_ = max_ns;
  has_time_span_ = max_ns > min_ns;
  update();
}

void MarkerTimeline::setLabelFormatter(std::function<QString(qint64)> formatter) {
  label_formatter_ = std::move(formatter);
}

// --- mark set ---------------------------------------------------------------

void MarkerTimeline::setMarks(const QVector<Mark>& marks) {
  // An owner refresh that lands mid-drag (e.g. a periodic widget_data tick) must
  // not snap the mark the user is actively dragging back to its pre-drag spot.
  if (dragging_id_ != 0) {
    return;
  }
  marks_ = marks;
  int max_id = 0;
  for (Mark& m : marks_) {
    normalize(m);
    max_id = std::max(max_id, m.id);
  }
  next_id_ = max_id + 1;
  update();  // owner-driven: no marksChanged (would loop the protocol round-trip)
}

void MarkerTimeline::normalize(Mark& m) const {
  m.start = clampValue(m.start);
  m.end = (m.kind == Kind::kEvent) ? m.start : clampValue(m.end);
  if (m.kind == Kind::kRegion && m.end < m.start) {
    std::swap(m.start, m.end);
  }
}

int MarkerTimeline::addMark(Kind kind, int start, int end) {
  Mark m;
  m.id = next_id_++;
  m.kind = kind;
  m.start = start;
  m.end = end;
  normalize(m);
  marks_.push_back(m);
  emit marksChanged();
  update();
  return m.id;
}

void MarkerTimeline::deleteMark(int id) {
  const auto before = marks_.size();
  marks_.erase(std::remove_if(marks_.begin(), marks_.end(), [id](const Mark& m) { return m.id == id; }), marks_.end());
  if (marks_.size() != before) {
    emit marksChanged();
    update();
  }
}

void MarkerTimeline::moveMark(int id, int new_start) {
  Mark* m = markById(id);
  if (m == nullptr) {
    return;
  }
  if (m->kind == Kind::kEvent) {
    m->start = m->end = clampValue(new_start);
  } else {
    const int width = m->end - m->start;
    const int start = std::min(clampValue(new_start), maximum_ - width);
    m->start = std::max(start, minimum_);
    m->end = m->start + width;
  }
  emit marksChanged();
  update();
}

void MarkerTimeline::resizeRegion(int id, int new_start, int new_end) {
  Mark* m = markById(id);
  if (m == nullptr || m->kind != Kind::kRegion) {
    return;
  }
  m->start = clampValue(new_start);
  m->end = clampValue(new_end);
  if (m->end < m->start) {
    std::swap(m->start, m->end);
  }
  if (m->end - m->start < kMinRegionWidth) {
    // Grow toward whichever bound leaves room; clamp keeps it in range.
    m->end = std::min(maximum_, m->start + kMinRegionWidth);
    m->start = std::max(minimum_, m->end - kMinRegionWidth);
  }
  emit marksChanged();
  update();
}

void MarkerTimeline::clearMarks() {
  if (marks_.isEmpty()) {
    return;
  }
  marks_.clear();
  emit marksChanged();
  update();
}

// --- geometry ---------------------------------------------------------------

double MarkerTimeline::usableLength() const {
  return std::max(1.0, static_cast<double>(width()) - 2.0 * kMargin - kHandleWidth);
}

double MarkerTimeline::pxForValue(int value) const {
  const double frac = static_cast<double>(value - minimum_) / static_cast<double>(maximum_ - minimum_);
  return kMargin + kHandleWidth / 2.0 + frac * usableLength();
}

int MarkerTimeline::valueForPx(double px) const {
  const double frac = (px - kMargin - kHandleWidth / 2.0) / usableLength();
  return clampValue(minimum_ + static_cast<int>(frac * (maximum_ - minimum_) + 0.5));
}

int MarkerTimeline::clampValue(int v) const {
  return std::max(minimum_, std::min(maximum_, v));
}

MarkerTimeline::Mark* MarkerTimeline::markById(int id) {
  for (Mark& m : marks_) {
    if (m.id == id) {
      return &m;
    }
  }
  return nullptr;
}

std::pair<int, MarkerTimeline::Grab> MarkerTimeline::hitTest(const QPoint& pos) const {
  const double track_top = (height() - kTrackHeight) / 2.0;
  const QRectF track(0, track_top, width(), kTrackHeight);
  if (!track.contains(QPointF(pos))) {
    // Allow a little vertical slack so thin handles stay grabbable.
    if (pos.y() < track_top - 2 || pos.y() > track_top + kTrackHeight + 2) {
      return {0, Grab::kNone};
    }
  }
  // Last-drawn wins: iterate front-to-back.
  for (auto it = marks_.rbegin(); it != marks_.rend(); ++it) {
    const double half = kHandleWidth / 2.0 + 1.0;
    if (it->kind == Kind::kEvent) {
      const double x = pxForValue(it->start);
      if (std::abs(pos.x() - x) <= half) {
        return {it->id, Grab::kMoveEvent};
      }
      continue;
    }
    const double xl = pxForValue(it->start);
    const double xr = pxForValue(it->end);
    if (std::abs(pos.x() - xl) <= half) {
      return {it->id, Grab::kResizeStart};
    }
    if (std::abs(pos.x() - xr) <= half) {
      return {it->id, Grab::kResizeEnd};
    }
    if (pos.x() > xl && pos.x() < xr) {
      return {it->id, Grab::kMoveRegion};
    }
  }
  return {0, Grab::kNone};
}

// --- painting ---------------------------------------------------------------

void MarkerTimeline::paintEvent(QPaintEvent*) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);

  const double track_top = (height() - kTrackHeight) / 2.0;
  const QRectF track(kMargin, track_top, width() - 2.0 * kMargin, kTrackHeight);
  const bool enabled = isEnabled();

  // 1. Region fills (under the groove border).
  for (const Mark& m : marks_) {
    if (m.kind != Kind::kRegion) {
      continue;
    }
    const double xl = pxForValue(m.start);
    const double xr = pxForValue(m.end);
    painter.setPen(Qt::NoPen);
    painter.setBrush(enabled ? kRegionFill : kDisabledInk);
    painter.drawRect(QRectF(xl, track.top(), xr - xl, track.height()));
  }

  // 2. Groove outline.
  painter.setPen(QPen(kGrooveBorder, 1));
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(track.adjusted(0.5, 0.5, -0.5, -0.5));

  // 3. Handles (region edges + event ticks), purple, active when hovered/dragged.
  auto paint_handle = [&](double center_x, bool active) {
    const QRectF r(center_x - kHandleWidth / 2.0, track_top, kHandleWidth, kTrackHeight);
    painter.setPen(QPen(enabled ? kHandleBorder : kDisabledInk, 1));
    painter.setBrush(!enabled ? kDisabledInk.lighter(125) : (active ? kHandleActive : kHandle));
    painter.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
  };
  for (const Mark& m : marks_) {
    const bool active = (m.id == hovered_id_) || (m.id == dragging_id_);
    paint_handle(pxForValue(m.start), active);
    if (m.kind == Kind::kRegion) {
      paint_handle(pxForValue(m.end), active);
    }
  }
}

// --- interaction ------------------------------------------------------------

void MarkerTimeline::mousePressEvent(QMouseEvent* event) {
  if (!(event->buttons() & Qt::LeftButton)) {
    return;
  }
  const auto [id, grab] = hitTest(event->pos());
  if (id == 0) {
    return;
  }
  dragging_id_ = id;
  grab_ = grab;
  const Mark* m = markById(id);
  const int anchor = (grab == Grab::kResizeEnd) ? m->end : m->start;
  grab_offset_px_ = event->position().x() - pxForValue(anchor);
  QToolTip::showText(event->globalPosition().toPoint(), tooltipFor(*m), this);
}

void MarkerTimeline::mouseMoveEvent(QMouseEvent* event) {
  if ((event->buttons() & Qt::LeftButton) && dragging_id_ != 0) {
    Mark* m = markById(dragging_id_);
    if (m == nullptr) {
      return;
    }
    const int value = valueForPx(event->position().x() - grab_offset_px_);
    switch (grab_) {
      case Grab::kMoveRegion: {
        const int w = m->end - m->start;
        m->start = std::max(minimum_, std::min(value, maximum_ - w));
        m->end = m->start + w;
        break;
      }
      case Grab::kResizeStart:
        m->start = std::max(minimum_, std::min(value, m->end - kMinRegionWidth));
        break;
      case Grab::kResizeEnd:
        m->end = std::min(maximum_, std::max(value, m->start + kMinRegionWidth));
        break;
      case Grab::kMoveEvent:
        m->start = m->end = value;
        break;
      case Grab::kNone:
        break;
    }
    QToolTip::showText(event->globalPosition().toPoint(), tooltipFor(*m), this);
    update();
    return;
  }

  // Hover highlight when not dragging.
  const int hovered = hitTest(event->pos()).first;
  if (hovered != hovered_id_) {
    hovered_id_ = hovered;
    update();
  }
}

void MarkerTimeline::mouseReleaseEvent(QMouseEvent*) {
  finalizeDrag();
}

void MarkerTimeline::hideEvent(QHideEvent* event) {
  // No mouse-release fires when the panel is hidden/closed mid-drag; finalize here
  // so the in-flight edit is emitted and dragging_id_ does not wedge setMarks().
  finalizeDrag();
  QWidget::hideEvent(event);
}

void MarkerTimeline::finalizeDrag() {
  if (dragging_id_ != 0) {
    dragging_id_ = 0;
    grab_ = Grab::kNone;
    QToolTip::hideText();
    emit marksChanged();
    update();
  }
}

void MarkerTimeline::contextMenuEvent(QContextMenuEvent* event) {
  const auto [id, grab] = hitTest(event->pos());
  if (id == 0 || grab == Grab::kNone) {
    return;
  }
  auto* menu = new QMenu(this);
  connect(
      menu->addAction(tr("Delete marker")), &QAction::triggered, this, [this, mark_id = id]() { deleteMark(mark_id); });
  showOwnedMenu(menu, event->globalPos());
}

void MarkerTimeline::leaveEvent(QEvent*) {
  if (hovered_id_ != 0) {
    hovered_id_ = 0;
    update();
  }
}

QString MarkerTimeline::tooltipFor(const Mark& mark) const {
  auto label = [&](int step) -> QString {
    if (!has_time_span_) {
      return QString::number(step);
    }
    const double frac = static_cast<double>(step - minimum_) / static_cast<double>(maximum_ - minimum_);
    const qint64 ns = min_ns_ + static_cast<qint64>(frac * static_cast<double>(max_ns_ - min_ns_));
    if (label_formatter_) {
      return label_formatter_(ns);
    }
    return QString::number(static_cast<double>(ns) / 1e9, 'f', 3) + QStringLiteral(" s");
  };
  if (mark.kind == Kind::kEvent) {
    return label(mark.start);
  }
  return label(mark.start) + QStringLiteral(" → ") + label(mark.end);
}

}  // namespace PJ
