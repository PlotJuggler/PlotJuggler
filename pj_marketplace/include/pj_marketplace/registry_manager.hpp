#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

#include "pj_marketplace/extension.hpp"

namespace PJ {

// Downloads and parses the marketplace registry JSON from a remote URL.
//
// Usage:
//   auto* mgr = new RegistryManager(this);
//   connect(mgr, &RegistryManager::fetchFinished, this, [mgr](bool ok) {
//     if (ok) use(mgr->extensions());
//   });
//   mgr->fetchRegistry(QUrl("https://raw.githubusercontent.com/.../registry.json"));
//
// A new fetchRegistry() call cancels any in-flight request.
class RegistryManager : public QObject {
  Q_OBJECT

 public:
  explicit RegistryManager(QObject* parent = nullptr);

  // Starts an async download of the registry JSON at `url`.
  // Emits fetchStarted() immediately, then fetchFinished() or fetchError() when done.
  void fetchRegistry(const QUrl& url);

  // Returns the parsed extensions after a successful fetch; empty list otherwise.
  QList<Extension> extensions() const;

  // Same as extensions(), but drops entries whose `platforms` map has no
  // artifact for `platform` (e.g. "linux-x86_64"). Use this for catalog
  // views — installation would fail anyway for the dropped entries (see
  // ExtensionManager::doInstall). The platform key is injected by the
  // caller so RegistryManager stays independent of host detection.
  QList<Extension> compatibleExtensions(const QString& platform) const;

  // Returns the first extension whose id matches, or a default-constructed Extension
  // (id is empty) when not found.
  Extension findById(const QString& id) const;

  // Overrides the platform key and application version used by the headless
  // candidate resolver. The SDK version remains the running SDK's own value.
  // Production wires nothing; this is the test seam. Takes effect on the next parse.
  void setEligibility(const QString& platform);

 signals:
  void fetchStarted();
  void fetchFinished(bool success);
  void fetchError(const QString& error_message);

 private:
  // Parses raw JSON bytes into extensions_.
  // Emits fetchError() and returns false on any parse failure.
  bool parseJson(const QByteArray& data);

  QNetworkAccessManager* network_;
  QNetworkReply* pending_reply_ = nullptr;  // Non-owning; owned by network_
  QList<Extension> extensions_;
  QString platform_;     // Artifact key this build can install; "" ranks every entry alike
  QString sdk_version_;  // SDK contract version used by the headless resolver
};

}  // namespace PJ
