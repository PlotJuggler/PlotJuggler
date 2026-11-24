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
// page-lifetime MEMFS; this bridge alone owns WebLocalStorageFormat and mirrors three bounded envelopes into it: a
// schema-validated preference allowlist, source-free layout recipes and the plugin settings a plugin marked durable.
// What never enters it: pj_app ARCHITECTURE "Persistence". Native builds never construct it.
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

  // The plugin-settings opt-in (pj_runtime ARCHITECTURE "Plugin settings"): the key "durable/<name>" of plugin <id>
  // is "PluginSettings/<id>/durable/<name>" in the settings, kept across a reload when sanitizeDurablePluginSetting
  // accepts it and within these caps, taken in key order. The envelope cap holds on write and on read alike, so a
  // write never produces an envelope the next load rejects.
  static constexpr qsizetype kMaxDurablePluginValueLength = 2048;
  static constexpr int kMaxDurablePluginKeysPerPlugin = 8;
  static constexpr int kMaxDurablePluginIds = 32;
  static constexpr qsizetype kMaxPluginSettingsBytes = 256 * 1024;

  struct DurablePluginSetting {
    QString plugin_id;
    QString value;
  };
  // Kept when `key` has the durable layout, a name after "durable/" that reads like no credential (isSecretKey) and
  // names no upload, and `value` is a string within kMaxDurablePluginValueLength that names no upload and carries no
  // URL password anywhere; neither holds a control character or a lone surrogate. The plugin keeps credentials out
  // of what it marks durable; this is a backstop.
  [[nodiscard]] static std::optional<DurablePluginSetting> sanitizeDurablePluginSetting(
      const QString& key, const QVariant& value);
  // The plugin settings of `settings` kept across a reload; reads only the durable groups.
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

  // How a stored envelope read: absent, valid, or rejected (malformed, oversized or another version).
  enum class EnvelopeStatus {
    kAbsent,
    kValid,
    kRejected,
  };

 private:
  template <class Values>
  struct Decoded {
    EnvelopeStatus status = EnvelopeStatus::kAbsent;
    Values values;
  };

  [[nodiscard]] static Decoded<QHash<QString, QVariant>> decodePreferences(const QVariant& stored);
  // Re-checks every entry as durablePluginSettings does.
  [[nodiscard]] static Decoded<QHash<QString, QString>> decodePluginSettings(const QVariant& stored);
  [[nodiscard]] static QByteArray encodePluginSettings(const QHash<QString, QString>& settings);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace PJ
