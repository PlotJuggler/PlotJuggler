#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Dialog.h"

namespace Ui {
class AboutDialog;
}

namespace PJ {

// The About box. Its "Third-party licences" link shows THIRD_PARTY_NOTICES.txt: embedded in the desktop executable,
// served at the deployment root of the browser build, which also links its source offer.
class AboutDialog : public Dialog {
  Q_OBJECT
 public:
  explicit AboutDialog(QWidget* parent = nullptr);
  ~AboutDialog() override;

 private:
  Ui::AboutDialog* ui_;
};

}  // namespace PJ
