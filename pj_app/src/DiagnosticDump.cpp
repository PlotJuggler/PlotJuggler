// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "DiagnosticDump.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <Qt>
#include <utility>

#include "pj_base/diagnostic_sink.hpp"
#include "pj_marketplace/qt_diagnostic_bridge.hpp"

namespace PJ {

namespace {

[[nodiscard]] QString levelName(int level) {
  switch (static_cast<DiagnosticLevel>(level)) {
    case DiagnosticLevel::kWarning:
      return QStringLiteral("warning");
    case DiagnosticLevel::kError:
      return QStringLiteral("error");
    case DiagnosticLevel::kInfo:
      break;
  }
  return QStringLiteral("info");
}

}  // namespace

DiagnosticDump::DiagnosticDump(QString output_path, QObject* parent)
    : QObject(parent), output_path_(std::move(output_path)) {}

void DiagnosticDump::attachTo(QtDiagnosticBridge* bridge) {
  bridge_ = bridge;
  connect(bridge, &QtDiagnosticBridge::diagnosticReported, this, &DiagnosticDump::record, Qt::UniqueConnection);
}

void DiagnosticDump::record(int level, const QString& source, const QString& id, const QString& message) {
  QJsonObject entry;
  entry.insert(QStringLiteral("level"), levelName(level));
  entry.insert(QStringLiteral("source"), source);
  entry.insert(QStringLiteral("id"), id);
  entry.insert(QStringLiteral("message"), message);
  entry.insert(QStringLiteral("timestamp"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  records_.append(entry);
}

int DiagnosticDump::size() const {
  return static_cast<int>(records_.size());
}

QString DiagnosticDump::outputPath() const {
  return output_path_;
}

bool DiagnosticDump::write() {
  // A diagnostic reported just before the quit (often the failure the dump exists for) is still a posted event
  // on the bridge. Only the bridge's: a quit-time write must not run unrelated shutdown work.
  if (bridge_ != nullptr) {
    QCoreApplication::sendPostedEvents(bridge_, QEvent::MetaCall);
  }
  QFile file(output_path_);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    return false;
  }
  // An envelope, never a bare array, so run-level metadata can be added compatibly.
  QJsonObject envelope;
  envelope.insert(QStringLiteral("version"), 1);
  envelope.insert(QStringLiteral("records"), records_);
  const QByteArray json = QJsonDocument(envelope).toJson(QJsonDocument::Indented);
  return file.write(json) == json.size();
}

}  // namespace PJ
