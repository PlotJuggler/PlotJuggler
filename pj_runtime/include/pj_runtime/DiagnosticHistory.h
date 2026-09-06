#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include "pj_base/diagnostic_sink.hpp"

namespace PJ {

class QtDiagnosticBridge;

// UI-facing form of pj_base::Diagnostic: same five fields with QString /
// QDateTime where pj_base uses std::string / system_clock::time_point.
struct DiagnosticRecord {
  DiagnosticLevel level = DiagnosticLevel::kInfo;
  QString source;
  QString id;
  QString message;
  QDateTime timestamp;

  bool operator==(const DiagnosticRecord&) const = default;
};

// Rolling buffer of recent diagnostics. Single source of truth for the
// title-bar bell + popup. Producers emit through pj_base::DiagnosticSink
// (typed into the data pipeline); a QtDiagnosticBridge cross-threads the
// records into Qt; this service stores a capped history (newest-first)
// and re-emits each record for views to subscribe to.
//
// Default cap is kDefaultMaxRecords (200). Setting a smaller cap trims
// the existing buffer; values < 1 are clamped to 1 so the buffer is
// never destroyed entirely.
class DiagnosticHistory : public QObject {
  Q_OBJECT
 public:
  explicit DiagnosticHistory(QObject* parent = nullptr);

  // Subscribe to a bridge so every diagnostic emitted through the sink
  // pipeline lands here. The bridge must outlive `this` (typical: same
  // QObject parent).
  void connectBridge(QtDiagnosticBridge* bridge);

  // Record one diagnostic. Newest entries land at the front of the
  // buffer; entries beyond the cap are dropped from the back. Emits
  // `recorded` synchronously after the trim.
  void record(const DiagnosticRecord& r);
  void record(DiagnosticLevel level, const QString& source, const QString& id, const QString& message);

  // Clear the buffer entirely. Emits `cleared`.
  void clear();

  void setMaxRecords(int n);
  [[nodiscard]] int maxRecords() const;
  [[nodiscard]] int size() const;
  [[nodiscard]] bool isEmpty() const;

  // Snapshot (newest-first).
  [[nodiscard]] QList<DiagnosticRecord> snapshot() const;

  static constexpr int kDefaultMaxRecords = 200;

 signals:
  void recorded(const DiagnosticRecord& r);
  void cleared();

 private:
  void trim();

  QList<DiagnosticRecord> records_;
  int max_records_ = kDefaultMaxRecords;
};

}  // namespace PJ
