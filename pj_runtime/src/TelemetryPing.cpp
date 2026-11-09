// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/TelemetryPing.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <chrono>
#include <utility>

#include "TelemetryIdentity.h"
#include "pj_runtime/HttpGet.h"
using namespace Qt::StringLiterals;

namespace PJ {

namespace {
constexpr auto kDefaultEndpointUrl = "https://app.plotjuggler.io/telemetry";
constexpr int kTransferTimeoutMs = 15000;

// Stable anonymous per-machine id. Hashing follows Qt's own machineUniqueId()
// privacy guidance — the value cannot be reversed to the machine id nor
// correlated with other apps that send the raw id. The salt is the one the
// PJ3-era dashboard has always counted with, so it must not change.
QString anonymousUserId() {
  return telemetry::saltedMachineHash("plotjuggler4-telemetry-v1");
}

// The full ping payload — the privacy contract: environment facts only, never
// behavior, never user content (paths, file names, topic names). Adding a
// field requires updating docs/TELEMETRY.md and telemetry_ping_test's
// allowed-key set. The four PJ3-era keys (user_id/os/version/installation)
// keep their exact names so the existing endpoint needs no changes.
QJsonObject buildTelemetryPayload(const QString& installation) {
  QJsonObject payload;
  payload[u"user_id"_s] = anonymousUserId();
  payload[u"os"_s] = QSysInfo::productType();
  payload[u"os_version"_s] = QSysInfo::productVersion();
  payload[u"version"_s] = QCoreApplication::applicationVersion();
  payload[u"installation"_s] = installation;
  payload[u"arch"_s] = QSysInfo::currentCpuArchitecture();
  // Linux only: xcb-forced builds can't infer the session from platformName(),
  // and "how many users run Wayland?" drives real bundling/bug decisions.
  const QString session_type = qEnvironmentVariable("XDG_SESSION_TYPE");
  if (!session_type.isEmpty()) {
    payload[u"display_server"_s] = session_type;
  }
  return payload;
}
}  // namespace

TelemetryPing::TelemetryPing(QObject* parent)
    : QObject(parent),
      network_(new QNetworkAccessManager(this)),
      endpoint_url_(QString::fromLatin1(kDefaultEndpointUrl)) {}

TelemetryPing::~TelemetryPing() = default;

void TelemetryPing::setEndpointUrl(const QUrl& url) {
  endpoint_url_ = url;
}

QUrl TelemetryPing::endpointUrl() const {
  return endpoint_url_;
}

void TelemetryPing::send(const QString& installation) {
  // An unstamped build never reports; every other channel does, including a
  // downstream packager's own. The outcome is the ordinary silent failure, so
  // callers need no special case.
  if (!telemetry::isReportingInstallation(installation)) {
    QMetaObject::invokeMethod(this, [this]() { emit finished(false); }, Qt::QueuedConnection);
    return;
  }

  QNetworkRequest request(endpoint_url_);
  request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
  request.setHeader(QNetworkRequest::UserAgentHeader, u"PlotJuggler"_s);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  const QByteArray body = QJsonDocument(buildTelemetryPayload(installation)).toJson(QJsonDocument::Compact);
  httpPostWithTimeout(
      *network_, std::move(request), body, std::chrono::milliseconds(kTransferTimeoutMs), this,
      [this](QNetworkReply& reply) { emit finished(reply.error() == QNetworkReply::NoError); });
}

}  // namespace PJ
