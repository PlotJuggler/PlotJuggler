#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QSettings>
#include <QString>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/sdk/settings_store_host.hpp"

namespace PJ {

// Host-side SettingsBackend backed by QSettings, providing the "pj.settings.v1" service (see sdk::SettingsStoreHost).
// Persists to the app's configuration (PlotJuggler4.conf, via the org/app name set on QApplication) or to an explicit
// INI file for headless hosts and tests. Keys use '/', which QSettings maps to nested groups; scalars are stored as
// strings, lists as a native QStringList.
class QSettingsBackend : public sdk::SettingsBackend {
 public:
  // App configuration store (PlotJuggler4.conf).
  QSettingsBackend();
  // Explicit INI file — for headless hosts and tests.
  explicit QSettingsBackend(const QString& ini_path);

  // The store of one plugin: every key lives under pluginSettingsGroup(plugin_id) (PluginSettingsKeys.h), so a plugin
  // reads and writes neither another plugin's keys nor the app's own settings. Empty `ini_path`: the app
  // configuration.
  [[nodiscard]] static std::unique_ptr<QSettingsBackend> forPlugin(
      std::string_view plugin_id, const QString& ini_path = {});

  std::optional<std::string> getString(std::string_view key) override;
  void setString(std::string_view key, std::string_view value) override;
  std::optional<std::vector<std::string>> getStringList(std::string_view key) override;
  void setStringList(std::string_view key, const std::vector<std::string>& values) override;
  bool contains(std::string_view key) override;
  void remove(std::string_view key) override;

 private:
  QSettings settings_;
};

}  // namespace PJ
