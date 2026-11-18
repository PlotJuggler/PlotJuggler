#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

#include "pj_widgets/ScrubberBase.h"

namespace PJ {

// Floating-point variant of the Blender-style scrubber; the API mirrors QDoubleSpinBox, so a .ui migrates by promoting
// the widget. The value is always finite: setValue() ignores NaN and infinity (and a non-finite bound that would clamp
// to one), and typed "nan" / "inf" is refused like any unparsable text (value kept, no editingFinished), because the
// value drives consumers that cannot take them.
class DoubleScrubber : public ScrubberBase {
  Q_OBJECT
  Q_PROPERTY(double value READ value WRITE setValue NOTIFY valueChanged USER true)
  Q_PROPERTY(double minimum READ minimum WRITE setMinimum)
  Q_PROPERTY(double maximum READ maximum WRITE setMaximum)
  Q_PROPERTY(double singleStep READ singleStep WRITE setSingleStep)
  Q_PROPERTY(int decimals READ decimals WRITE setDecimals)
  Q_PROPERTY(QString suffix READ suffix WRITE setSuffix)
  Q_PROPERTY(QString prefix READ prefix WRITE setPrefix)
 public:
  explicit DoubleScrubber(QWidget* parent = nullptr);

  double value() const {
    return value_;
  }
  double minimum() const {
    return minimum_;
  }
  double maximum() const {
    return maximum_;
  }
  double singleStep() const {
    return single_step_;
  }
  int decimals() const {
    return decimals_;
  }
  QString suffix() const {
    return suffix_;
  }
  QString prefix() const {
    return prefix_;
  }

 public slots:
  void setValue(double v);
  void setRange(double min, double max);
  void setMinimum(double v);
  void setMaximum(double v);
  void setSingleStep(double v);
  void setDecimals(int d);
  void setSuffix(const QString& s);
  void setPrefix(const QString& s);

 signals:
  void valueChanged(double v);

 protected:
  void stepBy(int steps_signed) override;
  QString displayText() const override;
  bool commitText(const QString& text) override;

 private:
  double clamp(double v) const;

  double value_ = 0.0;
  double minimum_ = 0.0;
  double maximum_ = 99.99;
  double single_step_ = 1.0;
  int decimals_ = 2;
  QString suffix_;
  QString prefix_;
};

}  // namespace PJ
