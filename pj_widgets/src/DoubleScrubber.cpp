// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/DoubleScrubber.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace PJ {

DoubleScrubber::DoubleScrubber(QWidget* parent) : ScrubberBase(parent) {}

double DoubleScrubber::clamp(double v) const {
  return std::clamp(v, minimum_, maximum_);
}

void DoubleScrubber::setValue(double v) {
  if (!std::isfinite(v)) {
    return;
  }
  const double clamped = clamp(v);
  if (!std::isfinite(clamped) || clamped == value_) {
    return;
  }
  value_ = clamped;
  emit valueChanged(value_);
  valueRepaint();
}

void DoubleScrubber::setRange(double min, double max) {
  minimum_ = std::min(min, max);
  maximum_ = std::max(min, max);
  setValue(value_);
  valueRepaint();
}

void DoubleScrubber::setMinimum(double v) {
  minimum_ = v;
  if (maximum_ < minimum_) {
    maximum_ = minimum_;
  }
  setValue(value_);
}

void DoubleScrubber::setMaximum(double v) {
  maximum_ = v;
  if (minimum_ > maximum_) {
    minimum_ = maximum_;
  }
  setValue(value_);
}

void DoubleScrubber::setSingleStep(double v) {
  single_step_ = std::max(std::numeric_limits<double>::min(), v);
}

void DoubleScrubber::setDecimals(int d) {
  decimals_ = std::max(0, d);
  valueRepaint();
}

void DoubleScrubber::setSuffix(const QString& s) {
  suffix_ = s;
  valueRepaint();
}

void DoubleScrubber::setPrefix(const QString& s) {
  prefix_ = s;
  valueRepaint();
}

void DoubleScrubber::stepBy(int steps_signed) {
  setValue(value_ + steps_signed * single_step_);
}

QString DoubleScrubber::displayText() const {
  return prefix_ + QString::number(value_, 'f', decimals_) + suffix_;
}

bool DoubleScrubber::commitText(const QString& text) {
  QString stripped = text;
  if (!prefix_.isEmpty() && stripped.startsWith(prefix_)) {
    stripped = stripped.mid(prefix_.size());
  }
  if (!suffix_.isEmpty() && stripped.endsWith(suffix_)) {
    stripped.chop(suffix_.size());
  }
  bool ok = false;
  const double parsed = stripped.trimmed().toDouble(&ok);
  if (!ok || !std::isfinite(parsed)) {
    return false;
  }
  setValue(parsed);
  return true;
}

}  // namespace PJ
