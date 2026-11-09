// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/UsageCounters.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <utility>

using namespace Qt::StringLiterals;

namespace PJ {

namespace {
// Bumped when the on-disk shape changes; a file of another version is discarded (it holds at most a
// week of counts, so migrating it is not worth the code).
constexpr int kStorageVersion = 1;
// A runaway caller cannot overflow a counter or inflate a report without bound.
constexpr int64_t kMaxCount = 1'000'000;

QDate utcToday() {
  return QDateTime::currentDateTimeUtc().date();
}

void bump(int64_t& value) {
  value = std::min(value + 1, kMaxCount);
}

QJsonObject dayToJson(const DayUsage& day) {
  QJsonObject features;
  for (const auto& [key, uses] : day.features) {
    features[QString::fromStdString(key)] = static_cast<double>(uses);
  }
  QJsonArray plugins;
  for (const auto& [row, uses] : day.plugin_uses) {
    const auto& [kind, action, id] = row;
    plugins.append(
        QJsonObject{
            {u"kind"_s, QString::fromStdString(kind)},
            {u"action"_s, QString::fromStdString(action)},
            {u"id"_s, QString::fromStdString(id)},
            {u"n"_s, static_cast<double>(uses)}});
  }
  QJsonArray installed;
  for (const auto& plugin : day.installed) {
    installed.append(
        QJsonObject{
            {u"id"_s, QString::fromStdString(plugin.id)},
            {u"v"_s, QString::fromStdString(plugin.version)},
            {u"kind"_s, QString::fromStdString(plugin.kind)}});
  }
  return QJsonObject{
      {u"report_id"_s, day.report_id},
      {u"app_version"_s, day.app_version},
      {u"features"_s, features},
      {u"plugins"_s, plugins},
      {u"installed"_s, installed}};
}

int64_t clampedCount(const QJsonValue& value) {
  return std::clamp<int64_t>(value.toInteger(), 0, kMaxCount);
}

DayUsage dayFromJson(const QDate& date, const QJsonObject& object) {
  DayUsage day;
  day.day = date;
  day.report_id = object.value(u"report_id"_s).toString();
  day.app_version = object.value(u"app_version"_s).toString();
  const QJsonObject features = object.value(u"features"_s).toObject();
  for (auto it = features.begin(); it != features.end(); ++it) {
    day.features[it.key().toStdString()] = clampedCount(it.value());
  }
  for (const QJsonValue& value : object.value(u"plugins"_s).toArray()) {
    const QJsonObject row = value.toObject();
    const std::string id = row.value(u"id"_s).toString().toStdString();
    if (id.empty() || day.plugin_uses.size() >= UsageCounters::kMaxPluginRowsPerDay) {
      continue;
    }
    day.plugin_uses[{
        row.value(u"kind"_s).toString().toStdString(), row.value(u"action"_s).toString().toStdString(), id}] =
        clampedCount(row.value(u"n"_s));
  }
  for (const QJsonValue& value : object.value(u"installed"_s).toArray()) {
    const QJsonObject row = value.toObject();
    day.installed.push_back(
        {row.value(u"id"_s).toString().toStdString(), row.value(u"v"_s).toString().toStdString(),
         row.value(u"kind"_s).toString().toStdString()});
  }
  return day;
}
}  // namespace

std::string_view usageFeatureKey(UsageFeature feature) {
  switch (feature) {
    case UsageFeature::kLayoutLoad:
      return "layout.load";
    case UsageFeature::kLayoutSave:
      return "layout.save";
    case UsageFeature::kRecordingStart:
      return "recording.start";
    case UsageFeature::kPanelScene3D:
      return "panel.scene3d";
    case UsageFeature::kPanelScene2D:
      return "panel.scene2d";
  }
  return "unknown";
}

std::string_view usagePluginKind(PluginUse use) {
  switch (use) {
    case PluginUse::kFileLoad:
    case PluginUse::kStreamStart:
      return "data_source";
    case PluginUse::kParserBind:
      return "parser";
    case PluginUse::kToolboxOpen:
      return "toolbox";
    case PluginUse::kWidgetCreate:
      return "widget";
  }
  return "unknown";
}

std::string_view usagePluginAction(PluginUse use) {
  switch (use) {
    case PluginUse::kFileLoad:
      return "file";
    case PluginUse::kStreamStart:
      return "stream";
    case PluginUse::kParserBind:
      return "decode";
    case PluginUse::kToolboxOpen:
      return "open";
    case PluginUse::kWidgetCreate:
      return "create";
  }
  return "unknown";
}

UsageCounters::UsageCounters(QString storage_file, QString app_version, std::function<QDate()> today)
    : storage_file_(std::move(storage_file)),
      app_version_(std::move(app_version)),
      today_(today ? std::move(today) : std::function<QDate()>(utcToday)) {}

void UsageCounters::configure(QString storage_file, QString app_version) {
  const std::scoped_lock lock(mutex_);
  storage_file_ = std::move(storage_file);
  app_version_ = std::move(app_version);
}

void UsageCounters::setEnabled(bool enabled) {
  const std::scoped_lock lock(mutex_);
  if (enabled == enabled_) {
    return;
  }
  enabled_ = enabled;
  days_.clear();
  dirty_ = false;
  if (enabled_) {
    loadLocked();
  } else if (!storage_file_.isEmpty()) {
    QFile::remove(storage_file_);
  }
}

bool UsageCounters::enabled() const {
  const std::scoped_lock lock(mutex_);
  return enabled_;
}

DayUsage& UsageCounters::todayLocked() {
  const QDate today = today_();
  auto it = days_.find(today);
  if (it == days_.end()) {
    DayUsage day;
    day.day = today;
    day.report_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    day.app_version = app_version_;
    it = days_.emplace(today, std::move(day)).first;
    pruneLocked();
  }
  return it->second;
}

void UsageCounters::count(UsageFeature feature) {
  const std::scoped_lock lock(mutex_);
  if (!enabled_) {
    return;
  }
  bump(todayLocked().features[std::string(usageFeatureKey(feature))]);
  dirty_ = true;
}

void UsageCounters::countPlugin(PluginUse use, std::string_view plugin_id) {
  if (plugin_id.empty()) {
    return;
  }
  const std::scoped_lock lock(mutex_);
  if (!enabled_) {
    return;
  }
  auto& uses = todayLocked().plugin_uses;
  std::tuple<std::string, std::string, std::string> row{
      std::string(usagePluginKind(use)), std::string(usagePluginAction(use)), std::string(plugin_id)};
  auto it = uses.find(row);
  if (it == uses.end()) {
    if (uses.size() >= kMaxPluginRowsPerDay) {
      return;
    }
    it = uses.emplace(std::move(row), 0).first;
  }
  bump(it->second);
  dirty_ = true;
}

void UsageCounters::setInstalledPlugins(std::vector<InstalledPluginRecord> plugins) {
  const std::scoped_lock lock(mutex_);
  if (!enabled_) {
    return;
  }
  if (plugins.size() > kMaxPluginRowsPerDay) {
    plugins.resize(kMaxPluginRowsPerDay);
  }
  todayLocked().installed = std::move(plugins);
  dirty_ = true;
}

void UsageCounters::pruneLocked() {
  // Keep today plus the newest kMaxStoredDays completed days. A day "after today" (a clock moved
  // backwards) is kept as completed data rather than guessed about.
  const QDate today = today_();
  std::vector<QDate> completed;
  for (const auto& [date, day] : days_) {
    if (date != today) {
      completed.push_back(date);  // ascending: std::map order
    }
  }
  const auto excess = static_cast<std::ptrdiff_t>(completed.size()) - kMaxStoredDays;
  for (std::ptrdiff_t i = 0; i < excess; ++i) {
    days_.erase(completed[static_cast<size_t>(i)]);
    dirty_ = true;
  }
}

void UsageCounters::flush() {
  const std::scoped_lock lock(mutex_);
  if (enabled_ && dirty_) {
    saveLocked();
  }
}

std::vector<DayUsage> UsageCounters::completedDays() const {
  const std::scoped_lock lock(mutex_);
  std::vector<DayUsage> result;
  if (!enabled_) {
    return result;
  }
  const QDate today = today_();
  for (const auto& [date, day] : days_) {
    if (date != today) {
      result.push_back(day);
    }
  }
  return result;
}

DayUsage UsageCounters::today() const {
  const std::scoped_lock lock(mutex_);
  const auto it = days_.find(today_());
  if (!enabled_ || it == days_.end()) {
    DayUsage empty;
    empty.day = today_();
    empty.app_version = app_version_;
    return empty;
  }
  return it->second;
}

void UsageCounters::acknowledge(const std::vector<QString>& report_ids) {
  const std::scoped_lock lock(mutex_);
  if (!enabled_) {
    return;
  }
  const QDate today = today_();
  for (auto it = days_.begin(); it != days_.end();) {
    const bool sent =
        it->first != today && std::find(report_ids.begin(), report_ids.end(), it->second.report_id) != report_ids.end();
    it = sent ? days_.erase(it) : std::next(it);
  }
  saveLocked();
}

void UsageCounters::loadLocked() {
  if (storage_file_.isEmpty()) {
    return;
  }
  QFile file(storage_file_);
  if (!file.open(QIODevice::ReadOnly)) {
    return;
  }
  const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
  if (root.value(u"v"_s).toInt() != kStorageVersion) {
    return;
  }
  const QJsonObject days = root.value(u"days"_s).toObject();
  for (auto it = days.begin(); it != days.end(); ++it) {
    const QDate date = QDate::fromString(it.key(), Qt::ISODate);
    const DayUsage day = dayFromJson(date, it.value().toObject());
    if (date.isValid() && !day.report_id.isEmpty()) {
      days_[date] = day;
    }
  }
  pruneLocked();
}

void UsageCounters::saveLocked() {
  dirty_ = false;
  if (storage_file_.isEmpty()) {
    return;
  }
  QJsonObject days;
  for (const auto& [date, day] : days_) {
    days[date.toString(Qt::ISODate)] = dayToJson(day);
  }
  QDir().mkpath(QFileInfo(storage_file_).absolutePath());
  QSaveFile file(storage_file_);
  if (!file.open(QIODevice::WriteOnly)) {
    return;
  }
  file.write(QJsonDocument(QJsonObject{{u"v"_s, kStorageVersion}, {u"days"_s, days}}).toJson(QJsonDocument::Compact));
  file.commit();
}

UsageCounters& usageCounters() {
  static UsageCounters counters({}, {});
  return counters;
}

}  // namespace PJ
