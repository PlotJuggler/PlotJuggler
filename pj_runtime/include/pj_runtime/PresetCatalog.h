#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

/**
 * @file PresetCatalog.h
 * @brief The static `presets/index.json` a deployment publishes next to the app.
 *
 * A preset is an ordinary layout file; the index only names them. The shape is
 * `{"presets": [{"id", "title", "description"?, "layout", "thumbnail"?}]}`, with
 * `layout` and `thumbnail` resolved against the index URL. There is no version
 * field: unknown keys are ignored, so the format grows by adding keys.
 */

#include <QList>
#include <QString>
#include <QUrl>
#include <QtGlobal>

class QByteArray;

namespace PJ {

/// Ceiling for one small preset file (index, layout or thumbnail): a static host is a trust
/// boundary, not a bulk channel.
inline constexpr qint64 kMaxPresetDownloadBytes = 16 * 1024 * 1024;

/// Ceiling for one preset recording, and for all of one preset's recordings together.
inline constexpr qint64 kMaxPresetDataBytes = 512 * 1024 * 1024;

/// One selectable preset. URLs are absolute http(s); `thumbnail_url` is empty when none was given.
struct PresetEntry {
  QString id;
  QString title;  ///< Falls back to `id` when the index gives none.
  QString description;
  QUrl layout_url;
  QUrl thumbnail_url;
};

/// Parses an index document. Returns the entries in file order (an empty `presets` array is an
/// empty list, not an error). Any malformed input (bad JSON, an entry that is not an object,
/// an empty or non-string id, a duplicate id, a missing `layout`, a reference that is not an
/// http(s) URL once resolved) returns an empty list and describes the first problem in `*error`;
/// `error` may be null and is cleared on success.
QList<PresetEntry> parsePresetCatalog(const QByteArray& json, const QUrl& index_url, QString* error);

/// The entry of `entries` with `id`, or null. Points into `entries`.
[[nodiscard]] const PresetEntry* findPreset(const QList<PresetEntry>& entries, const QString& id);

}  // namespace PJ
