#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// --dump-diagnostics: accumulates every diagnostic of a QtDiagnosticBridge for the whole run, unbounded (not a
// view over DiagnosticHistory, whose ring is capped), and writes them as JSON. main.cpp calls write() from
// aboutToQuit, which also fires on QCoreApplication::exit(nonzero).
//
// File shape (stable: scripts assert on `id` values, never message text):
//   {"version": 1,
//    "records": [{"level": "info" | "warning" | "error",
//                 "source": "...", "id": "...", "message": "...",
//                 "timestamp": "<ISO 8601 UTC with milliseconds>"}, ...]}
// `version` bumps on any incompatible change. The timestamp is the (queued) delivery time, as in
// DiagnosticHistory. The bridge re-emits queued, so attaching before the event loop first runs also captures
// earlier diagnostics (MainWindow construction).

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QString>

namespace PJ {

class QtDiagnosticBridge;

class DiagnosticDump : public QObject {
  Q_OBJECT

 public:
  explicit DiagnosticDump(QString output_path, QObject* parent = nullptr);

  // One bridge per collector: attaching the same one again is a no-op; a different one adds a subscription but
  // the quit-time drain follows only the latest.
  void attachTo(QtDiagnosticBridge* bridge);

  // `level` is a PJ::DiagnosticLevel cast to int (the bridge's signal shape); unknown values write "info".
  void record(int level, const QString& source, const QString& id, const QString& message);

  [[nodiscard]] int size() const;
  [[nodiscard]] QString outputPath() const;

  // Records oldest first, after delivering the bridge's still-queued diagnostics (the tail of the run).
  [[nodiscard]] bool write();

 private:
  QString output_path_;
  QJsonArray records_;
  // write() delivers only this bridge's queued events, never the whole queue: a quit-time write must not run
  // unrelated shutdown work.
  QPointer<QtDiagnosticBridge> bridge_;
};

}  // namespace PJ
