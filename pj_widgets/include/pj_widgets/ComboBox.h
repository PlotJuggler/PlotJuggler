#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QComboBox>

namespace PJ {

// Applies PJ's dropdown styling to an existing QComboBox in place: installs the gradient item delegate and strips the
// popup's native frame and shadow so only the QSS border shows. The core of PJ::ComboBox's constructor, also used to
// upgrade plain QComboBoxes loaded from plugin .ui files without swapping the widget (model, current index and
// connections survive). Idempotent. The popup-overlap nudge of ComboBox::showPopup() is not applied here: it needs the
// subclass.
void applyComboBoxStyling(QComboBox* combo);

// QComboBox that installs PJ::ComboBoxGradientDelegate, so the popup paints the app's light_purple to light_blue
// gradient on the selected or hovered item. Use it instead of QComboBox everywhere, so every dropdown is styled without
// per-call-site wiring. In .ui files, promote QComboBox to PJ::ComboBox (header pj_widgets/ComboBox.h).
class ComboBox : public QComboBox {
  Q_OBJECT
 public:
  explicit ComboBox(QWidget* parent = nullptr);

  // Nudges the popup up so its top border overlaps the closed combo's bottom border instead of stacking next to it (a
  // double line on some compositors).
  void showPopup() override;
};

}  // namespace PJ
