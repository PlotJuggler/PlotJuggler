// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "TelemetryIdentity.h"

#include <QCryptographicHash>
#include <QSettings>
#include <QSysInfo>
#include <QUuid>

using namespace Qt::StringLiterals;

namespace PJ::telemetry {

namespace {
// The PJ_INSTALLATION default (see the root CMakeLists.txt): anything that
// release packaging did not stamp. Self-built trees are developers, CI and
// automation rather than installs, and they all carry the in-development
// version, so counting them reports a user base that does not exist.
constexpr auto kUnpackagedInstallation = "source";
}  // namespace

QByteArray machineIdentity() {
  QByteArray machine_id = QSysInfo::machineUniqueId();
  if (machine_id.isEmpty()) {
    QSettings settings;
    const QString fallback_key = u"Telemetry::fallback_id"_s;
    QString stored = settings.value(fallback_key).toString();
    if (stored.isEmpty()) {
      stored = QUuid::createUuid().toString(QUuid::WithoutBraces);
      settings.setValue(fallback_key, stored);
    }
    machine_id = stored.toUtf8();
  }
  return machine_id;
}

QString saltedMachineHash(const QByteArray& salt) {
  const QByteArray salted = machineIdentity() + "|" + salt;
  return QString::fromLatin1(QCryptographicHash::hash(salted, QCryptographicHash::Sha256).toHex());
}

bool isReportingInstallation(const QString& installation) {
  const QString stamp = installation.trimmed();
  return !stamp.isEmpty() && stamp.compare(QLatin1String(kUnpackagedInstallation), Qt::CaseInsensitive) != 0;
}

}  // namespace PJ::telemetry
