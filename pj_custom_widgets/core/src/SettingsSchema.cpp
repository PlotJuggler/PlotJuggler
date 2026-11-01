// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_custom_widgets/SettingsSchema.h"

#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

std::optional<double> numberOf(const QJsonValue& value) {
  return value.isDouble() ? std::optional<double>(value.toDouble()) : std::nullopt;
}

}  // namespace

std::vector<SettingDecl> parseSettingsSchema(const QString& json) {
  std::vector<SettingDecl> settings;
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  for (const QJsonValue& value : doc.array()) {
    const QJsonObject entry = value.toObject();
    const QString key = entry.value(u"key"_s).toString();
    if (key.isEmpty()) {
      continue;
    }
    settings.push_back(
        SettingDecl{
            .key = key,
            .type = entry.value(u"type"_s).toString(),
            .label = entry.value(u"label"_s).toString(),
            .default_value = entry.value(u"default"_s),
            .min = numberOf(entry.value(u"min"_s)),
            .max = numberOf(entry.value(u"max"_s)),
            .options = entry.value(u"options"_s).toArray(),
        });
  }
  return settings;
}

}  // namespace PJ
