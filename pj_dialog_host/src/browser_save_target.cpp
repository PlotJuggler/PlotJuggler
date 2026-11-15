// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "browser_save_target.hpp"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <algorithm>
#include <pj_plugins/host_qt/browser_file_picker_fallback.hpp>
#include <pj_plugins/host_qt/widget_binding.hpp>

namespace PJ {

QString browserSaveName(const FilePickerStart& start, std::string_view default_suffix) {
  QString name = start.name.isEmpty() ? QStringLiteral("export") : start.name;
  if (!default_suffix.empty() && QFileInfo(name).suffix().isEmpty()) {
    name += u'.' + QString::fromUtf8(default_suffix.data(), static_cast<qsizetype>(default_suffix.size()));
  }
  return name;
}

BrowserSaveTarget::BrowserSaveTarget(const QString& file_name)
    : path_(file_name.isEmpty() ? dir_.path() : dir_.filePath(QFileInfo(file_name).fileName())) {
  if (!dir_.isValid()) {
    qWarning("Browser export: cannot create a scratch directory: %s", qPrintable(dir_.errorString()));
  }
}

QString BrowserSaveTarget::downloadName(const QString& written_path) const {
  return QDir(dir_.path()).relativeFilePath(written_path).replace(u'/', u'_');
}

QStringList BrowserSaveTarget::writtenPaths() const {
  QStringList paths;
  if (!dir_.isValid()) {
    return paths;
  }
  QDirIterator it(dir_.path(), QDir::Files, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    paths.append(it.next());
  }
  std::sort(paths.begin(), paths.end(), [this](const QString& a, const QString& b) {
    return downloadName(a) < downloadName(b);
  });
  return paths;
}

void BrowserSaveTarget::offerDownloads() const {
#ifdef Q_OS_WASM
  // Qt's portable download path: see ScopedOpenFilePickerFallback.
  const ScopedOpenFilePickerFallback fallback;
  for (const QString& path : writtenPaths()) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
      QFileDialog::saveFileContent(  // NOLINT(pj-canonical-chrome): Qt's browser download API.
          file.readAll(), downloadName(path));
    }
  }
#endif
}

}  // namespace PJ
