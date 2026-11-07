#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include <optional>
#include <vector>

namespace PJ {

/// One user-editable setting a widget type declares in its manifest `settings` list.
struct SettingDecl {
  QString key;
  QString type;   ///< "number", "integer", "boolean", "text", "color", "enum", or what the plugin wrote
  QString label;  ///< empty when the manifest gives none
  QJsonValue default_value = QJsonValue::Undefined;  ///< Undefined when the manifest gives no `default`
  std::optional<double> min;                         ///< set only when `min` is a number
  std::optional<double> max;                         ///< set only when `max` is a number
  QJsonArray options;  ///< the `enum` values as declared (strings or numbers, kept as JSON)
};

/// Parses the JSON array of a widget type's settings declaration. Entries that are not objects
/// with a non-empty `key` are dropped; anything but an array gives no declarations. Duplicate keys
/// are kept in order.
[[nodiscard]] std::vector<SettingDecl> parseSettingsSchema(const QString& json);

}  // namespace PJ
