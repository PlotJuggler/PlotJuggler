// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>
#include <utility>
#include <vector>

namespace PJ {

// A browser cannot hand a plugin a path to write to. For a save or folder
// picker the host instead gives the plugin a fresh scratch directory in the
// app's in-memory file system, lets it write there during its picker event,
// then offers every file it wrote as a browser download. Plugins write their
// export synchronously from that event, so the files are complete once it
// returns. The scratch directory is removed with this object.
struct FilePickerStart;

// The file name a browser save picker writes: the plugin's suggestion, else "export".
QString browserSaveName(const FilePickerStart& start);

class BrowserSaveTarget {
 public:
  // file_name is the save picker's suggested name; a folder picker passes none
  // and receives the directory itself.
  explicit BrowserSaveTarget(const QString& file_name = {});

  [[nodiscard]] bool isValid() const {
    return dir_.isValid();
  }
  [[nodiscard]] const QString& path() const {
    return path_;
  }

  // Every regular file under the scratch directory, as (file name, bytes),
  // sorted by name.
  [[nodiscard]] std::vector<std::pair<QString, QByteArray>> writtenFiles() const;

  // Starts one browser download per written file (no-op outside the browser).
  void offerDownloads() const;

 private:
  QTemporaryDir dir_;
  QString path_;
};

}  // namespace PJ
