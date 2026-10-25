// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/PresetCatalog.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

#include "pj_runtime/HttpUrl.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {

QString tr(const char* text) {
  return QCoreApplication::translate("PresetCatalog", text);
}

// Reads an optional string member; false means it is present but not a string.
bool readString(const QJsonObject& entry, const QString& key, QString* out) {
  const QJsonValue value = entry.value(key);
  if (value.isUndefined() || value.isNull()) {
    return true;
  }
  if (!value.isString()) {
    return false;
  }
  *out = value.toString();
  return true;
}

// A reference must land on http(s): the index is fetched from the page's own origin, but the
// resolved URL is handed to a network stack that would also read file:// natively.
bool resolveReference(const QUrl& index_url, const QString& reference, QUrl* out) {
  const QUrl resolved = index_url.resolved(QUrl(reference));
  if (!isHttpUrl(resolved)) {
    return false;
  }
  *out = resolved;
  return true;
}

}  // namespace

QList<PresetEntry> parsePresetCatalog(const QByteArray& json, const QUrl& index_url, QString* error) {
  QString scratch;
  QString& message = error != nullptr ? *error : scratch;
  message.clear();

  const auto fail = [&message](const QString& reason) {
    message = reason;
    return QList<PresetEntry>{};
  };

  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parse_error);
  if (parse_error.error != QJsonParseError::NoError) {
    return fail(tr("preset index is not valid JSON: %1").arg(parse_error.errorString()));
  }
  const QJsonValue presets = document.object().value(u"presets"_s);
  if (!presets.isArray()) {
    return fail(tr("preset index has no \"presets\" array"));
  }

  QList<PresetEntry> entries;
  QSet<QString> seen_ids;
  int position = 0;
  for (const QJsonValue& item : presets.toArray()) {
    ++position;
    if (!item.isObject()) {
      return fail(tr("preset #%1 is not an object").arg(position));
    }
    const QJsonObject object = item.toObject();
    PresetEntry entry;
    QString layout;
    QString thumbnail;
    if (!readString(object, u"id"_s, &entry.id) || !readString(object, u"title"_s, &entry.title) ||
        !readString(object, u"description"_s, &entry.description) || !readString(object, u"layout"_s, &layout) ||
        !readString(object, u"thumbnail"_s, &thumbnail)) {
      return fail(tr("preset #%1 has a field that is not a string").arg(position));
    }
    if (entry.id.isEmpty()) {
      return fail(tr("preset #%1 has no id").arg(position));
    }
    if (seen_ids.contains(entry.id)) {
      return fail(tr("duplicate preset id \"%1\"").arg(entry.id));
    }
    seen_ids.insert(entry.id);
    if (entry.title.isEmpty()) {
      entry.title = entry.id;
    }
    if (layout.isEmpty() || !resolveReference(index_url, layout, &entry.layout_url)) {
      return fail(tr("preset \"%1\" has no usable http(s) layout").arg(entry.id));
    }
    if (!thumbnail.isEmpty() && !resolveReference(index_url, thumbnail, &entry.thumbnail_url)) {
      return fail(tr("preset \"%1\" has no usable http(s) thumbnail").arg(entry.id));
    }
    entries.append(std::move(entry));
  }
  return entries;
}

const PresetEntry* findPreset(const QList<PresetEntry>& entries, const QString& id) {
  const auto found =
      std::find_if(entries.cbegin(), entries.cend(), [&id](const PresetEntry& entry) { return entry.id == id; });
  return found != entries.cend() ? &*found : nullptr;
}

}  // namespace PJ
