// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/QSettingsBackend.h"

#include <QStringList>
#include <QThread>
#include <QVariant>
#include <algorithm>
#include <array>
#include <stdexcept>

#include "pj_runtime/PluginSettingsKeys.h"

namespace PJ {

namespace {

QString toKey(std::string_view key) {
  return QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size()));
}

// Persisted name: set in a plugin's group once its root keys were copied in. A plugin never reads, writes or removes
// it (scopedKey), so it cannot collide with a plugin key; renaming it copies every plugin's root keys again.
constexpr QLatin1StringView kRootKeysCopiedMarker{"__pj_root_keys_copied__"};

// The app's own root groups whose names are lowercase; every other app key starts with a capital letter.
constexpr std::array<QLatin1StringView, 2> kAppLowercaseGroups{
    QLatin1StringView("ui"), QLatin1StringView("pj_scene3d")};

bool isLowercaseName(QStringView name) {
  for (const QChar c : name) {
    if (!(c.isDigit() || c == u'_' || c == u'-' || (c >= u'a' && c <= u'z'))) {
      return false;
    }
  }
  return !name.isEmpty();
}

// pj_runtime ARCHITECTURE "Plugin settings", "Root keys": the root key `key` may belong to the plugin whose id, with
// '_' read as '-', is `id_words` between '-' delimiters.
bool mayOwnRootKey(const QString& key, const QString& id_words) {
  qsizetype end = 0;
  while (end < key.size() && key[end] != u'/' && key[end] != u'.') {
    ++end;
  }
  const QStringView segment = QStringView(key).left(end);
  if (!isLowercaseName(segment) ||
      std::find(kAppLowercaseGroups.begin(), kAppLowercaseGroups.end(), segment) != kAppLowercaseGroups.end()) {
    return false;
  }
  return id_words.contains(u'-' + segment.toString().replace(u'_', u'-') + u'-');
}

}  // namespace

QSettingsBackend::QSettingsBackend() = default;

QSettingsBackend::QSettingsBackend(const QString& ini_path) : settings_(ini_path, QSettings::IniFormat) {}

std::unique_ptr<QSettingsBackend> QSettingsBackend::forPlugin(std::string_view plugin_id, const QString& ini_path) {
  if (plugin_id.empty()) {
    return nullptr;
  }
  auto backend =
      ini_path.isEmpty() ? std::make_unique<QSettingsBackend>() : std::make_unique<QSettingsBackend>(ini_path);
  backend->prefix_ = pluginSettingsGroup(plugin_id) + u'/';
  backend->copyRootKeysOnce(u'-' + toKey(plugin_id).replace(u'_', u'-') + u'-');
  return backend;
}

void QSettingsBackend::copyRootKeysOnce(const QString& id_words) {
  const QString marker = prefix_ + kRootKeysCopiedMarker;
  if (settings_.contains(marker)) {
    return;
  }
  for (const QString& key : settings_.allKeys()) {
    if (mayOwnRootKey(key, id_words) && !settings_.contains(prefix_ + key)) {
      settings_.setValue(prefix_ + key, settings_.value(key));
    }
  }
  settings_.setValue(marker, true);
}

std::optional<QString> QSettingsBackend::scopedKey(std::string_view key) const {
  if (prefix_.isEmpty()) {
    return toKey(key);
  }
  // QSettings drops empty segments itself; normalizing first is what lets the marker check see the stored key.
  const QString normalized = toKey(key).split(u'/', Qt::SkipEmptyParts).join(u'/');
  if (normalized.isEmpty()) {
    throw std::invalid_argument("pj.settings.v1: the key is empty");
  }
  if (normalized == kRootKeysCopiedMarker) {
    return std::nullopt;
  }
  return prefix_ + normalized;
}

void QSettingsBackend::checkThread() const {
  if (QThread::currentThread() != settings_.thread()) {
    throw std::runtime_error("pj.settings.v1 is available only on the thread that created it");
  }
}

void QSettingsBackend::store(std::string_view key, const QVariant& value) {
  checkThread();
  const std::optional<QString> scoped = scopedKey(key);
  if (!scoped.has_value()) {
    throw std::invalid_argument("pj.settings.v1: the key is reserved");
  }
  if (!settings_.isWritable()) {
    throw std::runtime_error("the settings file is not writable");
  }
  settings_.setValue(*scoped, value);
}

std::optional<QVariant> QSettingsBackend::value(std::string_view key) {
  checkThread();
  const std::optional<QString> scoped = scopedKey(key);
  if (!scoped.has_value()) {
    return std::nullopt;
  }
  QVariant stored = settings_.value(*scoped);
  if (!stored.isValid()) {
    return std::nullopt;
  }
  return stored;
}

std::optional<std::string> QSettingsBackend::getString(std::string_view key) {
  const std::optional<QVariant> stored = value(key);
  if (!stored.has_value()) {
    return std::nullopt;
  }
  return stored->toString().toStdString();
}

void QSettingsBackend::setString(std::string_view key, std::string_view value) {
  store(key, toKey(value));
}

std::optional<std::vector<std::string>> QSettingsBackend::getStringList(std::string_view key) {
  const std::optional<QVariant> stored = value(key);
  if (!stored.has_value()) {
    return std::nullopt;
  }
  std::vector<std::string> out;
  const QStringList list = stored->toStringList();
  out.reserve(static_cast<size_t>(list.size()));
  for (const QString& item : list) {
    out.push_back(item.toStdString());
  }
  return out;
}

void QSettingsBackend::setStringList(std::string_view key, const std::vector<std::string>& values) {
  QStringList list;
  list.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string& value : values) {
    list.append(QString::fromStdString(value));
  }
  store(key, list);
}

bool QSettingsBackend::contains(std::string_view key) {
  return value(key).has_value();
}

void QSettingsBackend::remove(std::string_view key) {
  checkThread();
  if (const std::optional<QString> scoped = scopedKey(key)) {
    settings_.remove(*scoped);
  }
}

}  // namespace PJ
