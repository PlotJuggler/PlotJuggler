#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QSettings>
#include <QString>
#include <QVariant>

namespace PJ {

// One long-lived default QSettings per thread. Constructing a QSettings costs 2-3 ms in the browser (the emscripten
// file system stats every candidate file); a constructed one reads from memory. It is not a value cache: every
// QSettings on a path shares Qt's in-process file state, so a write, remove or clear through any other QSettings on
// that file, on any thread, is visible here at once (tests/app_settings_test.cpp).
//
// The first call fixes the instance's format and path: in the browser it must come after BrowserPersistence switches
// the default format to its session IniFormat store (pj_app/src/main.cpp). Leaked on purpose, so thread exit and static
// destruction order cannot race it.
[[nodiscard]] inline QSettings& appSettings() {
#ifdef __EMSCRIPTEN__
  Q_ASSERT(QSettings::defaultFormat() == QSettings::IniFormat);
#endif
  thread_local QSettings* settings = new QSettings();
  return *settings;
}

// A default-QSettings value, for hot paths. The browser reads it through appSettings(). The desktop keeps a fresh
// QSettings per read: it is cheap there, and it re-reads the file, so a change saved by another running PlotJuggler
// still reaches this one.
[[nodiscard]] inline QVariant appSetting(const QString& key, const QVariant& fallback) {
#ifdef __EMSCRIPTEN__
  return appSettings().value(key, fallback);
#else
  return QSettings().value(key, fallback);
#endif
}

inline constexpr const char* kPrecisionSettingsKey = "Preferences::precision";

// Significant digits for value text (tracker, hover, curve list).
[[nodiscard]] inline int valuePrecision() {
  return appSetting(QString::fromLatin1(kPrecisionSettingsKey), 3).toInt();
}

}  // namespace PJ
