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

// Stages browser-selected bytes in Emscripten MEMFS behind a lifetime lease.
// The implementation is plain QFile/QDir, which also makes it testable against
// a temporary directory in a native build.
class BrowserFileStore {
 public:
  struct StageResult {
    std::optional<LoadInput> input;
    QString error;
  };
  using StageCompletion = std::function<void(StageResult)>;

  explicit BrowserFileStore(QString root = QStringLiteral("/pj_uploads"));

  // Synchronous staging: writes the whole buffer in one QFile::write. Retained
  // for native code paths and tests. On the browser main thread this blocks the
  // event loop for the full memcpy of a multi-hundred-MB pick, so the wasm
  // picker path uses stageAsync() instead. `bytes` is consumed (moved-from) so
  // the caller's wasm-heap copy can be released as early as this call — combine
  // with std::move at the call site to shed one of the 2-3x peak copies a
  // browser pick otherwise holds (picked buffer + MEMFS + transient ArrayBuffer).
  [[nodiscard]] StageResult stage(const QString& browser_name, QByteArray bytes);

  // Browser-main-thread-friendly staging: writes the buffer in fixed-size
  // chunks, yielding to the event loop between chunks via QTimer::singleShot(0)
  // so a large pick never freezes rendering for the whole memcpy. `completion`
  // runs on the event loop (never re-entrantly from this call) with the same
  // StageResult stage() would return. `bytes` is moved into an internal async
  // job and dropped the moment the last chunk is written — before `completion`
  // fires — so the source copy is gone before the load prologue starts parser
  // work. (QByteArray cannot free partially, so that single wasm-heap copy lives
  // until the final chunk; the JS-side transient ArrayBuffer, released by the
  // caller's move, is gone immediately.) Requires a running Qt event loop.
  //
  // Concurrency: each call captures an independent upload id + directory, so
  // overlapping stagings never collide and need no explicit cancellation — a
  // job whose caller loses interest simply completes into a lease nobody holds,
  // which self-deletes. Safe even if this store is destroyed mid-staging: the
  // job snapshots every value it needs at kickoff and touches the store no more.
  void stageAsync(const QString& browser_name, QByteArray bytes, StageCompletion completion);

  // Places a file the caller writes itself (a download streamed into MEMFS) and returns its input: the path
  // does not exist yet, content_sha256 is the caller's to fill, and the lease removes whatever the path holds.
  [[nodiscard]] StageResult reserve(const QString& browser_name);

  [[nodiscard]] static QString sanitizedBasename(const QString& browser_name);

  // Recovers the browser-provided display name carried in a pj-upload://
  // identity. Returns empty for another scheme or a malformed identity. The
  // backing MEMFS path is intentionally not involved.
  [[nodiscard]] static QString displayNameForIdentity(const QString& identity);

 private:
  // Immutable per-file placement + naming derived once at kickoff, so async jobs
  // are decoupled from the store's lifetime after they start.
  struct StagePlacement {
    QString session_directory;
    QString leaf_directory;
    QString path;
    QString identity;
    QString display_name;
    QString content_sha256;
  };

  // Allocate the next id, build the target directory, and compute the identity.
  // Returns the placement, or an error StageResult when the directory or name
  // is unusable.
  [[nodiscard]] std::optional<StagePlacement> preparePlacement(const QString& browser_name, QString& error_out);

  // Assemble the success LoadInput (with its self-deleting StagedFile lease)
  // from a fully-written, verified placement.
  [[nodiscard]] static StageResult makeSuccess(const StagePlacement& placement);

  QString root_;
  QString session_token_;
  std::atomic<quint64> next_id_{1};
};

}  // namespace PJ
