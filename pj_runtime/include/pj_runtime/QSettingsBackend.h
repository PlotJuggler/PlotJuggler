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

// Host-side SettingsBackend backed by QSettings, providing the "pj.settings.v1" service (see sdk::SettingsStoreHost).
// Persists to the app's QSettings (the org/app name set on QApplication) or to an explicit INI file for headless hosts
// and tests. Keys use '/', which QSettings maps to nested groups; scalars are stored as strings, lists as a native
// QStringList. Threads, failures and who gets which store: pj_runtime ARCHITECTURE "Plugin settings".
class QSettingsBackend : public sdk::SettingsBackend {
 public:
  // The app's QSettings, unscoped.
  QSettingsBackend();
  // Explicit INI file, unscoped: for headless hosts and tests.
  explicit QSettingsBackend(const QString& ini_path);

  // The store of one plugin: every key lives under pluginSettingsGroup(plugin_id) (PluginSettingsKeys.h), and the
  // first open copies in the root keys the plugin may own (ARCHITECTURE "Plugin settings", "Root keys"). nullptr for
  // an empty id. Empty `ini_path`: the app's QSettings.
  [[nodiscard]] static std::unique_ptr<QSettingsBackend> forPlugin(
      std::string_view plugin_id, const QString& ini_path = {});

  std::optional<std::string> getString(std::string_view key) override;
  void setString(std::string_view key, std::string_view value) override;
  std::optional<std::vector<std::string>> getStringList(std::string_view key) override;
  void setStringList(std::string_view key, const std::vector<std::string>& values) override;
  bool contains(std::string_view key) override;
  void remove(std::string_view key) override;

 private:
  void copyRootKeysOnce(const QString& id_words);
  // The stored key; nullopt for the store's own marker, which no plugin key reaches.
  [[nodiscard]] std::optional<QString> scopedKey(std::string_view key) const;
  void checkThread() const;
  void store(std::string_view key, const QVariant& value);
  [[nodiscard]] std::optional<QVariant> value(std::string_view key);

  QSettings settings_;
  QString prefix_;  // "<plugin group>/"; empty when unscoped
};

}  // namespace PJ
