// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/QSettingsBackend.h"

#include <QStringList>
#include <QVariant>

#include "pj_runtime/PluginSettingsKeys.h"

namespace PJ {

namespace {
QString toKey(std::string_view key) {
  return QString::fromUtf8(key.data(), static_cast<qsizetype>(key.size()));
}
}  // namespace

QSettingsBackend::QSettingsBackend() = default;

QSettingsBackend::QSettingsBackend(const QString& ini_path) : settings_(ini_path, QSettings::IniFormat) {}

std::unique_ptr<QSettingsBackend> QSettingsBackend::forPlugin(std::string_view plugin_id, const QString& ini_path) {
  auto backend =
      ini_path.isEmpty() ? std::make_unique<QSettingsBackend>() : std::make_unique<QSettingsBackend>(ini_path);
  backend->settings_.beginGroup(pluginSettingsGroup(plugin_id));
  return backend;
}

std::optional<std::string> QSettingsBackend::getString(std::string_view key) {
  const QString qkey = toKey(key);
  if (!settings_.contains(qkey)) {
    return std::nullopt;
  }
  return settings_.value(qkey).toString().toStdString();
}

void QSettingsBackend::setString(std::string_view key, std::string_view value) {
  settings_.setValue(toKey(key), QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
}

std::optional<std::vector<std::string>> QSettingsBackend::getStringList(std::string_view key) {
  const QString qkey = toKey(key);
  if (!settings_.contains(qkey)) {
    return std::nullopt;
  }
  std::vector<std::string> out;
  const QStringList list = settings_.value(qkey).toStringList();
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
  settings_.setValue(toKey(key), list);
}

bool QSettingsBackend::contains(std::string_view key) {
  return settings_.contains(toKey(key));
}

void QSettingsBackend::remove(std::string_view key) {
  settings_.remove(toKey(key));
}

}  // namespace PJ
