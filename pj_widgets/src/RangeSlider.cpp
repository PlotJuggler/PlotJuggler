// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Two-handle range slider implementation, wrapped in namespace PJ.
// See RangeSlider.h.

#include <pj_widgets/Hatch.h>
#include <pj_widgets/RangeSlider.h>

#include <QDebug>
#include <QGuiApplication>
#include <QPainterPath>
#include <QPalette>
#include <QRegion>
#include <algorithm>
#include <limits>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {

// Geometry mirrors the app's playback slider (QSlider#timeSlider) so the range slider reads as the same control: a
// full-height rectangular track with a thin vertical handle. Colors resolve from the semantic framework tokens that
// back the stylesheet.
const int kScHandleWidth = 8;   // timeSlider handle: 6px content + 1px border each side = 8px rendered
const int kScTrackHeight = 24;  // timeSlider groove + handle height (px)
const int kScLeftRightMargin = 1;

struct RangeSliderColors {
  QColor groove_border;
  QColor selection;
  QColor selection_border;  // darker-accent outline on the selected fill
  QColor handle;            // resting (nominal)
  QColor handle_active;     // hovered
  QColor handle_pressed;
  QColor handle_border;
  QColor selection_disabled;  // accent-family Disabled fill (playback ::sub-page:disabled parity)
  QColor handle_disabled;     // highlight-family Disabled fill (playback ::handle:disabled parity)
  QColor marker_line;
  QColor marker_text;
  QColor marker_in_range;
};

theme::Theme frameworkTheme() {
  // The application palette is read, never the widget's: QStyleSheetStyle rewrites widget palettes under QSS (a styled
  // ancestor can resolve Window to #000000), which mis-detects "dark" inside plugin dialogs. Theme.cpp keeps
  // QGuiApplication::palette()'s Window in lockstep with the theme backdrop.
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}

RangeSliderColors rangeSliderColors() {
  const auto fw_theme = frameworkTheme();
  return {
      .groove_border = theme::surface(PJ::theme::Surface::Separation, fw_theme),
      // Selected range fill = accent (blue); handles = highlight (magenta) so the
      // grips stand out against the fill — matching the app playback slider.
      .selection = theme::interaction(theme::Variant::Accent, theme::State::Nominal, fw_theme),
      .selection_border = theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme),
      .handle = theme::interaction(theme::Variant::Highlight, theme::State::Nominal, fw_theme),
      .handle_active = theme::interaction(theme::Variant::Highlight, theme::State::Hovered, fw_theme),
      .handle_pressed = theme::interaction(theme::Variant::Highlight, theme::State::Pressed, fw_theme),
      .handle_border = theme::surface(PJ::theme::Surface::Separation, fw_theme),
      .selection_disabled = theme::interaction(theme::Variant::Accent, theme::State::Disabled, fw_theme),
      .handle_disabled = theme::interaction(theme::Variant::Highlight, theme::State::Disabled, fw_theme),
      .marker_line = theme::surface(PJ::theme::Surface::Separation, fw_theme),
      .marker_text = theme::onSurface(theme::Surface::Backdrop, theme::Emphasis::Muted, fw_theme),
      .marker_in_range = theme::overlay(theme::Overlay::Selected, fw_theme),
  };
}

}  // namespace

RangeSlider::RangeSlider(QWidget* a_parent) : QWidget(a_parent) {
  setMouseTracking(true);
}

RangeSlider::RangeSlider(Qt::Orientation ori, Options t, QWidget* a_parent)
    : QWidget(a_parent), orientation_(ori), type_(t) {
  setMouseTracking(true);
}

void RangeSlider::paintEvent(QPaintEvent* a_event) {
  Q_UNUSED(a_event);
  QPainter painter(this);

  // Groove geometry: a full-height rectangular track (timeSlider shape).
  QRectF background_rect;
  if (orientation_ == Qt::Horizontal) {
    background_rect = QRectF(kScLeftRightMargin, trackTop(), width() - kScLeftRightMargin * 2, trackHeight());
  } else {
    background_rect =
        QRectF((width() - kScTrackHeight) / 2.0, kScLeftRightMargin, kScTrackHeight, height() - kScLeftRightMargin * 2);
  }

  const bool enabled = isEnabled();
  const auto fw_theme = frameworkTheme();
  const RangeSliderColors colors = rangeSliderColors();
  const QRectF left_handle_rect = firstHandleRect();
  const QRectF right_handle_rect = secondHandleRect();
  // Only the track outline carries the framework rounding (playback-bar parity); everything inside (fill, hatch,
  // markers, handles) draws square and is clipped to the rounded outline, so nothing pokes out of the corners.
  const qreal corner_radius = theme::radius(theme::Radius::Input, fw_theme);
  QPainterPath track_path;
  track_path.addRoundedRect(background_rect, corner_radius, corner_radius);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.save();
  painter.setClipPath(track_path);

  // 1. Selected-range fill (between the two handles), PJLightBlue like the playback slider's played sub-page. Drawn
  // first; the groove border is stroked on top so it reads crisply.
  QRectF selected_rect(background_rect);
  if (orientation_ == Qt::Horizontal) {
    selected_rect.setLeft(type_.testFlag(kLeftHandle) ? left_handle_rect.right() : left_handle_rect.left());
    selected_rect.setRight(type_.testFlag(kRightHandle) ? right_handle_rect.left() : right_handle_rect.right());
  } else {
    selected_rect.setTop(type_.testFlag(kLeftHandle) ? left_handle_rect.bottom() : left_handle_rect.top());
    selected_rect.setBottom(type_.testFlag(kRightHandle) ? right_handle_rect.top() : right_handle_rect.bottom());
  }
  // Darker-accent outline on the selected fill (playback-slider parity), inset by 0.5 px so the 1-px stroke lands
  // crisply inside the fill rect. Disabled keeps the geometry but flattens to the accent family's Disabled fill, as the
  // playback slider's ::sub-page:disabled.
  painter.setPen(QPen(enabled ? colors.selection_border : colors.selection_disabled, 1));
  painter.setBrush(enabled ? colors.selection : colors.selection_disabled);
  painter.drawRect(selected_rect.adjusted(0.5, 0.5, -0.5, -0.5));

  // 1b. "No data" texture: the shared app hatch (PJ::drawNoDataHatch) over the unselected part of the track
  // (background_rect minus the [lower, upper] fill), enabled and disabled. Phased to this widget's global origin, so
  // its diagonal lines line up with every other widget that paints the hatch (the Timeline). Contained to the track
  // height: it reads as one horizontal strip flanking the selection and never bleeds into the floating-label rows above
  // and below. Clipped to the unselected region, so the lines stay static as a handle drags (only the clip moves).
  {
    QRegion unselected(background_rect.toAlignedRect());
    unselected -= selected_rect.toAlignedRect();  // handles, drawn later, cover their own width on top
    painter.save();
    // Intersect with the ambient rounded-track clip so the hatch stays inside
    // the rounded corners AND the unselected region.
    painter.setClipRegion(unselected, Qt::IntersectClip);
    // Backdrop and hatch in one shared call (drawNoDataHatch), so the unselected track composites the same ink over the
    // same framework data backdrop the Timeline uses. Without the backdrop fill the bare groove let the white dialog
    // show through, giving the same ink ~26% more contrast (255 vs 238 backdrop) and a busier read.
    drawNoDataHatch(painter, background_rect, mapToGlobal(QPointF(0, 0)), enabled);
    painter.restore();
  }

  // 2. Groove outline — transparent body + 1px border, rounded at the framework
  //    input radius (timeSlider groove parity).
  painter.setPen(QPen(colors.groove_border, theme::stroke(theme::Stroke::Hairline, fw_theme)));
  painter.setBrush(Qt::NoBrush);
  painter.drawRoundedRect(background_rect.adjusted(0.5, 0.5, -0.5, -0.5), corner_radius, corner_radius);

  if (!markers_.empty()) {
    drawMarkers(painter, background_rect);
  }

  // 3. Handles: thin full-height square grips (the timeSlider handle shape): highlight nominal at rest, hovered on
  // hover, pressed while dragging, the highlight family's Disabled fill when disabled (as the playback slider's
  // ::handle:disabled).
  auto paint_handle = [&](const QRectF& r, bool hovered, bool pressed) {
    painter.setPen(QPen(colors.handle_border, 1));
    QColor fill = colors.handle;
    if (!enabled) {
      fill = colors.handle_disabled;
    } else if (pressed) {
      fill = colors.handle_pressed;
    } else if (hovered) {
      fill = colors.handle_active;
    }
    painter.setBrush(fill);
    painter.drawRect(r.adjusted(0.5, 0.5, -0.5, -0.5));
  };
  if (type_.testFlag(kLeftHandle)) {
    paint_handle(left_handle_rect, hovered_handle_ == 1, first_handle_pressed_);
  }
  if (type_.testFlag(kRightHandle)) {
    paint_handle(right_handle_rect, hovered_handle_ == 2, second_handle_pressed_);
  }
  painter.restore();  // rounded-track clip

  if (floating_labels_) {
    drawFloatingLabels(painter);
  }
}

QRectF RangeSlider::firstHandleRect() const {
  float percentage = (lower_value_ - minimum_) * 1.0 / interval_;
  return handleRect(percentage * validLength() + kScLeftRightMargin);
}

QRectF RangeSlider::secondHandleRect() const {
  float percentage = (upper_value_ - minimum_) * 1.0 / interval_;
  return handleRect(
      percentage * validLength() + kScLeftRightMargin + (type_.testFlag(kLeftHandle) ? kScHandleWidth : 0));
}

QRectF RangeSlider::handleRect(int a_value) const {
  // Thin grip spanning the full track height (timeSlider handle: 6px wide,
  // groove-tall), centered across the short axis.
  if (orientation_ == Qt::Horizontal) {
    return QRect(a_value, trackTop(), kScHandleWidth, trackHeight());
  } else {
    return QRect((width() - kScTrackHeight) / 2, a_value, kScTrackHeight, kScHandleWidth);
  }
}

void RangeSlider::mousePressEvent(QMouseEvent* a_event) {
  if (a_event->buttons() & Qt::LeftButton) {
    int pos_check, pos_max, pos_value, first_handle_rect_pos_value, second_handle_rect_pos_value;
    pos_check = (orientation_ == Qt::Horizontal) ? a_event->pos().y() : a_event->pos().x();
    pos_max = (orientation_ == Qt::Horizontal) ? height() : width();
    pos_value = (orientation_ == Qt::Horizontal) ? a_event->pos().x() : a_event->pos().y();
    first_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? firstHandleRect().x() : firstHandleRect().y();
    second_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? secondHandleRect().x() : secondHandleRect().y();

    // Floating labels double as hit-test targets.
    const bool on_lower_label =
        floating_labels_ && !lower_label_rect_.isNull() && lower_label_rect_.contains(a_event->pos());
    const bool on_upper_label =
        floating_labels_ && !upper_label_rect_.isNull() && upper_label_rect_.contains(a_event->pos());
    const bool on_center_label =
        floating_labels_ && !center_label_rect_.isNull() && center_label_rect_.contains(a_event->pos());

    second_handle_pressed_ =
        on_upper_label || (!on_lower_label && !on_center_label && secondHandleRect().contains(a_event->pos()));
    first_handle_pressed_ =
        on_lower_label || (!second_handle_pressed_ && !on_center_label && firstHandleRect().contains(a_event->pos()));
    range_drag_active_ = false;

    if (first_handle_pressed_) {
      delta_ = pos_value - (first_handle_rect_pos_value + kScHandleWidth / 2);
    } else if (second_handle_pressed_) {
      delta_ = pos_value - (second_handle_rect_pos_value + kScHandleWidth / 2);
    } else if (on_center_label && type_.testFlag(kDoubleHandles)) {
      range_drag_active_ = true;
      range_drag_start_pos_ = pos_value;
      range_drag_lower_start_ = lower_value_;
      range_drag_upper_start_ = upper_value_;
    } else if (
        type_.testFlag(kDoubleHandles) && pos_value > first_handle_rect_pos_value + kScHandleWidth &&
        pos_value < second_handle_rect_pos_value && pos_check >= 2 && pos_check <= pos_max - 2) {
      range_drag_active_ = true;
      range_drag_start_pos_ = pos_value;
      range_drag_lower_start_ = lower_value_;
      range_drag_upper_start_ = upper_value_;
    } else if (pos_check >= 2 && pos_check <= pos_max - 2) {
      int step = interval_ / 10 < 1 ? 1 : interval_ / 10;
      if (pos_value < first_handle_rect_pos_value) {
        setLowerValue(lower_value_ - step);
      } else if (pos_value > second_handle_rect_pos_value + kScHandleWidth) {
        setUpperValue(upper_value_ + step);
      }
    }
  }

  maybeShowHandleTooltip(a_event->globalPosition().toPoint(), a_event->pos());
}

void RangeSlider::mouseMoveEvent(QMouseEvent* a_event) {
  if (a_event->buttons() & Qt::LeftButton) {
    int pos_value, first_handle_rect_pos_value, second_handle_rect_pos_value;
    pos_value = (orientation_ == Qt::Horizontal) ? a_event->pos().x() : a_event->pos().y();
    first_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? firstHandleRect().x() : firstHandleRect().y();
    second_handle_rect_pos_value = (orientation_ == Qt::Horizontal) ? secondHandleRect().x() : secondHandleRect().y();

    if (range_drag_active_) {
      int pixel_delta = pos_value - range_drag_start_pos_;
      int value_delta = static_cast<int>(pixel_delta * 1.0 / validLength() * interval_);
      int new_lower = range_drag_lower_start_ + value_delta;
      int new_upper = range_drag_upper_start_ + value_delta;

      if (new_lower < minimum_) {
        new_upper += (minimum_ - new_lower);
        new_lower = minimum_;
      }
      if (new_upper > maximum_) {
        new_lower -= (new_upper - maximum_);
        new_upper = maximum_;
      }
      new_lower = std::max(new_lower, minimum_);
      new_upper = std::min(new_upper, maximum_);

      setLowerValue(new_lower);
      setUpperValue(new_upper);
    } else if (first_handle_pressed_ && type_.testFlag(kLeftHandle)) {
      if (pos_value - delta_ + kScHandleWidth / 2 <= second_handle_rect_pos_value) {
        setLowerValue(
            (pos_value - delta_ - kScLeftRightMargin - kScHandleWidth / 2) * 1.0 / validLength() * interval_ +
            minimum_);
      } else {
        setLowerValue(upper_value_);
      }
    } else if (second_handle_pressed_ && type_.testFlag(kRightHandle)) {
      if (first_handle_rect_pos_value + kScHandleWidth * (type_.testFlag(kDoubleHandles) ? 1.5 : 0.5) <=
          pos_value - delta_) {
        setUpperValue(
            (pos_value - delta_ - kScLeftRightMargin - kScHandleWidth / 2 -
             (type_.testFlag(kDoubleHandles) ? kScHandleWidth : 0)) *
                1.0 / validLength() * interval_ +
            minimum_);
      } else {
        setUpperValue(lower_value_);
      }
    }
  }

  // Hover tint (timeSlider parity): when not dragging, light up the handle the
  // cursor is over in PJPurple.
  if (!(a_event->buttons() & Qt::LeftButton)) {
    const QPointF p = a_event->position();
    int hovered = 0;
    if (type_.testFlag(kLeftHandle) && firstHandleRect().contains(p)) {
      hovered = 1;
    } else if (type_.testFlag(kRightHandle) && secondHandleRect().contains(p)) {
      hovered = 2;
    }
    hovered_handle_ = hovered;
  }

  update();
  maybeShowHandleTooltip(a_event->globalPosition().toPoint(), a_event->pos());
}

void RangeSlider::mouseReleaseEvent(QMouseEvent* a_event) {
  Q_UNUSED(a_event);

  first_handle_pressed_ = false;
  second_handle_pressed_ = false;
  range_drag_active_ = false;
  update();

  if (show_handle_value_tooltip_) {
    QToolTip::hideText();
    tooltip_visible_ = false;
  }
}

void RangeSlider::changeEvent(QEvent* a_event) {
  // Repaint on enable/disable so the groove/handles switch to/from the muted
  // (disabled) palette.
  if (a_event->type() == QEvent::EnabledChange) {
    update();
  }
}

void RangeSlider::leaveEvent(QEvent* e) {
  QWidget::leaveEvent(e);
  if (hovered_handle_ != 0) {
    hovered_handle_ = 0;
    update();
  }
  QToolTip::hideText();
  tooltip_visible_ = false;
}

QSize RangeSlider::minimumSizeHint() const {
  // Track height (as the playback scrubber) plus one label row above it for the floating handle labels; no reserve
  // below the track.
  int h = kScTrackHeight;
  if (floating_labels_) {
    QFontMetrics fm(font());
    h += fm.height() + 6 + 4;  // label_height + gap (one row, matches trackTop)
  }
  return QSize(kScHandleWidth * 2 + kScLeftRightMargin * 2, h);
}

int RangeSlider::getMinimun() const {
  return minimum_;
}
int RangeSlider::getMaximun() const {
  return maximum_;
}
int RangeSlider::getLowerValue() const {
  return lower_value_;
}
int RangeSlider::getUpperValue() const {
  return upper_value_;
}

void RangeSlider::setLowerValue(int a_lower_value) {
  if (a_lower_value > maximum_) {
    a_lower_value = maximum_;
  }
  if (a_lower_value < minimum_) {
    a_lower_value = minimum_;
  }
  // Post-clamp no-ops must not emit: the dialog host forwards every emission as a full plugin event round trip, so a
  // drag held past the track end (or a same-value echo re-apply) would flood it with no-op events.
  if (a_lower_value == lower_value_) {
    return;
  }
  lower_value_ = a_lower_value;
  emit lowerValueChanged(lower_value_);
  update();
}

void RangeSlider::setUpperValue(int a_upper_value) {
  if (a_upper_value > maximum_) {
    a_upper_value = maximum_;
  }
  if (a_upper_value < minimum_) {
    a_upper_value = minimum_;
  }
  if (a_upper_value == upper_value_) {
    return;  // same no-op rule as setLowerValue
  }
  upper_value_ = a_upper_value;
  emit upperValueChanged(upper_value_);
  update();
}

void RangeSlider::setMinimum(int a_minimum) {
  if (a_minimum <= maximum_) {
    minimum_ = a_minimum;
  } else {
    int old_max = maximum_;
    minimum_ = old_max;
    maximum_ = a_minimum;
  }
  interval_ = maximum_ - minimum_;
  update();

  setLowerValue(minimum_);
  setUpperValue(maximum_);

  emit rangeChanged(minimum_, maximum_);
}

void RangeSlider::setMaximum(int a_maximum) {
  if (a_maximum >= minimum_) {
    maximum_ = a_maximum;
  } else {
    int old_min = minimum_;
    maximum_ = old_min;
    minimum_ = a_maximum;
  }
  interval_ = maximum_ - minimum_;
  update();

  setLowerValue(minimum_);
  setUpperValue(maximum_);

  emit rangeChanged(minimum_, maximum_);
}

int RangeSlider::validLength() const {
  int len = (orientation_ == Qt::Horizontal) ? width() : height();
  return len - kScLeftRightMargin * 2 - kScHandleWidth * (type_.testFlag(kDoubleHandles) ? 2 : 1);
}

int RangeSlider::trackTop() const {
  if (orientation_ != Qt::Horizontal || !floating_labels_) {
    return static_cast<int>((height() - trackHeight()) / 2);  // centered (no label row)
  }
  // One label row above the track; the track fills the remaining height below.
  const QFontMetrics fm(font());
  return fm.height() + 6 + 4;
}

int RangeSlider::trackHeight() const {
  if (orientation_ != Qt::Horizontal || !floating_labels_) {
    return kScTrackHeight;
  }
  const QFontMetrics fm(font());
  const int label_row = fm.height() + 6 + 4;
  return std::max(kScTrackHeight, height() - label_row);
}

void RangeSlider::setRange(int a_minimum, int a_maximum) {
  setMinimum(a_minimum);
  setMaximum(a_maximum);
}

void RangeSlider::setOptions(Options t) {
  type_ = t;
  update();
}

void RangeSlider::setMarkers(std::vector<Marker> markers) {
  std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
  markers_ = std::move(markers);
  update();
}

void RangeSlider::drawMarkers(QPainter& painter, const QRectF& background_rect) {
  if (interval_ <= 0) {
    return;
  }
  const int px_len = validLength();
  if (px_len <= 0) {
    return;
  }
  // Same value->x mapping the handles + ticks use.
  const int offset = kScLeftRightMargin + (type_.testFlag(kDoubleHandles) ? kScHandleWidth : 0);
  auto value_to_x = [&](int value) -> int {
    const double pct = static_cast<double>(value - minimum_) / static_cast<double>(interval_);
    return static_cast<int>(pct * static_cast<double>(px_len)) + offset;
  };
  const QFontMetrics fm(painter.font());
  const int top = static_cast<int>(background_rect.top());
  const int height = static_cast<int>(background_rect.bottom()) - top;
  const RangeSliderColors colors = rangeSliderColors();

  for (const auto& m : markers_) {
    int x0 = value_to_x(m.start);
    int x1 = value_to_x(m.end);
    if (x1 <= x0) {
      x1 = x0 + 1;  // keep a degenerate box visible
    }
    const int box_w = x1 - x0;
    const QRect box(x0, top, box_w, height);

    // The box is shaded when its [start, end] overlaps the current [lower, upper] selection: the "which chunk falls in
    // the range" cue. Translucent over the groove, so the blue selection fill still reads underneath.
    if (m.start < upper_value_ && m.end > lower_value_) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(colors.marker_in_range);
      painter.drawRect(box);
    }

    // The box outline is at the chunk's true extent: disjoint chunks read as separate boxes with blank slider space
    // between them.
    painter.setPen(QPen(colors.marker_line, theme::stroke(theme::Stroke::Hairline, frameworkTheme())));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(box.adjusted(0, 0, -1, -1));

    // Chunk label, centered in the box, only when it fits.
    if (!m.label.isEmpty() && box_w >= fm.horizontalAdvance(m.label) + 4) {
      painter.setPen(colors.marker_text);
      painter.drawText(box, Qt::AlignCenter, m.label);
    }
  }
}

void RangeSlider::setShowHandleValueTooltip(bool on) {
  show_handle_value_tooltip_ = on;
  if (!on) {
    QToolTip::hideText();
    tooltip_visible_ = false;
  }
}

bool RangeSlider::showHandleValueTooltip() const {
  return show_handle_value_tooltip_;
}

QString RangeSlider::handleValueText(bool left) const {
  return QString::number(left ? lower_value_ : upper_value_);
}

void RangeSlider::maybeShowHandleTooltip(const QPoint& global_pos, const QPoint& local_pos) {
  if (!show_handle_value_tooltip_) {
    return;
  }
  bool over_left = type_.testFlag(kLeftHandle) && firstHandleRect().contains(local_pos);
  bool over_right = type_.testFlag(kRightHandle) && secondHandleRect().contains(local_pos);
  if (first_handle_pressed_ && type_.testFlag(kLeftHandle)) {
    over_left = true;
  }
  if (second_handle_pressed_ && type_.testFlag(kRightHandle)) {
    over_right = true;
  }
  if (over_left) {
    QToolTip::showText(global_pos, handleValueText(true), this);
    tooltip_visible_ = true;
  } else if (over_right) {
    QToolTip::showText(global_pos, handleValueText(false), this);
    tooltip_visible_ = true;
  } else if (tooltip_visible_) {
    QToolTip::hideText();
    tooltip_visible_ = false;
  }
}

// --- Real-value convenience API (kept for parity with single-value sliders) ---
int RangeSlider::toInt(double v) const {
  return static_cast<int>(v + 0.5);
}
double RangeSlider::toReal(int v) const {
  return static_cast<double>(v);
}
void RangeSlider::setRangeReal(double min_v, double max_v, int /*decimals*/) {
  setMinimum(static_cast<int>(min_v));
  setMaximum(static_cast<int>(max_v));
}
void RangeSlider::setLowerValueReal(double v) {
  setLowerValue(toInt(v));
}
void RangeSlider::setUpperValueReal(double v) {
  setUpperValue(toInt(v));
}
double RangeSlider::lowerValueReal() const {
  return toReal(lower_value_);
}
double RangeSlider::upperValueReal() const {
  return toReal(upper_value_);
}
int RangeSlider::decimals() const {
  return 0;
}

void RangeSlider::setFloatingLabelsVisible(bool on) {
  floating_labels_ = on;
  update();
}
bool RangeSlider::floatingLabelsVisible() const {
  return floating_labels_;
}

void RangeSlider::setLabelFormatter(std::function<QString(double)> formatter) {
  label_formatter_ = std::move(formatter);
  update();
}
void RangeSlider::setCenterLabelFormatter(std::function<QString(double, double)> formatter) {
  center_label_formatter_ = std::move(formatter);
  update();
}

QString RangeSlider::formatHandleValue(double value) const {
  if (label_formatter_) {
    return label_formatter_(value);
  }
  return handleValueText(value == lowerValueReal());
}

void RangeSlider::drawFloatingLabels(QPainter& painter) {
  lower_label_rect_ = QRect();
  upper_label_rect_ = QRect();
  center_label_rect_ = QRect();

  if (orientation_ != Qt::Horizontal) {
    return;
  }

  painter.setRenderHint(QPainter::Antialiasing);
  QFont label_font = font();
  painter.setFont(label_font);
  QFontMetrics fm(label_font);

  // A disabled slider dims its floating labels, so the dark HUD pills and the accent duration chip read as inactive
  // beside the disabled track and handles, not as a live control.
  painter.save();
  if (!isEnabled()) {
    painter.setOpacity(0.4);
  }

  const int label_height = fm.height() + 6;
  const int handle_top = trackTop();
  const int label_y = handle_top - label_height - 2;

  auto draw_label = [&](const QRectF& handle_rect, const QString& text) -> QRect {
    if (text.isEmpty()) {
      return QRect();
    }
    int text_width = fm.horizontalAdvance(text) + 8;
    int label_x = static_cast<int>(handle_rect.center().x()) - text_width / 2;
    label_x = std::max(0, std::min(label_x, width() - text_width));
    QRect rect(label_x, label_y, text_width, label_height);
    painter.setPen(Qt::NoPen);
    const auto fw_theme = frameworkTheme();
    painter.setBrush(theme::overlay(theme::Overlay::Hud, fw_theme));
    const int radius = theme::radius(theme::Radius::Input, fw_theme);
    painter.drawRoundedRect(rect, radius, radius);
    painter.setPen(theme::text(theme::Theme::Dark));
    painter.drawText(rect, Qt::AlignCenter, text);
    return rect;
  };

  if (type_.testFlag(kLeftHandle)) {
    lower_label_rect_ = draw_label(firstHandleRect(), formatHandleValue(static_cast<double>(lower_value_)));
  }
  if (type_.testFlag(kRightHandle)) {
    upper_label_rect_ = draw_label(secondHandleRect(), formatHandleValue(static_cast<double>(upper_value_)));
  }

  // The selected-duration chip sits on the track, centered between the handles (the start and end labels float above;
  // the duration reads inside the selected fill). Hidden when the handles are too close to fit it.
  if (center_label_formatter_) {
    QString center_text = center_label_formatter_(static_cast<double>(lower_value_), static_cast<double>(upper_value_));
    if (!center_text.isEmpty()) {
      const QRectF left_rect = firstHandleRect();
      const QRectF right_rect = secondHandleRect();
      const double gap_left = left_rect.right();
      const double gap_right = right_rect.left();
      const int text_width = fm.horizontalAdvance(center_text) + 16;
      if (gap_right - gap_left >= text_width + 6) {
        const double cx = (gap_left + gap_right) / 2.0;
        const int chip_h = fm.height() + 4;
        const double track_top = trackTop();
        QRect rect(
            static_cast<int>(cx - text_width / 2.0), static_cast<int>(track_top + (trackHeight() - chip_h) / 2.0),
            text_width, chip_h);
        // Bordered duration chip, using the same selection outline as the range.
        const auto fw_theme = frameworkTheme();
        painter.setPen(QPen(
            theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme),
            theme::stroke(theme::Stroke::Hairline, fw_theme)));
        painter.setBrush(theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme));
        const int radius = theme::radius(theme::Radius::Input, fw_theme);
        painter.drawRoundedRect(rect, radius, radius);
        painter.setPen(theme::onFill(theme::Variant::Accent, theme::State::Checked, fw_theme));
        painter.drawText(rect, Qt::AlignCenter, center_text);
        center_label_rect_ = rect;
      }
    }
  }
  painter.restore();
}

}  // namespace PJ
