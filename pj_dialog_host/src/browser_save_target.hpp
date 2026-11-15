// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <string_view>

namespace PJ {

// A browser cannot hand a plugin a path to write to. For a save or folder
// picker the host instead gives the plugin a fresh scratch directory in the
// app's in-memory file system, lets it write there during its picker event,
// then offers every file it wrote as a browser download. Only toolboxes (their
// PanelEngine) get this: their save and folder pickers are export targets.
// Plugins write their export synchronously from that event, so the files are
// complete once it returns. The scratch directory is removed with this object.
struct FilePickerStart;

// The file name a browser save picker writes: the plugin's suggestion, else
// "export", with the plugin's default suffix appended when it has none (as the
// desktop save dialog does).
QString browserSaveName(const FilePickerStart& start, std::string_view default_suffix);

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

  // Every regular file under the scratch directory, sorted by download name.
  [[nodiscard]] QStringList writtenPaths() const;

  // The browser download name of a written file: its path relative to the
  // scratch directory, folders joined with '_' (a download name has no folders).
  [[nodiscard]] QString downloadName(const QString& written_path) const;

  // Starts one browser download per written file, reading one file at a time
  // (no-op outside the browser).
  void offerDownloads() const;

 private:
  QTemporaryDir dir_;
  QString path_;
};

}  // namespace PJ
