// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>
#include <functional>
#include <optional>

#include "LoadInput.h"

namespace PJ {

// Stages browser-selected bytes in Emscripten MEMFS behind a lifetime lease. Plain QFile/QDir, so a native
// build tests it against a temporary directory.
class BrowserFileStore {
 public:
  struct StageResult {
    std::optional<LoadInput> input;
    QString error;
  };
  using StageCompletion = std::function<void(StageResult)>;

  explicit BrowserFileStore(QString root = QStringLiteral("/pj_uploads"));

  // Writes the whole buffer in one QFile::write: on the browser main thread it blocks the event loop for the
  // whole copy, so browser picks use stageAsync() or reserve(). `bytes` is consumed.
  [[nodiscard]] StageResult stage(const QString& browser_name, QByteArray bytes);

  // Writes in chunks, yielding to the event loop between them, so a large pick does not freeze rendering.
  // `completion` runs later on the event loop, never re-entrantly, with what stage() would return; `bytes` is
  // released after the last chunk and before `completion`, so the copy is gone before parsing starts. Requires
  // a running event loop. Each call gets its own upload id and directory, so overlapping stagings never collide;
  // the job copies what it needs at kickoff, so the store may be destroyed mid-staging. There is no cancel: a job
  // whose caller lost interest completes into a lease nobody holds, which deletes itself.
  void stageAsync(const QString& browser_name, QByteArray bytes, StageCompletion completion);

  // Places a file the caller creates itself (a download streamed into MEMFS, a lazily mounted pick) and returns
  // its input: the path does not exist yet, content_sha256 is the caller's to fill, and the lease removes whatever
  // the path holds.
  [[nodiscard]] StageResult reserve(const QString& browser_name);

  [[nodiscard]] static QString sanitizedBasename(const QString& browser_name);

  // The display name carried in a pj-upload:// identity; empty for another scheme or a malformed identity.
  [[nodiscard]] static QString displayNameForIdentity(const QString& identity);

 private:
  // Computed once at kickoff, so an async job never touches the store after it starts.
  struct StagePlacement {
    QString session_directory;
    QString leaf_directory;
    QString path;
    QString identity;
    QString display_name;
    QString content_sha256;
  };

  [[nodiscard]] std::optional<StagePlacement> preparePlacement(const QString& browser_name, QString& error_out);

  [[nodiscard]] static StageResult makeSuccess(const StagePlacement& placement);

  QString root_;
  QString session_token_;
  std::atomic<quint64> next_id_{1};
};

}  // namespace PJ
