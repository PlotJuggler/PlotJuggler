#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QSettings>
#include <QString>
#include <QVariant>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pj_base/sdk/settings_store_host.hpp"

namespace PJ {

class ServiceRegistryBuilder;

// Host-side SettingsBackend backed by QSettings, providing the "pj.settings.v1" service (see sdk::SettingsStoreHost).
// Persists to the app's QSettings (the org/app name set on QApplication) or to an explicit INI file for headless hosts
// and tests. Keys use '/', which QSettings maps to nested groups; scalars are stored as strings, lists as a native
// QStringList. Every call throws (a failed PJ_error_t through SettingsStoreHost) off the thread that created the
// backend, and a write throws when QSettings reports it could not store it. Who gets which store: pj_runtime
// ARCHITECTURE "Plugin settings".
class QSettingsBackend : public sdk::SettingsBackend {
 public:
  // The app's QSettings, unscoped.
  QSettingsBackend();
  // Explicit INI file, unscoped: for headless hosts and tests.
  explicit QSettingsBackend(const QString& ini_path);

  // The store of one plugin: every key lives under pluginSettingsGroup(plugin_id) (PluginSettingsKeys.h). A key the
  // store does not hold is read through from the root of the settings when the plugin may own it there, and copied
  // into the store; remove() removes that root copy too. The plugin may own a root key whose first segment (up to the
  // first '/' or '.') is lowercase and, with '_' read as '-', whole words of its id, other than the app's lowercase
  // groups; and every "source_presentation" key (the SDK's presentation records). nullptr for an empty id. Empty
  // `ini_path`: the app's QSettings.
  [[nodiscard]] static std::unique_ptr<QSettingsBackend> forPlugin(
      std::string_view plugin_id, const QString& ini_path = {});

  std::optional<std::string> getString(std::string_view key) override;
  void setString(std::string_view key, std::string_view value) override;
  std::optional<std::vector<std::string>> getStringList(std::string_view key) override;
  void setStringList(std::string_view key, const std::vector<std::string>& values) override;
  bool contains(std::string_view key) override;
  void remove(std::string_view key) override;

 private:
  [[nodiscard]] bool isLegacyKey(const QString& key) const;
  void checkThread() const;
  void store(const QString& key, const QVariant& value);
  [[nodiscard]] std::optional<QVariant> value(const QString& key);

  QSettings settings_;
  QString plugin_id_;  // empty when unscoped: no read-through
};

// Registers plugin `plugin_id`'s store (QSettingsBackend::forPlugin) as pj.settings.v1 in `registry` and returns its
// owner, which must outlive the plugin bound with `registry` (a DataSourceRuntimeHost::bind registrar keeps it so).
// An empty id registers nothing and returns nullptr.
[[nodiscard]] std::shared_ptr<void> registerPluginSettings(
    ServiceRegistryBuilder& registry, std::string_view plugin_id);

}  // namespace PJ
