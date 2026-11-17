#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QPixmap>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QString>
#include <QWidget>

class QLineEdit;
class QTimer;
class QVariantAnimation;

namespace PJ {

// Blender-style horizontal numeric scrubber. Drag the body to scrub the value (the cursor wraps around screen edges, so
// scrubbing never runs out of room); click without dragging for an inline text editor; hover to reveal the left and
// right step arrows. Subclasses provide the value type through stepBy / displayText / commitText.
class ScrubberBase : public QWidget {
  Q_OBJECT
 public:
  explicit ScrubberBase(QWidget* parent = nullptr);
  ~ScrubberBase() override;

  int pixelsPerStep() const {
    return pixels_per_step_;
  }
  void setPixelsPerStep(int px);

  bool cursorHiddenDuringDrag() const {
    return cursor_hidden_during_drag_;
  }
  void setCursorHiddenDuringDrag(bool hidden) {
    cursor_hidden_during_drag_ = hidden;
  }

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  // Emitted once when an interaction that may have changed the value settles: drag release, arrow-button release, or a
  // committed inline keyboard edit. Not per drag tick, and not on an Escape revert. Connect persistence (a QSettings
  // write) here and keep live preview on the subclass `valueChanged`: the value is tracked every tick, this only marks
  // "the user stopped editing".
  void editingFinished();

 protected:
  // Called by the subclass on every drag step and arrow click. The sign of `steps_signed` is the direction; the
  // magnitude is 1 today, the API leaves room for batching.
  virtual void stepBy(int steps_signed) = 0;

  // Subclass-formatted text drawn in the centre.
  virtual QString displayText() const = 0;

  // Parses and applies the edited text. False if invalid: the widget reverts to the prior value silently.
  virtual bool commitText(const QString& text) = 0;

  // Subclasses call it after setting a new value, to repaint.
  void valueRepaint();

  // The height the active QStyle gives a QLineEdit at this font (CT_LineEdit). Drives sizeHint, so scrubbers match the
  // app's input chrome (PJ::Style clamps every input to one height) under any style and DPI, with no hardcoded
  // constant.
  int styledHeight() const;

  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  bool eventFilter(QObject* obj, QEvent* event) override;

 private:
  enum class State { kIdle, kArmed, kDragging, kEditing };
  enum class Zone { kLeftArrow, kRightArrow, kBody };

  QRect leftArrowRect() const;
  QRect rightArrowRect() const;
  QRect centerRect() const;
  Zone zoneAt(const QPoint& pos) const;
  void updateCursorForPos(const QPoint& pos);

  void enterEditMode();
  // refocus: keyboard exits (Return, Escape) hand focus back to the scrubber, since hiding the editor would move it
  // down the focus chain to a sibling. Click and tab-away exits leave focus where the user sent it.
  void exitEditMode(bool commit, bool refocus = false);
  void startDrag();
  void endDrag();

  void startAutoRepeat(int direction);
  void stopAutoRepeat();
  void onAutoRepeatTick();

  void setHoverAlphaInternal(qreal alpha);
  void animateHover(bool entering);

  void handleDragMove(const QPointF& global_pos);
  static QRect screenGeometryAt(const QPointF& global_pos);

  QLineEdit* ensureLineEdit();

  int pixels_per_step_ = 5;
  bool cursor_hidden_during_drag_ = true;
  static constexpr int kClickDragThreshold = 4;
  static constexpr int kAutorepeatDelayMs = 400;
  static constexpr int kAutorepeatIntervalMs = 50;
  static constexpr int kArrowZoneWidth = 16;
  static constexpr int kHoverFadeMs = 120;

  State state_ = State::kIdle;
  bool is_hovered_ = false;
  qreal hover_alpha_ = 0.0;

  QPoint press_screen_pos_;
  QPointF last_drag_global_;
  qreal accumulated_pixels_ = 0.0;
  qreal total_move_ = 0.0;

  QLineEdit* line_edit_ = nullptr;
  QTimer* autorepeat_timer_ = nullptr;
  int autorepeat_direction_ = 0;
  QVariantAnimation* hover_animation_ = nullptr;
};

}  // namespace PJ
