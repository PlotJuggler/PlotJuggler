// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "browser_save_target.hpp"

#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <algorithm>
#include <pj_plugins/host_qt/widget_binding.hpp>

namespace PJ {

QString browserSaveName(const FilePickerStart& start) {
  return start.name.isEmpty() ? QStringLiteral("export") : start.name;
}

BrowserSaveTarget::BrowserSaveTarget(const QString& file_name)
    : path_(file_name.isEmpty() ? dir_.path() : dir_.filePath(QFileInfo(file_name).fileName())) {}

std::vector<std::pair<QString, QByteArray>> BrowserSaveTarget::writtenFiles() const {
  std::vector<std::pair<QString, QByteArray>> files;
  if (!dir_.isValid()) {
    return files;
  }
  QDirIterator it(dir_.path(), QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    QFile file(it.next());
    if (file.open(QIODevice::ReadOnly)) {
      files.emplace_back(QFileInfo(file).fileName(), file.readAll());
    }
  }
  std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return files;
}

void BrowserSaveTarget::offerDownloads() const {
#ifdef Q_OS_WASM
  for (const auto& [name, bytes] : writtenFiles()) {
    QFileDialog::saveFileContent(bytes, name);  // NOLINT(pj-canonical-chrome): Qt's browser download API.
  }
#endif
}

}  // namespace PJ
