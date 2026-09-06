// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QLoggingCategory>
#include <QMetaObject>
#include <QPointer>
#include <Qt>

#include "pj_marketplace/qt_diagnostic_bridge.hpp"

namespace PJ {

namespace {
// Console mirror of the title-bar bell: without it a failure such as a rejected
// plugin is invisible on stderr.
Q_LOGGING_CATEGORY(lcDiag, "pj.diagnostics")
}  // namespace

QtDiagnosticBridge::QtDiagnosticBridge(QObject* parent) : QObject(parent) {}

DiagnosticSink QtDiagnosticBridge::sink() {
  QPointer<QtDiagnosticBridge> guard(this);
  return [guard](const Diagnostic& diagnostic) {
    if (diagnostic.level == DiagnosticLevel::kError) {
      qCWarning(lcDiag).noquote() << QString::fromStdString(diagnostic.source) << QString::fromStdString(diagnostic.id)
                                  << QString::fromStdString(diagnostic.message);
    } else if (diagnostic.level == DiagnosticLevel::kWarning) {
      qCInfo(lcDiag).noquote() << QString::fromStdString(diagnostic.source) << QString::fromStdString(diagnostic.id)
                               << QString::fromStdString(diagnostic.message);
    }
    if (!guard) {
      return;
    }
    QMetaObject::invokeMethod(
        guard.data(),
        [guard, diagnostic]() {
          if (!guard) {
            return;
          }
          emit guard->diagnosticReported(
              static_cast<int>(diagnostic.level), QString::fromStdString(diagnostic.source),
              QString::fromStdString(diagnostic.id), QString::fromStdString(diagnostic.message));
        },
        Qt::QueuedConnection);
  };
}

}  // namespace PJ
