#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QString>
#include <QVariant>

namespace PJ {

/// QSettings().value(key, fallback) for the settings read on hot paths (every
/// paint, every tracker move), remembered after the first read. Constructing a
/// QSettings stats its four conf files: 2-3 ms each time in the browser build,
/// where a layout restore paid ~90 ms for it and playback paid it per frame.
///
/// A cached key must only be written through writeCachedSetting(); a writer that
/// goes through QSettings directly (several keys at once, a remove) calls
/// invalidateCachedSettings() afterwards. The cached keys: kThemeSettingsKey
/// (SvgUtil.h) and kPrecisionSettingsKey.
[[nodiscard]] QVariant cachedSetting(const QString& key, const QVariant& fallback);
void writeCachedSetting(const QString& key, const QVariant& value);
void invalidateCachedSettings();

inline constexpr const char* kPrecisionSettingsKey = "Preferences::precision";

/// The value precision of trackers, hover tooltips and the curve list.
[[nodiscard]] inline int valuePrecision() {
  return cachedSetting(QString::fromLatin1(kPrecisionSettingsKey), 3).toInt();
}

}  // namespace PJ
