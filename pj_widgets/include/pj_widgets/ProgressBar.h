#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QProgressBar>

namespace PJ {

// App-styled QProgressBar: a rounded, thick-outlined trough with a light-blue rounded fill and a single-colour centred
// caption ("Loading"). The look comes entirely from the host stylesheet through the class selector `PJ--ProgressBar`
// (`PJ--ProgressBar` and `PJ--ProgressBar::chunk` in resources/stylesheet_{light,dark}.qss).
//
// A subclass with no logic because a bare `QProgressBar { }` rule would restyle every progress bar in the app
// (ProgressDialog's included); a dedicated type gives the stylesheet a selector that scopes the look to opt-in
// instances. That works only because Q_OBJECT gives the class its own meta-object: without it `className()` reports
// "QProgressBar" and the selector silently never matches. Do not drop Q_OBJECT.
//
// The caption is the QProgressBar `format()` string: a literal (`setFormat("Loading")`) for a fixed label, or the
// default "%p%". Value and range still drive the fill width. The text is drawn once, over the fill, in one colour: it
// does not split or invert where the fill crosses behind it (that would need a custom paintEvent, which this variant
// avoids).
class ProgressBar : public QProgressBar {
  Q_OBJECT
 public:
  explicit ProgressBar(QWidget* parent = nullptr);
};

}  // namespace PJ
