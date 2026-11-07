#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <algorithm>
#include <cmath>

namespace PJ {

// Double-precision slider: wraps QSlider's integer API with a floating-point
// range.
class RealSlider : public QSlider {
  Q_OBJECT
 public:
  explicit RealSlider(QWidget* parent = nullptr) : QSlider(parent) {
    setLimits(0.0, 1.0, 1);
    connect(this, &QSlider::valueChanged, this, &RealSlider::onValueChanged);
  }

  void setLimits(double min, double max, int steps) {
    min_value_ = min;
    max_value_ = max;
    QSlider::setRange(0, steps);
  }

  double getValue() const {
    const int span = maximum() - minimum();
    if (span <= 0) {
      return min_value_;
    }
    const double ratio = static_cast<double>(value() - minimum()) / static_cast<double>(span);
    return (max_value_ - min_value_) * ratio + min_value_;
  }

  void setRealValue(double val) {
    val = std::clamp(val, min_value_, max_value_);
    if (max_value_ == min_value_) {
      QSlider::setValue(minimum());
      return;
    }
    const double ratio = (val - min_value_) / (max_value_ - min_value_);
    const long pos = std::lround(static_cast<double>(maximum() - minimum()) * ratio + minimum());
    QSlider::setValue(static_cast<int>(pos));
  }

  double getMaximum() const {
    return max_value_;
  }
  double getMinimum() const {
    return min_value_;
  }

  void setRealStepValue(double step) {
    const double ratio = (max_value_ - min_value_) / static_cast<double>(maximum() - minimum());
    const int new_step = std::max(1, static_cast<int>(std::round(step / ratio)));
    QSlider::setSingleStep(new_step);
  }

 signals:
  void realValueChanged(double value);

 protected:
  /// Clicking the groove seeks immediately and starts a normal handle drag.
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() == Qt::LeftButton && isEnabled()) {
      QStyleOptionSlider option;
      initStyleOption(&option);
      const QPoint press = event->position().toPoint();
      const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
      if (!handle.contains(press)) {
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const bool horizontal = orientation() == Qt::Horizontal;
        const int length = horizontal ? handle.width() : handle.height();
        const int start = horizontal ? groove.left() : groove.top();
        const int span = (horizontal ? groove.width() : groove.height()) - length;
        const int position = (horizontal ? press.x() : press.y()) - start - length / 2;
        setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), position, span, option.upsideDown));
      }
    }
    QSlider::mousePressEvent(event);
  }

 private slots:
  void onValueChanged(int value) {
    const int min = minimum();
    const int max = maximum();
    const double ratio = static_cast<double>(value) / static_cast<double>(max - min);
    const double pos_x = (max_value_ - min_value_) * ratio + min_value_;
    emit realValueChanged(pos_x);
  }

 private:
  double min_value_ = 0.0;
  double max_value_ = 1.0;
};

}  // namespace PJ
