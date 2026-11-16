// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/ScrubberBase.h"

#include <QApplication>
#include <QCursor>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScreen>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QTimer>
#include <QVariantAnimation>
#include <Qt>
#include <cmath>

#include "pj_widgets/FrameworkTokens.h"
#include "pj_widgets/SvgUtil.h"

namespace PJ {

ScrubberBase::ScrubberBase(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::StrongFocus);
  // Cursor is updated per-zone in mouseMoveEvent / enterEvent. Default
  // (un-hovered) is the arrow cursor.
  setAutoFillBackground(false);
  // Hug the text vertically so layouts can't stretch us back up.
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

ScrubberBase::~ScrubberBase() {
  if (state_ == State::kDragging && cursor_hidden_during_drag_) {
    QApplication::restoreOverrideCursor();
  }
}

void ScrubberBase::setPixelsPerStep(int px) {
  pixels_per_step_ = std::max(1, px);
}

int ScrubberBase::styledHeight() const {
  // Ask the active style what a QLineEdit of this font is tall (CT_LineEdit).
  // PJ::Style clamps every input to one height, so this is exactly what the line
  // edits / combos / spin boxes beside the scrubber resolve to — tracking style,
  // font and DPI with no hardcoded constant, and no throwaway widget per call.
  QStyleOptionFrame opt;
  opt.initFrom(this);
  return style()->sizeFromContents(QStyle::CT_LineEdit, &opt, {80, fontMetrics().height()}, this).height();
}

QSize ScrubberBase::sizeHint() const {
  return {80, styledHeight()};
}

QSize ScrubberBase::minimumSizeHint() const {
  return {50, styledHeight()};
}

void ScrubberBase::valueRepaint() {
  update();
}

QRect ScrubberBase::leftArrowRect() const {
  return {0, 0, kArrowZoneWidth, height()};
}

QRect ScrubberBase::rightArrowRect() const {
  return {width() - kArrowZoneWidth, 0, kArrowZoneWidth, height()};
}

QRect ScrubberBase::centerRect() const {
  return {kArrowZoneWidth, 0, width() - 2 * kArrowZoneWidth, height()};
}

ScrubberBase::Zone ScrubberBase::zoneAt(const QPoint& pos) const {
  if (leftArrowRect().contains(pos)) {
    return Zone::kLeftArrow;
  }
  if (rightArrowRect().contains(pos)) {
    return Zone::kRightArrow;
  }
  return Zone::kBody;
}

void ScrubberBase::updateCursorForPos(const QPoint& pos) {
  if (state_ == State::kDragging || state_ == State::kEditing) {
    return;
  }
  switch (zoneAt(pos)) {
    case Zone::kLeftArrow:
    case Zone::kRightArrow:
      setCursor(Qt::PointingHandCursor);
      break;
    case Zone::kBody:
      setCursor(Qt::SizeHorCursor);
      break;
  }
}

QRect ScrubberBase::screenGeometryAt(const QPointF& global_pos) {
  if (auto* s = QGuiApplication::screenAt(global_pos.toPoint())) {
    return s->geometry();
  }
  return {};
}

void ScrubberBase::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);

  const bool light = currentTheme().contains("light");
  const auto fw_theme = theme::themeFor(light);

  // Background fill: full rect. Qt's palette(base) brush varies per platform,
  // so self-painted input chrome reads the same framework token as QSS inputs.
  QPainterPath fill_path;
  const qreal corner_radius = theme::radius(theme::Radius::Input, fw_theme);
  fill_path.addRoundedRect(rect(), corner_radius, corner_radius);
  const QBrush fill_brush(theme::surface(theme::Surface::Input, fw_theme));
  p.fillPath(fill_path, fill_brush);

  // Border stroke: inset by 0.5 px so the 1 px line lands cleanly on
  // pixel boundaries (otherwise antialiasing smears the edge across two
  // rows/columns and the rectangle looks uneven).
  const bool focused = state_ == State::kDragging || state_ == State::kEditing || hasFocus();
  const auto outline_state =
      focused ? theme::OutlineState::Focused : (is_hovered_ ? theme::OutlineState::Hovered : theme::OutlineState::Rest);
  const QColor border = theme::outline(theme::OutlineRole::Interactive, outline_state, fw_theme);
  QPainterPath stroke_path;
  stroke_path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), corner_radius, corner_radius);
  p.setPen(QPen(border, theme::stroke(theme::Stroke::Hairline, fw_theme)));
  p.setBrush(Qt::NoBrush);
  p.drawPath(stroke_path);

  // Centre text (skip while editing — the QLineEdit covers it)
  if (state_ != State::kEditing) {
    p.setPen(theme::onSurface(theme::Surface::Input, theme::Emphasis::Default, fw_theme));
    p.drawText(centerRect(), Qt::AlignCenter, displayText());
  }

  // Arrows: only fade in while hovered
  if (hover_alpha_ > 0.0 && state_ != State::kEditing) {
    const QPixmap& left = loadSvg(":/resources/svg/keyboard_arrow_left_light.svg", currentTheme());
    const QPixmap& right = loadSvg(":/resources/svg/keyboard_arrow_right_light.svg", currentTheme());
    p.setOpacity(hover_alpha_);
    const QSize icon_size(12, 12);
    auto draw = [&](const QPixmap& pm, const QRect& zone) {
      QRect target(QPoint(0, 0), icon_size);
      target.moveCenter(zone.center());
      p.drawPixmap(target, pm);
    };
    draw(left, leftArrowRect());
    draw(right, rightArrowRect());
    p.setOpacity(1.0);
  }
}

void ScrubberBase::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mousePressEvent(event);
    return;
  }
  switch (zoneAt(event->pos())) {
    case Zone::kLeftArrow:
      stepBy(-1);
      startAutoRepeat(-1);
      event->accept();
      return;
    case Zone::kRightArrow:
      stepBy(+1);
      startAutoRepeat(+1);
      event->accept();
      return;
    case Zone::kBody:
      state_ = State::kArmed;
      press_screen_pos_ = event->globalPosition().toPoint();
      last_drag_global_ = event->globalPosition();
      accumulated_pixels_ = 0.0;
      total_move_ = 0.0;
      event->accept();
      return;
  }
}

void ScrubberBase::mouseMoveEvent(QMouseEvent* event) {
  if (state_ == State::kIdle) {
    updateCursorForPos(event->pos());
  }
  if (state_ == State::kArmed) {
    const qreal dx = event->globalPosition().x() - press_screen_pos_.x();
    const qreal dy = event->globalPosition().y() - press_screen_pos_.y();
    if (std::sqrt(dx * dx + dy * dy) > kClickDragThreshold) {
      startDrag();
    }
  }
  if (state_ == State::kDragging) {
    // A platform may lose the release while the pointer is outside the
    // application (tab switch, native menu, window deactivation). The next
    // move carries the authoritative button state; settle instead of keeping
    // an app-wide wasm drag filter alive indefinitely.
    if (!(event->buttons() & Qt::LeftButton)) {
      endDrag();
      event->accept();
      return;
    }
    handleDragMove(event->globalPosition());
    event->accept();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void ScrubberBase::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QWidget::mouseReleaseEvent(event);
    return;
  }
  if (autorepeat_direction_ != 0) {
    stopAutoRepeat();
    emit editingFinished();  // arrow click / autorepeat burst settled
    event->accept();
    return;
  }
  if (state_ == State::kArmed) {
    // No drag happened — treat as click ⇒ enter edit.
    state_ = State::kIdle;
    enterEditMode();
    event->accept();
    return;
  }
  if (state_ == State::kDragging) {
    endDrag();
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void ScrubberBase::mouseDoubleClickEvent(QMouseEvent* event) {
  // Double-click on the body fast-paths into edit. Double-click on an
  // arrow falls through to mousePressEvent so it just steps twice.
  if (event->button() == Qt::LeftButton && state_ != State::kEditing && zoneAt(event->pos()) == Zone::kBody) {
    enterEditMode();
    event->accept();
    return;
  }
  QWidget::mouseDoubleClickEvent(event);
}

void ScrubberBase::enterEvent(QEnterEvent* event) {
  is_hovered_ = true;
  animateHover(true);
  updateCursorForPos(event->position().toPoint());
  QWidget::enterEvent(event);
}

void ScrubberBase::leaveEvent(QEvent* event) {
  is_hovered_ = false;
  animateHover(false);
  QWidget::leaveEvent(event);
}

void ScrubberBase::resizeEvent(QResizeEvent* event) {
  if (line_edit_ && state_ == State::kEditing) {
    line_edit_->setGeometry(centerRect());
  }
  QWidget::resizeEvent(event);
}

void ScrubberBase::keyPressEvent(QKeyEvent* event) {
  // Editor handles its own keys; this only fires when the scrubber itself
  // has focus (not the line edit).
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    enterEditMode();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void ScrubberBase::startDrag() {
  state_ = State::kDragging;
  if (cursor_hidden_during_drag_) {
    QApplication::setOverrideCursor(Qt::BlankCursor);
  }
#ifdef PJ_TARGET_WASM
  // Qt-wasm can't warp the pointer, so a long drag walks the cursor off the
  // widget and the implicit grab stops delivering move/release to us. Watch the
  // app-wide stream instead (mirrors PJ::Dialog's wasm drag) so the release
  // always reaches endDrag() and commits the value.
  qApp->installEventFilter(this);
#endif
  update();
}

void ScrubberBase::endDrag() {
  if (cursor_hidden_during_drag_) {
    QApplication::restoreOverrideCursor();
  }
#ifdef PJ_TARGET_WASM
  qApp->removeEventFilter(this);
#else
  QCursor::setPos(press_screen_pos_);
#endif
  state_ = State::kIdle;
  update();
  emit editingFinished();  // drag gesture settled
}

void ScrubberBase::handleDragMove(const QPointF& global_pos) {
  const qreal dx = global_pos.x() - last_drag_global_.x();
  total_move_ += std::abs(dx);
  accumulated_pixels_ += dx;

  while (accumulated_pixels_ >= pixels_per_step_) {
    stepBy(+1);
    accumulated_pixels_ -= pixels_per_step_;
  }
  while (accumulated_pixels_ <= -pixels_per_step_) {
    stepBy(-1);
    accumulated_pixels_ += pixels_per_step_;
  }

#ifndef PJ_TARGET_WASM
  // Wrap at screen edges so the scrub can continue forever. Qt-wasm can't warp
  // the pointer, so on that target the drag simply tracks real pointer travel
  // (below) — the app-wide filter installed in startDrag keeps events flowing.
  const QRect screen = screenGeometryAt(global_pos);
  if (!screen.isNull()) {
    if (global_pos.x() <= screen.left() + 1) {
      const QPoint warped(screen.right() - 2, static_cast<int>(global_pos.y()));
      QCursor::setPos(warped);
      last_drag_global_ = QPointF(warped);
      return;
    }
    if (global_pos.x() >= screen.right() - 1) {
      const QPoint warped(screen.left() + 2, static_cast<int>(global_pos.y()));
      QCursor::setPos(warped);
      last_drag_global_ = QPointF(warped);
      return;
    }
  }
#endif
  last_drag_global_ = global_pos;
}

QLineEdit* ScrubberBase::ensureLineEdit() {
  if (line_edit_) {
    return line_edit_;
  }
  line_edit_ = new QLineEdit(this);
  line_edit_->setFrame(false);
  line_edit_->setAlignment(Qt::AlignCenter);
  line_edit_->hide();
  // Enter commits; Escape and focus-out both revert (handled in
  // eventFilter — editingFinished is too coarse, it fires for both).
  connect(line_edit_, &QLineEdit::returnPressed, this, [this]() {
    if (state_ == State::kEditing) {
      exitEditMode(/*commit=*/true);
    }
  });
  line_edit_->installEventFilter(this);
  return line_edit_;
}

bool ScrubberBase::eventFilter(QObject* obj, QEvent* event) {
#ifdef PJ_TARGET_WASM
  // Wasm has no pointer-warp and the implicit grab drops move/release once the
  // pointer leaves the widget, so drive the drag from the app-wide stream: the
  // release here is what fires endDrag()→editingFinished and commits the value.
  if (state_ == State::kDragging) {
    if (event->type() == QEvent::MouseMove) {
      auto* mouse_event = static_cast<QMouseEvent*>(event);
      if (!(mouse_event->buttons() & Qt::LeftButton)) {
        endDrag();
        return false;
      }
      handleDragMove(mouse_event->globalPosition());
      return true;
    }
    if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
      endDrag();
      return true;
    }
  }
#endif
  if (state_ == State::kEditing) {
    // Line-edit-targeted: Escape and FocusOut both revert.
    if (obj == line_edit_) {
      if (event->type() == QEvent::FocusOut) {
        exitEditMode(/*commit=*/false);
      } else if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Escape) {
          exitEditMode(/*commit=*/false);
          return true;
        }
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
          // QLineEdit commits via returnPressed, then ignores the key. Left to
          // bubble, it reaches keyPressEvent and reopens the editor just closed;
          // QApplication::notify propagates an ignored event even when filtered.
          line_edit_->event(event);
          event->accept();
          return true;
        }
      }
    }
    // App-wide: any mouse press outside the line edit reverts and falls
    // through to the target widget (we don't consume).
    if (event->type() == QEvent::MouseButtonPress) {
      auto* w = qobject_cast<QWidget*>(obj);
      if (w && line_edit_ && w != line_edit_ && !line_edit_->isAncestorOf(w)) {
        exitEditMode(/*commit=*/false);
      }
    }
  }
  return QWidget::eventFilter(obj, event);
}

void ScrubberBase::enterEditMode() {
  if (state_ == State::kEditing) {
    return;
  }
  QLineEdit* le = ensureLineEdit();
  state_ = State::kEditing;
  le->setText(displayText());
  le->setGeometry(centerRect());
  le->show();
  le->selectAll();
  le->setFocus(Qt::MouseFocusReason);
  // Catch clicks on no-focus widgets (sliders, labels, backdrops) which
  // would otherwise leave focus parked on the line edit and never revert.
  qApp->installEventFilter(this);
  update();
}

void ScrubberBase::exitEditMode(bool commit) {
  if (state_ != State::kEditing) {
    return;
  }
  state_ = State::kIdle;
  qApp->removeEventFilter(this);
  if (line_edit_) {
    if (commit) {
      const QString t = line_edit_->text();
      // commitText returns false on parse error / out-of-range — the
      // widget silently reverts to the prior value, so editingFinished
      // fires only when the edit actually committed a valid value.
      if (commitText(t)) {
        emit editingFinished();
      }
    }
    line_edit_->hide();
  }
  update();
}

void ScrubberBase::startAutoRepeat(int direction) {
  autorepeat_direction_ = direction;
  if (!autorepeat_timer_) {
    autorepeat_timer_ = new QTimer(this);
    autorepeat_timer_->setSingleShot(true);
    connect(autorepeat_timer_, &QTimer::timeout, this, &ScrubberBase::onAutoRepeatTick);
  }
  autorepeat_timer_->setInterval(kAutorepeatDelayMs);
  autorepeat_timer_->start();
}

void ScrubberBase::stopAutoRepeat() {
  autorepeat_direction_ = 0;
  if (autorepeat_timer_) {
    autorepeat_timer_->stop();
  }
}

void ScrubberBase::onAutoRepeatTick() {
  if (autorepeat_direction_ == 0) {
    return;
  }
  stepBy(autorepeat_direction_);
  autorepeat_timer_->setInterval(kAutorepeatIntervalMs);
  autorepeat_timer_->start();
}

void ScrubberBase::setHoverAlphaInternal(qreal alpha) {
  hover_alpha_ = alpha;
  update();
}

void ScrubberBase::animateHover(bool entering) {
  if (!hover_animation_) {
    hover_animation_ = new QVariantAnimation(this);
    hover_animation_->setEasingCurve(QEasingCurve::OutCubic);
    connect(hover_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
      setHoverAlphaInternal(v.toReal());
    });
  }
  hover_animation_->stop();
  hover_animation_->setDuration(kHoverFadeMs);
  hover_animation_->setStartValue(hover_alpha_);
  hover_animation_->setEndValue(entering ? 1.0 : 0.0);
  hover_animation_->start();
}

}  // namespace PJ
