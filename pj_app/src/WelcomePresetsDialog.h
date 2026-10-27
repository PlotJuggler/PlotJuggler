#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QString>
#include <QUrl>
#include <vector>

#include "PresetController.h"
#include "pj_runtime/PresetCatalog.h"
#include "pj_widgets/Dialog.h"

class QLabel;
class QPushButton;

namespace Ui {
class WelcomePresetsDialog;
}

namespace PJ {

/**
 * @brief The browser welcome dialog: a deployment's presets, each with its thumbnail, plus
 * "Upload your own…".
 *
 * A pure view: it loads nothing. Picking an entry (or selectPreset()) emits `presetSelected` and
 * shows that load in progress, with Cancel (and how much of its data has downloaded, through
 * layoutLoadProgress()), until the shell reports its end through layoutLoadFinished(): success closes the dialog, a
 * failure shows the reason with Retry, a cancel gives the list back. Thumbnails download one at a time through the
 * dialog's own capped PresetController; one that fails or does not decode keeps its placeholder and never blocks the
 * entry. GUI thread only; never runs a nested event loop (the shell shows it with show()).
 */
class WelcomePresetsDialog : public Dialog {
  Q_OBJECT

 public:
  explicit WelcomePresetsDialog(QList<PresetEntry> entries, QWidget* parent = nullptr);
  ~WelcomePresetsDialog() override;

  /// Selects `id` as a click on its entry would: emits presetSelected and shows the load. Ignored
  /// for an id the dialog does not list, or while a load is shown.
  void selectPreset(const QString& id);

  /// Shows `message` as an error with no Retry (a deep link that cannot be honoured).
  void showError(const QString& message);

 public slots:
  /// The end of a layout load by URL (MainWindow::layoutUrlLoadFinished). Ignored unless `url` is
  /// the layout of the preset this dialog is showing as loading.
  void layoutLoadFinished(const QUrl& url, bool ok, const QString& reason);

  /// A download of the load shown (MainWindow::layoutUrlLoadProgress), ignored for any other `url`: fills the bar
  /// with `received` of `total` bytes, or keeps it busy with the count alone when `total` is -1. Once
  /// received == total the download is done and the dialog says the preset is opening, until layoutLoadFinished().
  void layoutLoadProgress(const QUrl& url, qint64 received, qint64 total);

  /// Closing the dialog (✕, Escape, close()) while a load is shown also asks to cancel it.
  /// Overridden here, not closeEvent(): the title-bar ✕ and Escape reach reject() without a
  /// QCloseEvent, and QDialog::closeEvent() itself calls reject().
  void reject() override;

 signals:
  /// `layout_url` is the selected entry's layout, so the shell need not look the id up.
  void presetSelected(QString id, QUrl layout_url);
  /// The dialog has closed itself; the shell opens its data-file picker.
  void uploadRequested();
  /// The user asked to stop the load shown; the dialog keeps showing it until its terminal.
  void cancelRequested();

 private:
  /// Children of the dialog, alive as long as it is.
  struct EntryWidgets {
    QLabel* thumbnail;
    QPushButton* open;
  };

  void addEntryCard(const PresetEntry& entry, int index);
  /// Downloads the next thumbnail from `index` on; the chain ends at the last entry.
  void fetchThumbnails(int index);
  void setThumbnail(int index, const QByteArray& bytes);
  /// Applies loading_id_ / retry_id_ / the status text to every control.
  void updateControls();

  Ui::WelcomePresetsDialog* ui_;
  QList<PresetEntry> entries_;
  std::vector<EntryWidgets> entry_widgets_;  ///< In `entries_` order.
  PresetController thumbnails_;
  QString loading_id_;  ///< The preset whose load is shown; empty when idle.
  QString retry_id_;    ///< The preset Retry repeats; empty when Retry is hidden.
};

}  // namespace PJ
