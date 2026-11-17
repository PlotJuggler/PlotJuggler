#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>

#include "pj_widgets/ScrubberBase.h"

namespace PJ {

// Integer variant of the Blender-style scrubber; the API mirrors QSpinBox, so a .ui migrates by promoting the widget.
class IntScrubber : public ScrubberBase {
  Q_OBJECT
  Q_PROPERTY(int value READ value WRITE setValue NOTIFY valueChanged USER true)
  Q_PROPERTY(int minimum READ minimum WRITE setMinimum)
  Q_PROPERTY(int maximum READ maximum WRITE setMaximum)
  Q_PROPERTY(int singleStep READ singleStep WRITE setSingleStep)
  Q_PROPERTY(QString suffix READ suffix WRITE setSuffix)
  Q_PROPERTY(QString prefix READ prefix WRITE setPrefix)
  Q_PROPERTY(int padWidth READ padWidth WRITE setPadWidth)
 public:
  explicit IntScrubber(QWidget* parent = nullptr);

  int value() const {
    return value_;
  }
  int minimum() const {
    return minimum_;
  }
  int maximum() const {
    return maximum_;
  }
  int singleStep() const {
    return single_step_;
  }
  QString suffix() const {
    return suffix_;
  }
  QString prefix() const {
    return prefix_;
  }
  int padWidth() const {
    return pad_width_;
  }

 public slots:
  void setValue(int v);
  void setRange(int min, int max);
  void setMinimum(int v);
  void setMaximum(int v);
  void setSingleStep(int v);
  void setSuffix(const QString& s);
  void setPrefix(const QString& s);
  // Zero-pads the displayed value to at least `w` digits (0 = none); display only, editing and commit parse a plain
  // integer. w=2: 5 shows "05".
  void setPadWidth(int w);

 signals:
  void valueChanged(int v);

 protected:
  void stepBy(int steps_signed) override;
  QString displayText() const override;
  bool commitText(const QString& text) override;

 private:
  int pad_width_ = 0;
  int clamp(int v) const;

  int value_ = 0;
  int minimum_ = 0;
  int maximum_ = 99;
  int single_step_ = 1;
  QString suffix_;
  QString prefix_;
};

}  // namespace PJ
