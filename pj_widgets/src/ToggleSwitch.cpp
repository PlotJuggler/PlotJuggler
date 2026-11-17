// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ToggleSwitch.h"

#include <QEasingCurve>
#include <QEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"

namespace PJ {

namespace {
constexpr int kDefaultWidth = 34;
constexpr int kDefaultHeight = 18;
constexpr int kThumbMargin = 3;  // gap between thumb and track edge
constexpr int kAnimationMs = 180;
constexpr int kIconInset = 4;     // padding between slot rect and icon
constexpr int kLabelSpacing = 6;  // gap between the switch pill and its label
// Breathing room reserved for the label beyond its exact text advance. Without it the size hint fits the text to the
// pixel, and any sub-pixel, DPI or font-render difference between the size-hint metrics and the paint (or a layout that
// shaves a pixel) elides the label.
constexpr int kLabelMargin = 8;
// Gap between a right-anchored switch (Left-side label) and the widget's right edge, so a row-filling toggle sits a
// touch off the panel edge. Button groups use the same inset to line up on the right.
constexpr int kRightInset = 8;

theme::Theme frameworkTheme() {
  // The theme comes from the app-global palette, not the widget's: under the app stylesheet QStyleSheetStyle clobbers
  // per-widget palettes, so widget->palette() can report the wrong theme (a white label in light mode).
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}
}  // namespace

ToggleSwitch::ToggleSwitch(QWidget* parent)
    : QWidget(parent), anim_(new QPropertyAnimation(this, "thumbPosition", this)) {
  setFocusPolicy(Qt::TabFocus);
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_Hover, true);
  // Pinned to the compact default size, so a form or grid field column never stretches the switch; callers wanting
  // another size use setFixedSize (the larger theme toggle in PreferencesDialog).
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  anim_->setDuration(kAnimationMs);
  anim_->setEasingCurve(QEasingCurve::OutCubic);
  // toggled fires only when the slide animation reaches its end: rapid clicking interrupts the animation (anim_->stop()
  // inside setChecked) and QPropertyAnimation::finished is not emitted on an interrupted stop, so a listener sees only
  // the final settled state, as iOS UISwitch's "Value Changed", and expensive listeners (the theme switch) do not run
  // once per click in a rapid burst.
  connect(anim_, &QPropertyAnimation::finished, this, [this]() { emit toggled(checked_); });
}

ToggleSwitch::~ToggleSwitch() = default;

void ToggleSwitch::setChecked(bool checked, bool animate) {
  if (checked_ == checked) {
    // Even on a no-op the thumb is anchored at the right endpoint, in case an interrupted animation left
    // thumb_position_ intermediate.
    const qreal target = checked_ ? 1.0 : 0.0;
    if (anim_->state() != QAbstractAnimation::Running && thumb_position_ != target) {
      thumb_position_ = target;
      update();
    }
    return;
  }
  checked_ = checked;
  anim_->stop();
  if (animate) {
    anim_->setStartValue(thumb_position_);
    anim_->setEndValue(checked_ ? 1.0 : 0.0);
    anim_->start();
    // toggled is emitted from anim_->finished (see the constructor), not here: it fires once the thumb has settled, so
    // an interrupted animation does not emit it.
  } else {
    // Programmatic init path: snap to the endpoint, no animation,
    // no `toggled` emit.
    thumb_position_ = checked_ ? 1.0 : 0.0;
    update();
  }
}

void ToggleSwitch::toggle() {
  setChecked(!checked_);
}

void ToggleSwitch::setText(const QString& text) {
  if (text_ == text) {
    return;
  }
  text_ = text;
  updateGeometry();
  update();
}

void ToggleSwitch::setLabelSide(LabelSide side) {
  if (label_side_ == side) {
    return;
  }
  label_side_ = side;
  update();
}

void ToggleSwitch::setLeftIcon(const QIcon& icon) {
  left_icon_ = icon;
  update();
}

void ToggleSwitch::setRightIcon(const QIcon& icon) {
  right_icon_ = icon;
  update();
}

QSize ToggleSwitch::sizeHint() const {
  if (text_.isEmpty()) {
    return {kDefaultWidth, kDefaultHeight};
  }
  // The switch pill keeps the default 34x18 proportions; the label adds its advance width plus a gap. The height grows
  // only if the font needs more.
  const int height = std::max(kDefaultHeight, fontMetrics().height());
  const int inset = (label_side_ == LabelSide::Left) ? kRightInset : 0;
  const int width = kDefaultWidth + kLabelSpacing + fontMetrics().horizontalAdvance(text_) + kLabelMargin + inset;
  return {width, height};
}

QSize ToggleSwitch::minimumSizeHint() const {
  return sizeHint();
}

void ToggleSwitch::setThumbPosition(qreal pos) {
  thumb_position_ = pos;
  update();
}

QRect ToggleSwitch::trackRect() const {
  if (text_.isEmpty()) {
    return rect();
  }
  // A fixed-width pill near the edge opposite the label; a Left-side label anchors it to the right, kRightInset off the
  // edge. The pill never grows past the height it asks for and centres in the box it is given, so a layout that
  // stretches the widget (a header band sizes its controls to the band height) gets a taller hit area, not a fat oval.
  const int track_w = kDefaultWidth;
  const int track_h = std::min(height(), sizeHint().height());
  const int x = (label_side_ == LabelSide::Left) ? width() - track_w - kRightInset : 0;
  return {x, (height() - track_h) / 2, track_w, track_h};
}

QRect ToggleSwitch::labelRect() const {
  if (text_.isEmpty()) {
    return {};
  }
  const QRect track = trackRect();
  if (label_side_ == LabelSide::Left) {
    return {0, 0, track.left() - kLabelSpacing, height()};
  }
  return {track.right() + 1 + kLabelSpacing, 0, width() - (track.right() + 1 + kLabelSpacing), height()};
}

QRect ToggleSwitch::thumbRect() const {
  const QRect track = trackRect();
  const int diameter = track.height() - (2 * kThumbMargin);
  const int x_left = track.left() + kThumbMargin;
  const int x_right = track.left() + track.width() - kThumbMargin - diameter;
  const int x = x_left + static_cast<int>(thumb_position_ * (x_right - x_left));
  return {x, track.top() + kThumbMargin, diameter, diameter};
}

QRect ToggleSwitch::leftSlotRect() const {
  // The slot is the square the thumb occupies when fully left. Sized and positioned as the thumb, whatever is painted
  // here lines up with the thumb's center at any widget width or height.
  const QRect track = trackRect();
  const int diameter = track.height() - (2 * kThumbMargin);
  return {track.left() + kThumbMargin, track.top() + kThumbMargin, diameter, diameter};
}

QRect ToggleSwitch::rightSlotRect() const {
  // Square at the thumb's RIGHT end position; mirror of leftSlotRect.
  const QRect track = trackRect();
  const int diameter = track.height() - (2 * kThumbMargin);
  return {track.left() + track.width() - kThumbMargin - diameter, track.top() + kThumbMargin, diameter, diameter};
}

void ToggleSwitch::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Track: a pill (corner radius half the height). The fill interpolates from the "off" tone to the "on" tone as the
  // thumb moves, so the color transition tracks the animation.
  const auto fw_theme = frameworkTheme();
  const QColor off_track = theme::interaction(theme::Variant::Neutral, theme::State::Nominal, fw_theme);
  // Darker accent (checked) for the "on" track, so the filled state reads clearly
  // as checked rather than a resting nominal fill.
  const QColor on_track = theme::interaction(theme::Variant::Accent, theme::State::Checked, fw_theme);
  const auto lerp = [](int a, int b, qreal t) { return static_cast<int>(a + ((b - a) * t)); };
  const QColor track_color(
      lerp(off_track.red(), on_track.red(), thumb_position_),
      lerp(off_track.green(), on_track.green(), thumb_position_),
      lerp(off_track.blue(), on_track.blue(), thumb_position_));

  const QRect track = trackRect();
  const qreal radius = track.height() / 2.0;
  painter.setPen(Qt::NoPen);
  painter.setBrush(track_color);
  painter.drawRoundedRect(track, radius, radius);

  // Optional inline label, faded when disabled, aligned toward the switch and
  // vertically centred.
  if (!text_.isEmpty()) {
    painter.setPen(
        isEnabled() ? theme::text(fw_theme)
                    : theme::onSurface(theme::Surface::Backdrop, theme::Emphasis::Disabled, fw_theme));
    const QRect lr = labelRect();
    // The text is left-aligned within its rect, so the label's left edge sits at the widget's leading edge, flush with
    // sibling form labels. (Right-aligning a Left-side label pushes the text right by the sizeHint's anti-elide slack,
    // indenting adapted-checkbox rows past the other labels in a form.)
    const int align = Qt::AlignLeft | Qt::AlignVCenter;
    const QString elided = fontMetrics().elidedText(text_, Qt::ElideRight, lr.width());
    painter.drawText(lr, align, elided);
  }

  // Both slot backgrounds are always painted; the thumb composites on top. The default implementations fade each by
  // thumb_position_, so only the icon opposite the thumb is visible at rest.
  paintLeftSlot(painter, leftSlotRect(), thumb_position_);
  paintRightSlot(painter, rightSlotRect(), thumb_position_);

  // Thumb: raised circle with a faint outer outline for definition.
  const QRect thumb = thumbRect();
  painter.setPen(
      QPen(theme::surface(PJ::theme::Surface::Separation, fw_theme), theme::stroke(theme::Stroke::Hairline, fw_theme)));
  painter.setBrush(theme::interaction(theme::Variant::Neutral, theme::State::Nominal, fw_theme));
  painter.drawEllipse(thumb);
}

void ToggleSwitch::paintLeftSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position) {
  if (left_icon_.isNull()) {
    return;
  }
  // The left icon is visible when the thumb has moved right (away from it). A symmetric inset preserves the slot's true
  // center: slot.center() plus an offset would lose half a pixel to QRect's integer center floor when the slot has an
  // even width or height, drifting the icon 1px up and left of the thumb. adjusted(d, d, -d, -d) shifts both edges
  // identically.
  const int inset = kIconInset / 2;
  const QRect target = slot_rect.adjusted(inset, inset, -inset, -inset);
  painter.save();
  painter.setOpacity(thumb_position);
  painter.drawPixmap(target, left_icon_.pixmap(target.width(), target.height()));
  painter.restore();
}

void ToggleSwitch::paintRightSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position) {
  if (right_icon_.isNull()) {
    return;
  }
  // Right icon is visible when the thumb is LEFT (away from it).
  const int inset = kIconInset / 2;
  const QRect target = slot_rect.adjusted(inset, inset, -inset, -inset);
  painter.save();
  painter.setOpacity(1.0 - thumb_position);
  painter.drawPixmap(target, right_icon_.pixmap(target.width(), target.height()));
  painter.restore();
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
    toggle();
    emit clicked();
  }
  QWidget::mouseReleaseEvent(event);
}

void ToggleSwitch::keyPressEvent(QKeyEvent* event) {
  const int key = event->key();
  if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter) {
    toggle();
    emit clicked();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void ToggleSwitch::changeEvent(QEvent* event) {
  // Whenever the widget re-enters the interactive state (typically after an external setEnabled(true) that unlocked a
  // click and theme-apply cycle), the thumb is forced to the canonical endpoint of checked_ if no animation is running.
  // A no-op normally (the animation that fired `toggled` ended at the endpoint), but if anything left thumb_position_
  // intermediate (an interrupted animation, a programmatic poke, a dialog destroyed mid-slide and reopened) the next
  // interactive moment heals the visual state.
  if (event->type() == QEvent::EnabledChange && isEnabled() && anim_->state() != QAbstractAnimation::Running) {
    const qreal target = checked_ ? 1.0 : 0.0;
    if (thumb_position_ != target) {
      thumb_position_ = target;
      update();
    }
  }
  QWidget::changeEvent(event);
}

}  // namespace PJ
