#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QIcon>
#include <QPointer>
#include <QPropertyAnimation>
#include <QWidget>

namespace PJ {

// iOS-style pill toggle: a circular thumb slides between two end positions inside a rounded track; clicking anywhere on
// the widget (or Space or Enter while focused) flips the state.
//
// The two slots, the halves of the track on either side of the thumb, are paintable. The default draws the optional
// leftIcon() / rightIcon() centered in each slot, which covers the common case (sun and moon, off and on glyphs).
// Subclass and override paintLeftSlot / paintRightSlot for custom backgrounds (text, gradients, compositions).
//
// Geometry follows the widget's size: the thumb diameter is `height - 2*kThumbMargin`, slots are width/2 each. The
// default sizeHint() is a compact 34x18, and the constructor pins a Fixed size policy so a form or grid field column
// cannot stretch it; use setFixedSize for a larger switch (an icon-bearing toggle).
//
// Optional inline label: setText() makes it a QCheckBox-like control: the switch shrinks to a fixed pill on one side
// and the text fills the rest, labelSide() choosing the side (default Right, switch then text like a checkbox; Left
// puts the label first and the switch on the right edge, the settings-row look). With no text the widget paints and
// sizes as the bare switch. The whole widget stays clickable, so clicking the label toggles too.
class ToggleSwitch : public QWidget {
  Q_OBJECT
  Q_PROPERTY(bool checked READ isChecked WRITE setChecked NOTIFY toggled)
  Q_PROPERTY(qreal thumbPosition READ thumbPosition WRITE setThumbPosition)
  Q_PROPERTY(QString text READ text WRITE setText)
  Q_PROPERTY(LabelSide labelSide READ labelSide WRITE setLabelSide)

 public:
  enum class LabelSide { Left, Right };
  Q_ENUM(LabelSide)

  explicit ToggleSwitch(QWidget* parent = nullptr);
  ~ToggleSwitch() override;

  bool isChecked() const {
    return checked_;
  }

  // Optional inline label; empty (the default) keeps the bare switch.
  QString text() const {
    return text_;
  }
  void setText(const QString& text);

  LabelSide labelSide() const {
    return label_side_;
  }
  void setLabelSide(LabelSide side);

  QIcon leftIcon() const {
    return left_icon_;
  }
  void setLeftIcon(const QIcon& icon);

  QIcon rightIcon() const {
    return right_icon_;
  }
  void setRightIcon(const QIcon& icon);

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 public slots:
  // Animated by default; `animate = false` is for programmatic initialization (restoring state from settings): the
  // thumb snaps to its endpoint without the 180 ms slide and without emitting `toggled`.
  void setChecked(bool checked, bool animate);
  void setChecked(bool checked) {
    setChecked(checked, true);
  }
  void toggle();

 signals:
  void toggled(bool checked);
  void clicked();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void changeEvent(QEvent* event) override;

  // Slot rects sit inside the thumb margin, one half of the track each, and are static (independent of thumb position).
  // `thumb_position` is 0.0 with the thumb fully left, 1.0 fully right, animating through intermediate values.
  //
  // The default paints the configured QIcon for the slot, faded so only the icon opposite the thumb shows: the thumb
  // covers one slot at each end, the other side shows its icon at full opacity, and during the animation the two icons
  // cross-fade. Override to draw arbitrary backgrounds (text, gradients, compositions, opacity-independent content).
  virtual void paintLeftSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position);
  virtual void paintRightSlot(QPainter& painter, const QRect& slot_rect, qreal thumb_position);

 private:
  qreal thumbPosition() const {
    return thumb_position_;
  }
  void setThumbPosition(qreal pos);

  // The switch's pill region: with no label the whole widget (rect()), so every geometry helper reduces to the bare
  // formulas; with a label a fixed-width pill flush to the side opposite the label.
  QRect trackRect() const;
  // The area left for the label (empty when text_ is empty).
  QRect labelRect() const;
  QRect thumbRect() const;
  QRect leftSlotRect() const;
  QRect rightSlotRect() const;

  bool checked_ = false;
  qreal thumb_position_ = 0.0;  // 0.0 = thumb on left, 1.0 = thumb on right
  QString text_;
  LabelSide label_side_ = LabelSide::Right;
  QIcon left_icon_;
  QIcon right_icon_;
  QPointer<QPropertyAnimation> anim_;
};

}  // namespace PJ
