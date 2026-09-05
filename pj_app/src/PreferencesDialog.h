#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <vector>

#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/Dialog.h"

class QNetworkAccessManager;

namespace Ui {
// Generated from PreferencesDialog.ui, whose root <class> is PreferencesContent
// (Qt Designer syncs the generated class to the root widget's objectName).
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

  // Stack indices a caller names to land the user on a section instead of
  // writing a literal. Nav rows are built in pagesStack order, so a row's
  // position IS its page index, and the optional Recording page comes last.
  static constexpr int kAppearancePage = 0;
  static constexpr int kPluginsPage = 4;

  // Opens the dialog on the given nav page (stack index), updating both the
  // visible page and the nav-row highlight. Call before exec() to land the user
  // straight on a specific section (e.g. kPluginsPage from the marketplace).
  void showPage(int index);

 signals:
  // The recording folder and write-queue budget were just written to
  // QSettings on OK. The host re-reads them from here, so the next recording
  // picks them up without a restart; a running one keeps its options.
  void recordingSettingsChanged();

 private:
  // Validates the registry-URL field when editing finishes: syntax first, then
  // a reachability probe (async GET; file:// URLs check existence instead;
  // skipped entirely when the system is not online, so an offline user can
  // still store a URL that will work later). The outcome is advisory: a failed
  // check paints the text red (setRegistryUrlError) and never blocks or
  // reverts the value.
  void onRegistryUrlEditingFinished();

  // Toggles the field's red-text error state (QSS keys on the [urlError]
  // dynamic property; the style is re-polished so the change applies live).
  void setRegistryUrlError(bool error);

  Ui::PreferencesContent* ui_;
  Theme& theme_;
  // Last text the URL check ran for — suppresses duplicate probes when
  // editingFinished re-fires with unchanged content (Return spam).
  QString last_checked_registry_url_;
  // Created on first reachability probe; parented to the dialog.
  QNetworkAccessManager* network_ = nullptr;
  // Owns nothing — Qt parentage owns the row widgets. This is just the
  // iteration target for selection updates.
  std::vector<PreferencesNavRow*> nav_rows_;
  // Snapshot taken at construction so Cancel can revert any live
  // preview the user triggered via the toggle / scrubbers.
  QString original_theme_;
  ChromeMetrics original_metrics_;
};

}  // namespace PJ
