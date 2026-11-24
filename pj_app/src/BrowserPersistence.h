// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <memory>
#include <optional>

class QSettings;

namespace PJ {

// The only writer of durable browser storage. On WASM the default QSettings backend is redirected to
// page-lifetime MEMFS, so paths, plugin configs, credentials and extension settings never become durable; this
// bridge alone owns WebLocalStorageFormat and mirrors a schema-validated preference allowlist plus source-free
// layout recipes. Native builds never construct it.
class BrowserPersistence final : public QObject {
  friend class BrowserPersistenceTestPeer;

 public:
  struct LayoutRecipe {
    QString id;
    QString name;
    QByteArray xml;
  };

  explicit BrowserPersistence(QObject* parent = nullptr);
  ~BrowserPersistence() override;

  BrowserPersistence(const BrowserPersistence&) = delete;
  BrowserPersistence& operator=(const BrowserPersistence&) = delete;

  [[nodiscard]] static BrowserPersistence* instance();

  // The security boundary, public so desktop unit tests can check it.
  [[nodiscard]] static QStringList durablePreferenceKeys();
  [[nodiscard]] static bool isDurablePreferenceKey(const QString& key);
  [[nodiscard]] static std::optional<QVariant> sanitizePreference(const QString& key, const QVariant& value);
  [[nodiscard]] static bool isSafeGenericLayoutRecipe(const QByteArray& xml);

  // The opt-in for plugin settings (PluginSettingsKeys.h): a pj.settings.v1 key a plugin writes as
  // "durable/<name>" is stored as "PluginSettings/<plugin>/durable/<name>" and kept across a reload when its value is
  // a string of at most kMaxDurablePluginValueLength characters with no upload identity or control character; at most
  // kMaxDurablePluginKeysPerPlugin such keys per plugin (the first in key order). Every other plugin key lives as
  // long as the page. The plugin keeps credentials out of what it marks durable; the host stores what it is given.
  static constexpr qsizetype kMaxDurablePluginValueLength = 2048;
  static constexpr int kMaxDurablePluginKeysPerPlugin = 8;
  // The value to keep for `key`, else nullopt.
  [[nodiscard]] static std::optional<QString> sanitizeDurablePluginSetting(const QString& key, const QVariant& value);
  // Every plugin setting of `settings` that is kept across a reload, with its value.
  [[nodiscard]] static QHash<QString, QString> durablePluginSettings(QSettings& settings);

  [[nodiscard]] QList<LayoutRecipe> recentLayouts() const;
  [[nodiscard]] std::optional<LayoutRecipe> recentLayout(const QString& id) const;
  bool rememberGenericLayout(const QString& browser_name, const QByteArray& xml, QString* error = nullptr);
  void clearRecentLayouts();

  // Flushes allowed session preferences now; also runs every kPreferenceSyncInterval and at aboutToQuit.
  void syncPreferences();

#ifdef PJ_TARGET_WASM
  // Defined only in probe builds; declared in production WASM too so every TU sees one class shape.
  void seedLegacyUnsafeValuesForTest();
  [[nodiscard]] bool durableContainsForTest(const QString& key) const;
  [[nodiscard]] qsizetype durableKeyCountForTest() const;
#endif

 private:
  enum class PreferenceDecodeStatus {
    kAbsent,
    kValid,
    kRejected,
  };

  struct DecodedPreferences {
    PreferenceDecodeStatus status = PreferenceDecodeStatus::kAbsent;
    QHash<QString, QVariant> values;
  };

  [[nodiscard]] static DecodedPreferences decodePreferences(const QVariant& stored);
  // The durable plugin settings of a stored envelope, re-checked as durablePluginSettings checks them.
  [[nodiscard]] static QHash<QString, QString> decodePluginSettings(const QVariant& stored);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
