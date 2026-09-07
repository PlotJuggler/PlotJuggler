// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <string>

#include "pj_base/sdk/version.hpp"
#include "pj_marketplace/platform_utils.hpp"
#include "pj_marketplace/registry_manager.hpp"
#include "pj_marketplace/registry_resolver.hpp"

namespace PJ {

RegistryManager::RegistryManager(QObject* parent)
    : QObject(parent),
      network_(new QNetworkAccessManager(this)),
      platform_(PlatformUtils::currentPlatform()),
      sdk_version_(QString::fromStdString(std::string(sdkVersion()))) {}

void RegistryManager::setEligibility(const QString& platform) {
  platform_ = platform;
}

void RegistryManager::fetchRegistry(const QUrl& url) {
  // Cancel any in-flight request before starting a new one.
  if (pending_reply_ && pending_reply_->isRunning()) {
    pending_reply_->abort();
  }

  extensions_.clear();

  emit fetchStarted();

  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  QNetworkReply* reply = network_->get(request);
  pending_reply_ = reply;

  // Capture this call's own reply (not the shared member): a stale/aborted reply
  // must not read or clear whatever pending_reply_ points at by the time its
  // finished() fires.
  connect(reply, &QNetworkReply::finished, this, [this, reply]() {
    reply->deleteLater();

    // If this is no longer the current request, it was aborted to start a fresh
    // fetch (e.g. the user clicked Refresh again while one was in flight). That
    // is a user-initiated cancellation, not a failure — ignore it silently so a
    // re-fetch never surfaces a spurious "Failed to load registry".
    if (reply != pending_reply_ || reply->error() == QNetworkReply::OperationCanceledError) {
      return;
    }
    pending_reply_ = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
      emit fetchError(reply->errorString());
      emit fetchFinished(false);
      return;
    }

    const QByteArray data = reply->readAll();
    const bool ok = parseJson(data);
    emit fetchFinished(ok);
  });
}

QList<Extension> RegistryManager::extensions() const {
  return extensions_;
}

QList<Extension> RegistryManager::compatibleExtensions(const QString& platform) const {
  QList<Extension> result;
  result.reserve(extensions_.size());
  for (const Extension& ext : extensions_) {
    if (ext.platforms.contains(platform)) {
      result.append(ext);
    }
  }
  return result;
}

Extension RegistryManager::findById(const QString& id) const {
  for (const Extension& ext : extensions_) {
    if (ext.id == id) {
      return ext;
    }
  }
  return {};  // Default-constructed: id is empty, callers must check id.isEmpty()
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool RegistryManager::parseJson(const QByteArray& data) {
  // Reads a required string field; emits fetchError() and returns nullopt if missing.
  auto required_string = [this](const QJsonObject& obj, const QString& key) -> std::optional<QString> {
    if (!obj.contains(key) || !obj[key].isString() || obj[key].toString().isEmpty()) {
      emit fetchError(QString("Registry parse error: missing or empty required string field \"%1\"").arg(key));
      return std::nullopt;
    }
    return obj[key].toString();
  };
  auto optional_string = [this](const QJsonObject& obj, const QString& key) -> std::optional<QString> {
    if (!obj.contains(key)) {
      return QString{};
    }
    if (!obj[key].isString()) {
      emit fetchError(QString("Registry parse error: field \"%1\" must be a string").arg(key));
      return std::nullopt;
    }
    return obj[key].toString();
  };

  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);

  if (doc.isNull()) {
    emit fetchError(QString("JSON parse error: %1").arg(parse_error.errorString()));
    return false;
  }

  if (!doc.isObject()) {
    emit fetchError("Registry JSON root must be an object");
    return false;
  }

  const QJsonObject root = doc.object();

  if (!root.contains("extensions") || !root["extensions"].isArray()) {
    emit fetchError("Registry JSON missing \"extensions\" array");
    return false;
  }

  QList<Extension> parsed;

  for (const QJsonValue& value : root["extensions"].toArray()) {
    if (!value.isObject()) {
      emit fetchError("Each entry in \"extensions\" must be a JSON object");
      return false;
    }

    const QJsonObject obj = value.toObject();
    Extension ext;

    // Required fields — abort the entire fetch if any are missing.
    auto id = required_string(obj, "id");
    auto name = required_string(obj, "name");
    auto version = required_string(obj, "version");

    if (!id || !name || !version) {
      return false;
    }

    ext.id = *id;
    ext.name = *name;
    ext.version = *version;

    // Optional fields — use empty string as sentinel when absent.
    ext.description = obj["description"].toString();
    ext.author = obj["author"].toString();
    ext.publisher = obj["publisher"].toString();
    ext.website = obj["website"].toString();
    ext.repository = obj["repository"].toString();
    ext.license = obj["license"].toString();
    ext.icon_url = obj["icon_url"].toString();
    ext.category = obj["category"].toString();
    const auto minimum_sdk = optional_string(obj, "min_sdk_required");
    const auto minimum_application = optional_string(obj, "min_plotjuggler_version");
    const auto suggested_sdk = optional_string(obj, "suggested_sdk_version");
    if (!minimum_sdk || !minimum_application || !suggested_sdk) {
      return false;
    }
    ext.min_sdk_required = *minimum_sdk;
    ext.min_plotjuggler_version = *minimum_application;
    ext.suggested_sdk_version = *suggested_sdk;

    for (const QJsonValue& tag : obj["tags"].toArray()) {
      ext.tags.append(tag.toString());
    }

    // Platforms: { "linux-x86_64": { "url": "...", "checksum": "sha256:..." } }
    const QJsonObject platforms = obj["platforms"].toObject();
    for (auto it = platforms.begin(); it != platforms.end(); ++it) {
      if (!it.value().isObject()) {
        continue;
      }
      const QJsonObject artifact_obj = it.value().toObject();
      Platform artifact;
      artifact.url = artifact_obj["url"].toString();
      artifact.checksum = artifact_obj["checksum"].toString();
      ext.platforms.insert(it.key(), artifact);
    }

    // Changelog: { "1.0.0": "Initial release", "1.1.0": "Bug fixes" }
    const QJsonObject changelog = obj["changelog"].toObject();
    for (auto it = changelog.begin(); it != changelog.end(); ++it) {
      ext.changelog.insert(it.key(), it.value().toString());
    }

    parsed.append(std::move(ext));
  }

  auto resolved = resolveRegistryCandidates(parsed, {platform_, sdk_version_});
  if (!resolved) {
    emit fetchError(QString("Registry resolution error: %1").arg(resolved.error()));
    return false;
  }
  extensions_ = std::move(*resolved);
  return true;
}

}  // namespace PJ
