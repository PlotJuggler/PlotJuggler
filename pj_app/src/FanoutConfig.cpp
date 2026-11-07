// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "FanoutConfig.h"

#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <functional>
#include <optional>
#include <utility>
using namespace Qt::StringLiterals;

namespace PJ::detail {

namespace {
Q_LOGGING_CATEGORY(lcFanout, "pj.app.fileloader")

std::optional<QJsonObject> parseObject(std::string_view config) {
  if (config.empty()) {
    return std::nullopt;
  }
  const QByteArray bytes(config.data(), static_cast<qsizetype>(config.size()));
  const QJsonDocument doc = QJsonDocument::fromJson(bytes);
  return doc.isObject() ? std::optional<QJsonObject>{doc.object()} : std::nullopt;
}

bool isRebasableFilepath(const QJsonValue& previous, const QString& fresh) {
  if (!previous.isString()) {
    return false;
  }
  const QString old_path = previous.toString();
  return !old_path.isEmpty() && !old_path.contains(u"://"_s) && QDir::isAbsolutePath(old_path) &&
         QDir::cleanPath(old_path) != QDir::cleanPath(fresh);
}

std::string compactJson(const QJsonObject& object) {
  return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}

// Applies `fn` to each `__pj_fanout` child that is a string parsing as a JSON
// object. Other entries (malformed children, non-strings) pass through
// untouched: extractFanout diagnoses them later, and hiding a plugin's
// malformed child behind a different valid object would mask the fault.
QJsonObject mapFanoutChildren(QJsonObject object, const std::function<QJsonObject(QJsonObject)>& fn) {
  const QJsonValue fanout_value = object.value(u"__pj_fanout"_s);
  if (!fanout_value.isArray()) {
    return object;
  }
  const QJsonArray entries = fanout_value.toArray();
  QJsonArray mapped;
  for (const QJsonValue& entry : entries) {
    std::optional<QJsonObject> nested_object;
    if (entry.isString()) {
      nested_object = parseObject(entry.toString().toStdString());
    }
    if (!nested_object.has_value()) {
      mapped.push_back(entry);
      continue;
    }
    mapped.push_back(QString::fromUtf8(QJsonDocument(fn(std::move(*nested_object))).toJson(QJsonDocument::Compact)));
  }
  object.insert(u"__pj_fanout"_s, mapped);
  return object;
}

QJsonObject rewriteObjectFilepaths(QJsonObject object, const QString& fresh_path, SavedFilepath saved) {
  const QJsonValue previous = object.value(u"filepath"_s);
  object.remove(kSavedFilepathKey);
  if (saved == SavedFilepath::kRecord && isRebasableFilepath(previous, fresh_path)) {
    object.insert(kSavedFilepathKey, previous);
  }
  object.insert(u"filepath"_s, fresh_path);
  return mapFanoutChildren(std::move(object), [&](QJsonObject child) {
    return rewriteObjectFilepaths(std::move(child), fresh_path, saved);
  });
}

QJsonObject stripSavedFilepath(QJsonObject object) {
  object.remove(kSavedFilepathKey);
  return mapFanoutChildren(std::move(object), [](QJsonObject child) { return stripSavedFilepath(std::move(child)); });
}
}  // namespace

std::vector<std::string> extractFanout(std::string_view config) {
  const auto single_instance_fallback = [&]() { return std::vector<std::string>{std::string(config)}; };
  if (config.empty()) {
    return single_instance_fallback();
  }
  const QByteArray bytes(config.data(), static_cast<qsizetype>(config.size()));
  const auto doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject()) {
    return single_instance_fallback();
  }
  const QJsonValue v = doc.object().value(u"__pj_fanout"_s);
  if (!v.isArray()) {
    return single_instance_fallback();
  }
  const QJsonArray arr = v.toArray();
  if (arr.isEmpty()) {
    return single_instance_fallback();
  }
  std::vector<std::string> out;
  out.reserve(static_cast<std::size_t>(arr.size()));
  for (qsizetype idx = 0; idx < arr.size(); ++idx) {
    const QJsonValue entry = arr.at(idx);
    if (!entry.isString()) {
      // A non-string entry means the plugin emitted a malformed fanout array.
      // Skipping it would silently drop an instance the user asked for, so log
      // it — the caller surfaces the aggregate result to the user.
      qCWarning(lcFanout) << "[FileLoader] extractFanout: dropping non-string __pj_fanout entry at index" << idx;
      continue;
    }
    out.emplace_back(entry.toString().toStdString());
  }
  if (out.empty()) {
    qCWarning(lcFanout) << "[FileLoader] extractFanout: __pj_fanout array had no usable string entries; "
                           "falling back to single-instance import";
    return single_instance_fallback();
  }
  return out;
}

QString parseDisplaySuffix(std::string_view cfg, const QString& fallback) {
  if (cfg.empty()) {
    return fallback;
  }
  const QByteArray bytes(cfg.data(), static_cast<qsizetype>(cfg.size()));
  const auto doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject()) {
    return fallback;
  }
  const QJsonValue v = doc.object().value(u"display_suffix"_s);
  if (!v.isString()) {
    return fallback;
  }
  const QString s = v.toString();
  return s.isEmpty() ? fallback : s;
}

QString parseDisplayName(std::string_view cfg) {
  if (cfg.empty()) {
    return QString{};
  }
  const QByteArray bytes(cfg.data(), static_cast<qsizetype>(cfg.size()));
  const auto doc = QJsonDocument::fromJson(bytes);
  if (!doc.isObject()) {
    return QString{};
  }
  const QJsonValue v = doc.object().value(u"display_name"_s);
  return v.isString() ? v.toString() : QString{};
}

std::optional<std::string> rewriteReplayFilepaths(
    std::string_view config, const QString& fresh_path, SavedFilepath saved) {
  QJsonObject object;
  if (!config.empty()) {
    std::optional<QJsonObject> parsed = parseObject(config);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    object = std::move(*parsed);
  }
  return compactJson(rewriteObjectFilepaths(std::move(object), fresh_path, saved));
}

std::string stripHostReplayKeys(std::string_view config) {
  if (config.find(std::string_view(kSavedFilepathKey.data(), kSavedFilepathKey.size())) == std::string_view::npos) {
    return std::string(config);
  }
  const std::optional<QJsonObject> object = parseObject(config);
  if (!object.has_value()) {
    return std::string(config);
  }
  return compactJson(stripSavedFilepath(*object));
}

}  // namespace PJ::detail
