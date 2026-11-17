// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/DualOptionsWidget.h"

#include <QEasingCurve>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QVariantAnimation>
#include <algorithm>

#include "pj_widgets/FrameworkTokens.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
constexpr auto kHPadding = theme::Space::Comfortable;
constexpr int kAnimationDurationMs = 140;

theme::Theme frameworkTheme() {
  const QColor window = QGuiApplication::palette().color(QPalette::Window);
  return theme::themeFor(window.lightness() >= 128);
}
}  // namespace

DualOptionsWidget::DualOptionsWidget(QWidget* parent) : DualOptionsWidget(u"Option A"_s, u"Option B"_s, parent) {}

DualOptionsWidget::DualOptionsWidget(const QString& opt0, const QString& opt1, QWidget* parent)
    : DualOptionsWidget(QStringList{opt0, opt1}, parent) {}

DualOptionsWidget::DualOptionsWidget(const QStringList& options, QWidget* parent)
    : QWidget(parent),
      // Defaults mirror the framework roles so the widget looks correct before
      // a stylesheet injects the qproperties.
      accent_color_(theme::surface(PJ::theme::Surface::Separation, frameworkTheme())),
      border_color_(theme::surface(PJ::theme::Surface::Separation, frameworkTheme())),
      base_fill_color_(theme::surface(theme::Surface::Input, frameworkTheme())),
      selected_fill_color_(theme::interaction(theme::Variant::Accent, theme::State::Checked, frameworkTheme())),
      text_color_(theme::text(frameworkTheme())) {
  selection_animation_ = new QVariantAnimation(this);
  selection_animation_->setDuration(kAnimationDurationMs);
  selection_animation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(selection_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    visual_selection_ = value.toReal();
    update();
  });

  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::TabFocus);
  setAttribute(Qt::WA_Hover, true);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  setOptions(options);
}

void DualOptionsWidget::setOptions(const QStringList& options) {
  if (options.size() < 2) {
    return;
  }
  options_ = options;
  // The prior enable state survives wherever the index still exists; segments the new option list adds are enabled.
  segment_enabled_.resize(options_.size(), true);
  if (selected_ >= optionCount()) {
    setSelectedIndex(optionCount() - 1);
  }
  updateGeometry();
  update();
}

void DualOptionsWidget::setSegmentEnabled(int index, bool enabled) {
  if (index < 0 || index >= optionCount() || segment_enabled_[index] == enabled) {
    return;
  }
  segment_enabled_[index] = enabled;
  update();
}

bool DualOptionsWidget::isSegmentEnabled(int index) const {
  return index < 0 || index >= optionCount() || segment_enabled_[index];
}

void DualOptionsWidget::setOptions(const QString& opt0, const QString& opt1) {
  setOptions(QStringList{opt0, opt1});
}

void DualOptionsWidget::setSelectedIndex(int index) {
  if (index == selected_ || index < 0 || index >= optionCount() || !isSegmentEnabled(index)) {
    return;
  }
  selected_ = index;
  animateSelectedIndex(index);
  emit selectionChanged(selected_);
}

void DualOptionsWidget::setOrientation(Qt::Orientation orientation) {
  if (orientation_ == orientation) {
    return;
  }
  orientation_ = orientation;
  // Horizontal: fill-width row, fixed height. Vertical: fixed width, fill-height column. Maximum on the growing axis
  // keeps the strip snug to its content.
  setSizePolicy(
      orientation_ == Qt::Horizontal ? QSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed)
                                     : QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Maximum));
  // The stacked height is hard-pinned, so a constrained parent (an aligned sub-layout, which honours minimumSize but
  // not minimumSizeHint) cannot squash the vertical pills below their text. Horizontal keeps its natural single-row
  // height.
  setMinimumHeight(orientation_ == Qt::Vertical ? sizeHint().height() : 0);
  // The app QSS pins PJ--DualOptionsWidget to a single input-row height (min and max-height), which QStyleSheetStyle
  // enforces over the C++ size hints: right for a horizontal strip, fatal for a stacked one. This property lets the
  // stylesheet lift that cap for the vertical case so the height above wins; re-polished so the selector re-evaluates
  // at once.
  setProperty("pjOrientation", orientation_ == Qt::Vertical ? "vertical" : "horizontal");
  if (QStyle* s = style(); s != nullptr) {
    s->unpolish(this);
    s->polish(this);
  }
  updateGeometry();
  update();
}

void DualOptionsWidget::setAccentColor(const QColor& color) {
  if (accent_color_ == color) {
    return;
  }
  accent_color_ = color;
  update();
}

void DualOptionsWidget::setBorderColor(const QColor& color) {
  if (border_color_ == color) {
    return;
  }
  border_color_ = color;
  update();
}

void DualOptionsWidget::setBaseFillColor(const QColor& color) {
  if (base_fill_color_ == color) {
    return;
  }
  base_fill_color_ = color;
  update();
}

void DualOptionsWidget::setSelectedFillColor(const QColor& color) {
  if (selected_fill_color_ == color) {
    return;
  }
  selected_fill_color_ = color;
  update();
}

void DualOptionsWidget::setTextColor(const QColor& color) {
  if (text_color_ == color) {
    return;
  }
  text_color_ = color;
  update();
}

QSize DualOptionsWidget::sizeHint() const {
  const QFontMetrics fm = fontMetrics();
  int max_w = 0;
  for (const QString& option : options_) {
    max_w = std::max(max_w, fm.horizontalAdvance(option));
  }
  const int segment_w = max_w + 2 * theme::space(kHPadding);
  const int segment_h = theme::metric(theme::Metric::InputOuterHeight);
  if (orientation_ == Qt::Horizontal) {
    return {optionCount() * segment_w, segment_h};
  }
  // Vertical: one segment wide (all segments share the widest label), N tall. A stacked column gives each pill its own
  // row; snug breathing room keeps the stack compact enough to pair with a single input-row-scale neighbour (the
  // comfortable horizontal inset would fatten each row well past the row grid).
  const int vertical_segment_h = std::max(segment_h, fm.height() + 2 * theme::space(theme::Space::Snug));
  return {segment_w, optionCount() * vertical_segment_h};
}

QSize DualOptionsWidget::minimumSizeHint() const {
  return sizeHint();
}

bool DualOptionsWidget::event(QEvent* event) {
  switch (event->type()) {
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::HoverEnter:
    case QEvent::HoverLeave:
    case QEvent::FocusIn:
    case QEvent::FocusOut:
      update();
      break;
    default:
      break;
  }
  return QWidget::event(event);
}

void DualOptionsWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  const auto fw_theme = frameworkTheme();
  const qreal border_width = theme::stroke(theme::Stroke::Hairline, fw_theme);
  const qreal corner_radius = theme::radius(theme::Radius::Input, fw_theme);

  const QRectF box =
      QRectF(rect()).adjusted(border_width / 2.0, border_width / 2.0, -border_width / 2.0, -border_width / 2.0);
  // Segment geometry runs along the strip's axis; a fractional position (the animated visual_selection_ for the chip,
  // the integer index for a label) maps to a rect of the full cross-axis thickness.
  const bool horizontal = orientation_ == Qt::Horizontal;
  const qreal segment_extent = (horizontal ? box.width() : box.height()) / optionCount();
  const auto segment_box = [&](qreal pos) -> QRectF {
    return horizontal ? QRectF(box.left() + segment_extent * pos, box.top(), segment_extent, box.height())
                      : QRectF(box.left(), box.top() + segment_extent * pos, box.width(), segment_extent);
  };

  const QColor bg =
      isEnabled() ? base_fill_color_ : theme::interaction(theme::Variant::Neutral, theme::State::Disabled, fw_theme);
  const QColor sel_fill =
      isEnabled() ? selected_fill_color_ : theme::interaction(theme::Variant::Accent, theme::State::Disabled, fw_theme);
  const QColor base_border = !isEnabled()                   ? theme::surface(PJ::theme::Surface::Separation, fw_theme)
                             : (underMouse() || hasFocus()) ? accent_color_
                                                            : border_color_;
  const QColor selected_border = isEnabled() ? accent_color_ : theme::surface(PJ::theme::Surface::Separation, fw_theme);

  painter.setBrush(bg);
  painter.setPen(QPen(base_border, border_width));
  painter.drawRoundedRect(box, corner_radius, corner_radius);

  const QRectF selected_box = segment_box(visual_selection_);
  painter.setBrush(sel_fill);
  painter.setPen(QPen(selected_border, border_width));
  painter.drawRoundedRect(selected_box, corner_radius, corner_radius);

  const QColor disabled_label = theme::onSurface(theme::Surface::Backdrop, theme::Emphasis::Disabled, fw_theme);
  for (int i = 0; i < optionCount(); ++i) {
    // A whole disabled widget, or an individually disabled segment, greys that
    // label so an unavailable option reads as unavailable.
    painter.setPen((isEnabled() && isSegmentEnabled(i)) ? text_color_ : disabled_label);
    painter.drawText(segment_box(static_cast<qreal>(i)), Qt::AlignCenter, options_[i]);
  }
}

void DualOptionsWidget::mousePressEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    const int pos = orientation_ == Qt::Horizontal ? event->pos().x() : event->pos().y();
    const int extent = std::max(1, orientation_ == Qt::Horizontal ? width() : height());
    const int index = std::clamp(static_cast<int>(pos * optionCount() / extent), 0, optionCount() - 1);
    // setSelectedIndex no-ops on a disabled segment, so a click on one is inert.
    setSelectedIndex(index);
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void DualOptionsWidget::keyPressEvent(QKeyEvent* event) {
  switch (event->key()) {
    // Arrow keys navigate along the strip's axis; both pairs are accepted so the
    // control feels natural whether it is laid out horizontally or vertically.
    case Qt::Key_Left:
    case Qt::Key_Up:
      setSelectedIndex(nextEnabledIndex(-1, /*wrap=*/false));
      event->accept();
      break;
    case Qt::Key_Right:
    case Qt::Key_Down:
      setSelectedIndex(nextEnabledIndex(+1, /*wrap=*/false));
      event->accept();
      break;
    case Qt::Key_Space:
    case Qt::Key_Return:
      // Cycles to the next enabled segment; for two options this is the classic
      // toggle.
      setSelectedIndex(nextEnabledIndex(+1, /*wrap=*/true));
      event->accept();
      break;
    default:
      QWidget::keyPressEvent(event);
  }
}

void DualOptionsWidget::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::EnabledChange) {
    update();
  }
}

int DualOptionsWidget::nextEnabledIndex(int step, bool wrap) const {
  const int count = optionCount();
  for (int offset = 1; offset < count; ++offset) {
    int candidate = selected_ + (step * offset);
    if (wrap) {
      candidate = ((candidate % count) + count) % count;
    } else if (candidate < 0 || candidate >= count) {
      break;
    }
    if (isSegmentEnabled(candidate)) {
      return candidate;
    }
  }
  return selected_;
}

void DualOptionsWidget::animateSelectedIndex(int index) {
  const qreal target = static_cast<qreal>(index);
  if (selection_animation_ == nullptr || !isVisible()) {
    visual_selection_ = target;
    update();
    return;
  }

  selection_animation_->stop();
  selection_animation_->setStartValue(visual_selection_);
  selection_animation_->setEndValue(target);
  selection_animation_->start();
}

}  // namespace PJ
