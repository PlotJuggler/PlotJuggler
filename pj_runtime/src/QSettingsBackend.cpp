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

// The app's own root groups whose names are lowercase; every other app key starts with a capital letter.
constexpr std::array<QLatin1StringView, 2> kAppLowercaseGroups{
    QLatin1StringView("ui"), QLatin1StringView("pj_scene3d")};

bool isLowercaseName(QStringView name) {
  for (const QChar c : name) {
    if (!(c.isDigit() || c == u'_' || (c >= u'a' && c <= u'z'))) {
      return false;
    }
  }
  return !name.isEmpty();
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
  backend->settings_.beginGroup(pluginSettingsGroup(plugin_id));
  backend->plugin_id_ = toKey(plugin_id);
  return backend;
}

bool QSettingsBackend::isLegacyKey(const QString& key) const {
  if (plugin_id_.isEmpty()) {
    return false;
  }
  qsizetype end = 0;
  while (end < key.size() && key[end] != u'/' && key[end] != u'.') {
    ++end;
  }
  const QStringView segment = QStringView(key).left(end);
  if (segment == QLatin1StringView("source_presentation")) {
    return true;
  }
  if (!isLowercaseName(segment) ||
      std::find(kAppLowercaseGroups.begin(), kAppLowercaseGroups.end(), segment) != kAppLowercaseGroups.end()) {
    return false;
  }
  const QString words = u'-' + plugin_id_ + u'-';
  return words.contains(u'-' + segment.toString().replace(u'_', u'-') + u'-');
}

void QSettingsBackend::checkThread() const {
  if (QThread::currentThread() != settings_.thread()) {
    throw std::runtime_error("pj.settings.v1 is available on the main thread only");
  }
}

void QSettingsBackend::store(const QString& key, const QVariant& value) {
  settings_.setValue(key, value);
  settings_.sync();
  if (settings_.status() != QSettings::NoError) {
    throw std::runtime_error("the setting could not be stored");
  }
}

std::optional<QVariant> QSettingsBackend::value(const QString& key) {
  checkThread();
  if (settings_.contains(key)) {
    return settings_.value(key);
  }
  if (!isLegacyKey(key)) {
    return std::nullopt;
  }
  const QString group = settings_.group();
  settings_.endGroup();
  std::optional<QVariant> legacy;
  if (settings_.contains(key)) {
    legacy = settings_.value(key);
  }
  settings_.beginGroup(group);
  if (legacy.has_value()) {
    settings_.setValue(key, *legacy);
  }
  return legacy;
}

std::optional<std::string> QSettingsBackend::getString(std::string_view key) {
  const std::optional<QVariant> stored = value(toKey(key));
  if (!stored.has_value()) {
    return std::nullopt;
  }
  return stored->toString().toStdString();
}

void QSettingsBackend::setString(std::string_view key, std::string_view value) {
  checkThread();
  store(toKey(key), QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
}

std::optional<std::vector<std::string>> QSettingsBackend::getStringList(std::string_view key) {
  const std::optional<QVariant> stored = value(toKey(key));
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
  checkThread();
  QStringList list;
  list.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string& value : values) {
    list.append(QString::fromStdString(value));
  }
  store(toKey(key), list);
}

bool QSettingsBackend::contains(std::string_view key) {
  return value(toKey(key)).has_value();
}

void QSettingsBackend::remove(std::string_view key) {
  checkThread();
  const QString qkey = toKey(key);
  settings_.remove(qkey);
  // Else the next read would bring the legacy copy back.
  if (isLegacyKey(qkey)) {
    const QString group = settings_.group();
    settings_.endGroup();
    settings_.remove(qkey);
    settings_.beginGroup(group);
  }
}

}  // namespace PJ
