#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

// Header-only and Qt-only so pj_app's test runner without pj_runtime (BrowserPersistence) shares the key layout.

#include <QLatin1StringView>
#include <QString>
#include <QUrl>
#include <string_view>

namespace PJ {

// Persisted names: the pj.settings.v1 store of a plugin (QSettingsBackend::forPlugin) is the app-settings group
// "<kPluginSettingsRoot>/<plugin id, percent-encoded>". Renaming either loses every plugin's stored settings.
inline constexpr QLatin1StringView kPluginSettingsRoot{"PluginSettings"};
// A plugin key "<kDurablePluginSettingsGroup>/<name>" opts into surviving a browser reload (BrowserPersistence,
// bounded strings only); every other plugin key lives as long as the page.
inline constexpr QLatin1StringView kDurablePluginSettingsGroup{"durable"};

// Percent-encoding leaves no '/' in the id, so one plugin's group can never nest inside another's.
[[nodiscard]] inline QString pluginSettingsGroup(std::string_view plugin_id) {
  const QString id = QString::fromUtf8(plugin_id.data(), static_cast<qsizetype>(plugin_id.size()));
  return QString(kPluginSettingsRoot) + u'/' + QString::fromLatin1(QUrl::toPercentEncoding(id));
}

}  // namespace PJ
