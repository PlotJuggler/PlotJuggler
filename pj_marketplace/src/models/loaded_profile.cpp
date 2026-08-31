// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_marketplace/loaded_profile.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>

using namespace Qt::StringLiterals;

namespace PJ {
namespace {

// Bumped only on an incompatible on-disk change; an older/newer schema is read
// as "no profile" rather than misinterpreted.
constexpr int kLoadedProfileSchema = 1;
constexpr QLatin1StringView kSchemaKey{"schema"};
constexpr QLatin1StringView kExtensionsKey{"extensions"};

QString loadedProfilePath(const QString& profiles_root) {
  return QDir(profiles_root).absoluteFilePath(QLatin1StringView(kLoadedProfileFileName));
}

}  // namespace

QMap<QString, QString> readLoadedProfile(const QString& profiles_root) {
  QMap<QString, QString> result;
  QFile file(loadedProfilePath(profiles_root));
  if (!file.open(QIODevice::ReadOnly)) {
    return result;
  }
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    return result;
  }
  const QJsonObject root = document.object();
  if (root.value(kSchemaKey).toInt() != kLoadedProfileSchema) {
    return result;
  }
  const QJsonObject extensions = root.value(kExtensionsKey).toObject();
  for (auto it = extensions.constBegin(); it != extensions.constEnd(); ++it) {
    const QString digest = it.value().toString();
    // A blank id or digest would name no object; skip it rather than record a
    // mapping that can only ever miss.
    if (!it.key().isEmpty() && !digest.isEmpty()) {
      result.insert(it.key(), digest);
    }
  }
  return result;
}

bool writeLoadedProfile(const QString& profiles_root, const QMap<QString, QString>& id_to_digest) {
  if (!QDir().mkpath(profiles_root)) {
    return false;
  }
  QJsonObject extensions;
  for (auto it = id_to_digest.constBegin(); it != id_to_digest.constEnd(); ++it) {
    extensions.insert(it.key(), it.value());
  }
  QJsonObject root;
  root.insert(kSchemaKey, kLoadedProfileSchema);
  root.insert(kExtensionsKey, extensions);

  QSaveFile file(loadedProfilePath(profiles_root));
  if (!file.open(QIODevice::WriteOnly)) {
    return false;
  }
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  return file.commit();
}

}  // namespace PJ
