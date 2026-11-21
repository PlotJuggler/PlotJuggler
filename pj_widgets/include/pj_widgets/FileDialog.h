#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCheckBox>
#include <QFileDialog>
#include <QLabel>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

#include "pj_widgets/ChromeMetrics.h"
#include "pj_widgets/Dialog.h"

namespace PJ {

// Frameless wrapper around QFileDialog: embeds a non-native QFileDialog in PJ::Dialog's chrome so file pickers match
// the app (custom title bar, drag to move, edge resize) instead of the native GTK frame. Most call sites use the static
// getOpenFileName / getSaveFileName helpers.
//
// Start-folder guard: a folder on a dead network share blocks QFileDialog's GUI thread for about 80 s, so
// setDirectory() first gives it 2 s to answer off-thread and otherwise stays at the home folder with a notice. Only the
// start folder is guarded; navigating into a dead location from inside the dialog still blocks (Qt's file system model
// is synchronous).
//
// Replace confirmation: a save dialog asks before accepting an existing file with a PJ::MessageBox (Replace, Cancel),
// not Qt's native QMessageBox, so every save picker shows the app's own question.
class FileDialog : public Dialog {
  Q_OBJECT
 public:
  explicit FileDialog(QWidget* parent = nullptr);
  ~FileDialog() override;

  // Forwarders to the embedded QFileDialog; add more as callers need them.
  void setAcceptMode(QFileDialog::AcceptMode mode);
  void setFileMode(QFileDialog::FileMode mode);
  void setNameFilter(const QString& filter);
  // Falls back to the home folder, with a visible notice, when `directory` does not answer within about 2 s.
  void setDirectory(const QString& directory);
  void setDefaultSuffix(const QString& suffix);
  void selectFile(const QString& filename);

  [[nodiscard]] QStringList selectedFiles() const;
  // The first entry of selectedFiles(), empty when nothing is selected.
  [[nodiscard]] QString selectedFile() const;

  // Drop-in replacements for QFileDialog::getOpenFileName / getSaveFileName. A non-empty `selected_name` (a bare file
  // name) is filled in as the suggestion, as QFileDialog does for a `dir` that names a file.
  static QString getOpenFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      const QString& selected_name = QString());
  // Multi-select open (QFileDialog::ExistingFiles): every chosen path, empty on cancel.
  static QStringList getOpenFileNames(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter);
  static QString getSaveFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      const QString& default_suffix = QString(), const QString& selected_name = QString());
  // Drop-in replacement for QFileDialog::getExistingDirectory (Directory mode, ShowDirsOnly): the chosen directory,
  // empty on cancel.
  static QString getExistingDirectory(QWidget* parent, const QString& caption, const QString& dir = QString());

  // Opts in to the platform's own picker for the open, save and folder helpers (not getSaveFileNameWithOptions, whose
  // checkboxes need the embedded picker). Unlike the Qt-drawn picker it lists drives without waiting on disconnected
  // network drives, at the cost of the app's styling. Process-wide; the app applies its Preferences value. No effect
  // unless systemDialogSupported().
  static void setUseSystemDialog(bool enabled);
  // Windows and macOS only: on Linux the native GTK picker crashes on this app's libpng ABI skew
  // (pj_app/CMakeLists.txt, --exclude-libs), and the browser has its own picker.
  [[nodiscard]] static constexpr bool systemDialogSupported() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
  }

  // Test-only: replaces the reachability probe run by setDirectory() and for the persisted sidebar shortcuts (empty
  // restores the default, QFileInfo::exists). The probe runs on a detached thread, so it must not touch Qt objects.
  static void setStartFolderProbeForTest(std::function<bool(const QString&)> probe);
  // Test-only: overrides how long setDirectory() waits for the probe, in ms (negative restores the default).
  static void setStartFolderTimeoutForTest(int timeout_ms);

  struct ExtraOption {
    QString label;
    bool default_checked = false;
  };
  struct SaveResult {
    QString path;
    std::vector<bool> option_states;  // same order/length as the input extras
  };

  // Save dialog that embeds one checkbox per ExtraOption in a row between the file list and the OK and Cancel buttons.
  // Returns the chosen path and the final state of each checkbox; on cancel the path is empty and option_states stays
  // at its defaults.
  static SaveResult getSaveFileNameWithOptions(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
      const std::vector<ExtraOption>& extras);

  // Templated overloads that wire chrome metrics from a signal source, which exposes `chromeMetrics()` returning a
  // ChromeMetrics and a signal `chromeMetricsChanged(const ChromeMetrics&)` (in practice MainWindow). The template
  // keeps pj_widgets decoupled from pj_app.
  template <typename SignalSourceT>
  static QString getOpenFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      SignalSourceT* metrics_source);
  template <typename SignalSourceT>
  static QStringList getOpenFileNames(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      SignalSourceT* metrics_source);
  template <typename SignalSourceT>
  static QString getSaveFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
      SignalSourceT* metrics_source);
  template <typename SignalSourceT>
  static SaveResult getSaveFileNameWithOptions(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
      const std::vector<ExtraOption>& extras, SignalSourceT* metrics_source);

 public slots:
  // Applies chrome metrics (icon size) to the embedded QFileDialog; connect MainWindow::chromeMetricsChanged to it to
  // keep the toolbar icons in step. The templated static helpers prime and connect for you.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private:
  // Appends a horizontal row of checkboxes (one per ExtraOption) to the QFileDialog's QGridLayout, spanning all
  // columns, above the OK and Cancel buttons. Returns the checkboxes in input order so callers read their state on
  // accept; an empty vector when `extras` is empty.
  std::vector<QCheckBox*> embedExtras(const std::vector<ExtraOption>& extras);

  // True when the helpers should hand over to runSystemDialog().
  [[nodiscard]] static bool useSystemDialog();
  // Runs the platform picker modally; the chosen paths, empty on cancel.
  static QStringList runSystemDialog(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, QFileDialog::FileMode mode,
      QFileDialog::AcceptMode accept, const QString& default_suffix = QString(),
      const QString& selected_name = QString());

  QFileDialog* inner_;
  QLabel* start_folder_notice_;
  ChromeMetrics chrome_metrics_;
};

template <typename SignalSourceT>
QString FileDialog::getOpenFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, SignalSourceT* metrics_source) {
  if (useSystemDialog()) {
    return runSystemDialog(parent, caption, dir, filter, QFileDialog::ExistingFile, QFileDialog::AcceptOpen).value(0);
  }
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptOpen);
  dlg.setFileMode(QFileDialog::ExistingFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (metrics_source != nullptr) {
    dlg.onChromeMetricsChanged(metrics_source->chromeMetrics());
    QObject::connect(metrics_source, &SignalSourceT::chromeMetricsChanged, &dlg, &FileDialog::onChromeMetricsChanged);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

template <typename SignalSourceT>
QStringList FileDialog::getOpenFileNames(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, SignalSourceT* metrics_source) {
  if (useSystemDialog()) {
    return runSystemDialog(parent, caption, dir, filter, QFileDialog::ExistingFiles, QFileDialog::AcceptOpen);
  }
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptOpen);
  dlg.setFileMode(QFileDialog::ExistingFiles);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (metrics_source != nullptr) {
    dlg.onChromeMetricsChanged(metrics_source->chromeMetrics());
    QObject::connect(metrics_source, &SignalSourceT::chromeMetricsChanged, &dlg, &FileDialog::onChromeMetricsChanged);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFiles();
}

template <typename SignalSourceT>
QString FileDialog::getSaveFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
    SignalSourceT* metrics_source) {
  if (useSystemDialog()) {
    return runSystemDialog(parent, caption, dir, filter, QFileDialog::AnyFile, QFileDialog::AcceptSave, default_suffix)
        .value(0);
  }
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptSave);
  dlg.setFileMode(QFileDialog::AnyFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (!default_suffix.isEmpty()) {
    dlg.setDefaultSuffix(default_suffix);
  }
  if (metrics_source != nullptr) {
    dlg.onChromeMetricsChanged(metrics_source->chromeMetrics());
    QObject::connect(metrics_source, &SignalSourceT::chromeMetricsChanged, &dlg, &FileDialog::onChromeMetricsChanged);
  }
  if (dlg.exec() != QDialog::Accepted) {
    return {};
  }
  return dlg.selectedFile();
}

template <typename SignalSourceT>
FileDialog::SaveResult FileDialog::getSaveFileNameWithOptions(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
    const std::vector<ExtraOption>& extras, SignalSourceT* metrics_source) {
  FileDialog dlg(parent);
  if (!caption.isEmpty()) {
    dlg.setDialogTitle(caption);
  }
  dlg.setAcceptMode(QFileDialog::AcceptSave);
  dlg.setFileMode(QFileDialog::AnyFile);
  if (!dir.isEmpty()) {
    dlg.setDirectory(dir);
  }
  if (!filter.isEmpty()) {
    dlg.setNameFilter(filter);
  }
  if (!default_suffix.isEmpty()) {
    dlg.setDefaultSuffix(default_suffix);
  }
  if (metrics_source != nullptr) {
    dlg.onChromeMetricsChanged(metrics_source->chromeMetrics());
    QObject::connect(metrics_source, &SignalSourceT::chromeMetricsChanged, &dlg, &FileDialog::onChromeMetricsChanged);
  }
  auto boxes = dlg.embedExtras(extras);

  SaveResult result;
  result.option_states.assign(extras.size(), false);
  for (std::size_t i = 0; i < extras.size(); ++i) {
    result.option_states[i] = extras[i].default_checked;
  }
  if (dlg.exec() != QDialog::Accepted) {
    return result;  // empty path; option_states left at defaults
  }
  const QStringList selected = dlg.selectedFiles();
  if (!selected.isEmpty()) {
    result.path = selected.first();
  }
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    result.option_states[i] = boxes[i]->isChecked();
  }
  return result;
}

}  // namespace PJ
