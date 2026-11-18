#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <vector>

#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/Dialog.h"

class QNetworkAccessManager;

namespace Ui {
// Generated from PreferencesDialog.ui: the class is named after its root widget.
class PreferencesContent;
}  // namespace Ui

namespace PJ {

class Theme;
class PreferencesNavRow;

class PreferencesDialog : public Dialog {
  Q_OBJECT
 public:
  explicit PreferencesDialog(Theme& theme, QWidget* parent = nullptr);
  ~PreferencesDialog() override;

  // pagesStack indices. A nav row's position IS its page index, so a hidden page must lose its row too; the
  // optional Recording page comes last.
  static constexpr int kAppearancePage = 0;
  static constexpr int kPluginsPage = 4;

  // Selects the page and its nav row; call before exec().
  void showPage(int index);

 signals:
  // Emitted on OK after the recording and source-cache settings are written to QSettings; a running recording
  // keeps its options.
  void recordingSettingsChanged();

  // Emitted on OK after the derived-markers mode and reload rate are written to QSettings.
  void derivedBatchSettingsChanged();
  // Emitted on OK after MainWindow::kClockUtcKey is written.
  void timeLabelSettingsChanged();
  // Emitted on OK after a changed kPrecisionSettingsKey is written: readers format values only when their time
  // moves, so the receiver must re-seed them.
  void valuePrecisionChanged();

 private:
  // Syntax, then reachability (async GET; existence for file://; skipped while offline). Advisory only: a
  // failure paints the text red and never blocks or reverts the value.
  void onRegistryUrlEditingFinished();

  // Sets the [urlError] dynamic property the QSS keys on, and re-polishes.
  void setRegistryUrlError(bool error);

  Ui::PreferencesContent* ui_;
  Theme& theme_;
  // editingFinished re-fires on unchanged text; this suppresses the duplicate probe.
  QString last_checked_registry_url_;
  QNetworkAccessManager* network_ = nullptr;  // Created on the first probe.
  std::vector<PreferencesNavRow*> nav_rows_;  // Not owned.
  // Cancel reverts the live preview of the theme toggle and scrubbers to these.
  QString original_theme_;
  ChromeMetrics original_metrics_;
};

}  // namespace PJ
