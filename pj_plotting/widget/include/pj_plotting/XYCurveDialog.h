// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QString>

#include "pj_widgets/Dialog.h"

namespace Ui {
class XYCurveDialog;
}

namespace PJ {

// Modal dialog for creating an XY (scatter) curve from two dragged series: the X and Y names are read-only, a Swap
// button exchanges them, and an editable Name field (auto-suggested) becomes the curve's alias. OK is enabled only when
// the name is non-empty.
class XYCurveDialog : public Dialog {
  Q_OBJECT
 public:
  // x_label / y_label are the human-readable series names to display.
  XYCurveDialog(const QString& x_label, const QString& y_label, QWidget* parent = nullptr);
  ~XYCurveDialog() override;

  // True when the user swapped X and Y relative to the constructor arguments.
  [[nodiscard]] bool swapped() const;
  // The trimmed alias the user chose (the curve title). Non-empty once accepted.
  [[nodiscard]] QString alias() const;

  // Auto-suggested alias: commonPrefix + "[" + suffixX + ";" + suffixY + "]".
  [[nodiscard]] static QString suggestAlias(const QString& x_label, const QString& y_label);

 private:
  void refreshSuggestion();
  void refreshOkState();

  Ui::XYCurveDialog* ui_;
  bool swapped_ = false;
};

}  // namespace PJ
