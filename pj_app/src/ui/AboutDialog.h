#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_widgets/Dialog.h"

namespace Ui {
class AboutDialog;
}

namespace PJ {

// App-styled About box (Help ▸ About PlotJuggler…): logo, version from
// QApplication::applicationVersion(), MPL-2.0 notice and repository
// link. The browser build adds a "Third-party licences" link to
// THIRD_PARTY_NOTICES.txt, which scripts/package_wasm.mjs serves at the
// deployment root; the relative href resolves against the page URL.
// Modal; closed via the chrome ✕.
class AboutDialog : public Dialog {
  Q_OBJECT
 public:
  explicit AboutDialog(QWidget* parent = nullptr);
  ~AboutDialog() override;

 private:
  Ui::AboutDialog* ui_;
};

}  // namespace PJ
