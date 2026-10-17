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

// Frameless wrapper around QFileDialog: embeds a non-native
// QFileDialog inside PJ::Dialog's chrome so file pickers match the
// rest of the app (custom title bar, drag-to-move, edge resize) instead
// of the native GTK frame. Most call sites should use the static
// getOpenFileName / getSaveFileName helpers below.
//
// Start-folder guard: a folder on a dead network share blocks QFileDialog's
// GUI thread for ~80 s, so setDirectory() first gives it 2 s to answer
// off-thread and otherwise stays at the home folder with a notice. Only the
// start folder is guarded; navigating into a dead location from inside the
// dialog still blocks (Qt's file system model is synchronous).
class FileDialog : public Dialog {
  Q_OBJECT
 public:
  explicit FileDialog(QWidget* parent = nullptr);
  ~FileDialog() override;

  // Forwarders to the embedded QFileDialog. Add more as callers need.
  void setAcceptMode(QFileDialog::AcceptMode mode);
  void setFileMode(QFileDialog::FileMode mode);
  void setNameFilter(const QString& filter);
  // Falls back to the home folder (with a visible notice) when `directory` does
  // not answer within ~2 s.
  void setDirectory(const QString& directory);
  void setDefaultSuffix(const QString& suffix);
  void selectFile(const QString& filename);

  [[nodiscard]] QStringList selectedFiles() const;
  // First entry of selectedFiles(), or an empty string when nothing selected.
  [[nodiscard]] QString selectedFile() const;

  // Drop-in replacements for QFileDialog::getOpenFileName / getSaveFileName.
  // A non-empty `selected_name` (a bare file name) is filled in as the
  // suggestion, the way QFileDialog pre-selects a `dir` that names a file.
  static QString getOpenFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      const QString& selected_name = QString());
  // Multi-select open (QFileDialog::ExistingFiles). Returns every chosen path,
  // empty on cancel.
  static QStringList getOpenFileNames(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter);
  static QString getSaveFileName(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter,
      const QString& default_suffix = QString(), const QString& selected_name = QString());
  // Drop-in replacement for QFileDialog::getExistingDirectory (Directory mode +
  // ShowDirsOnly). Returns the chosen directory, empty on cancel.
  static QString getExistingDirectory(QWidget* parent, const QString& caption, const QString& dir = QString());

  // Test-only: replace the reachability probe run by setDirectory() and for
  // the persisted sidebar shortcuts (empty restores the default,
  // QFileInfo::exists). The probe runs on a detached
  // thread, so it must not touch Qt objects.
  static void setStartFolderProbeForTest(std::function<bool(const QString&)> probe);
  // Test-only: override how long setDirectory() waits for the probe, in ms
  // (negative restores the default).
  static void setStartFolderTimeoutForTest(int timeout_ms);

  struct ExtraOption {
    QString label;
    bool default_checked = false;
  };
  struct SaveResult {
    QString path;
    std::vector<bool> option_states;  // same order/length as the input extras
  };

  // Save dialog that embeds one or more checkboxes (one per ExtraOption) in
  // a row between the file list and the OK/Cancel buttons. Returns the
  // chosen path plus the final state of each checkbox. Path is empty on
  // cancel; option_states is left default-checked in that case.
  static SaveResult getSaveFileNameWithOptions(
      QWidget* parent, const QString& caption, const QString& dir, const QString& filter, const QString& default_suffix,
      const std::vector<ExtraOption>& extras);

  // Templated overloads that wire chrome metrics from a signal source.
  // The source must expose `chromeMetrics()` returning a (const?)
  // ChromeMetrics(&) and a Qt signal `chromeMetricsChanged(const
  // ChromeMetrics&)`. In practice that's MainWindow; the template lets
  // pj_widgets stay decoupled from pj_app types.
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
  // Apply chrome metrics (icon size) to the embedded QFileDialog. Connect
  // MainWindow::chromeMetricsChanged to this to keep the toolbar icons
  // in step with the rest of the app — the templated static helpers do
  // both the prime + connect for you.
  void onChromeMetricsChanged(const ChromeMetrics& metrics);

 private:
  // Append a horizontal row of checkboxes (one per ExtraOption) to the
  // QFileDialog's QGridLayout, spanning all columns so it sits cleanly
  // above the OK/Cancel buttons. Returns the checkbox pointers in input
  // order so callers can read state on accept. No-op (and returns an
  // empty vector) when `extras` is empty.
  std::vector<QCheckBox*> embedExtras(const std::vector<ExtraOption>& extras);

  QFileDialog* inner_;
  QLabel* start_folder_notice_;
  ChromeMetrics chrome_metrics_;
};

template <typename SignalSourceT>
QString FileDialog::getOpenFileName(
    QWidget* parent, const QString& caption, const QString& dir, const QString& filter, SignalSourceT* metrics_source) {
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
