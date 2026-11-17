// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

#include "LoadInput.h"

class QWidget;

namespace PJ {

class BrowserFileStore;

class FileSelectionService {
 public:
  struct Selection {
    QString browser_name;
    QByteArray bytes;
  };
  using Callback = std::function<void(Selection)>;

  struct StagedSelection {
    QString browser_name = {};
    std::optional<LoadInput> input = std::nullopt;
    QString error = {};
  };
  using StagedCallback = std::function<void(StagedSelection)>;

  // Returns immediately. An empty name means cancelled; a zero-byte file is a valid selection.
  static void selectFileContent(QWidget* parent, const QString& name_filter, Callback callback);

  // On WebAssembly a browser download: no success/cancel callback and no destination path.
  static void saveFileContent(QWidget* parent, const QByteArray& bytes, const QString& file_name_hint);

  // Stages the pick behind a LoadInput lease. Cancel: no input and no error; a failure carries `error`. The
  // shared store stays valid if the caller dies while the picker is open. In the browser the File is never
  // copied (BrowserPickedFile.h), so its size is bounded by neither the wasm heap nor the tab's memory; call it
  // from a user gesture.
  //
  // `fingerprint` (browser only): kBeforeSelection delays the callback until content_sha256 is computed (a full
  // read); kInBackground reports at once with it empty, and FileLoader computes it while the file loads.
  enum class Fingerprint { kBeforeSelection, kInBackground };
  static void selectAndStageFile(
      QWidget* parent, const QString& name_filter, std::shared_ptr<BrowserFileStore> store, StagedCallback callback,
      Fingerprint fingerprint = Fingerprint::kBeforeSelection);

  // The first (aggregate) QFileDialog filter: the web picker has no filter dropdown, and the per-plugin
  // entries would become redundant File System Access types.
  static QString browserContentFilter(const QString& name_filter);

  // The `accept` attribute for a QFileDialog filter: every `*.ext` of its first entry as `.ext`,
  // comma-separated; empty (any file) when that entry has a bare `*` or no extension.
  static QString browserAcceptList(const QString& name_filter);
};

}  // namespace PJ
