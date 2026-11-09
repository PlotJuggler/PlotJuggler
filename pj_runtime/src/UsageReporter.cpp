// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include "pj_runtime/UsageReporter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <chrono>
#include <map>
#include <tuple>
#include <utility>

#include "TelemetryIdentity.h"
#include "pj_runtime/HttpGet.h"

using namespace Qt::StringLiterals;

namespace PJ {

namespace {
constexpr auto kDefaultEndpointUrl = "https://app.plotjuggler.io/telemetry/v2/usage";
constexpr int kTransferTimeoutMs = 15000;
constexpr int kSchemaVersion = 2;
// What a plugin outside the public set is reported as.
constexpr auto kPrivatePluginId = "_private";

// Month-scoped machine hash: counts distinct machines per month, changes every month, and uses a salt
// of its own so it cannot be matched against the launch ping's user_id.
QString periodId(const QDate& day) {
  return telemetry::saltedMachineHash("plotjuggler4-usage-v1|" + day.toString(u"yyyy-MM"_s).toLatin1());
}

QJsonObject buildReport(const DayUsage& day, const QString& installation, const std::set<std::string>& public_ids) {
  const auto named = [&public_ids](const std::string& id) {
    return public_ids.count(id) != 0 ? id : std::string(kPrivatePluginId);
  };

  QJsonObject features;
  for (const auto& [key, uses] : day.features) {
    features[QString::fromStdString(key)] = static_cast<double>(uses);
  }

  // Folding private ids can merge rows, so re-aggregate.
  std::map<std::tuple<std::string, std::string, std::string>, int64_t> uses_by_row;
  for (const auto& [row, uses] : day.plugin_uses) {
    const auto& [kind, action, id] = row;
    uses_by_row[{kind, action, named(id)}] += uses;
  }
  QJsonArray plugins;
  for (const auto& [row, uses] : uses_by_row) {
    const auto& [kind, action, id] = row;
    plugins.append(
        QJsonObject{
            {u"kind"_s, QString::fromStdString(kind)},
            {u"action"_s, QString::fromStdString(action)},
            {u"id"_s, QString::fromStdString(id)},
            {u"n"_s, static_cast<double>(uses)}});
  }

  // A private plugin's version would identify it as surely as its id, so it is dropped with the id.
  std::map<std::tuple<std::string, std::string, std::string>, int64_t> installed_rows;
  for (const auto& plugin : day.installed) {
    const std::string id = named(plugin.id);
    installed_rows[{plugin.kind, id, id == kPrivatePluginId ? std::string() : plugin.version}] += 1;
  }
  QJsonArray installed;
  for (const auto& [row, n] : installed_rows) {
    const auto& [kind, id, version] = row;
    QJsonObject entry{
        {u"kind"_s, QString::fromStdString(kind)},
        {u"id"_s, QString::fromStdString(id)},
        {u"n"_s, static_cast<double>(n)}};
    if (!version.empty()) {
      entry[u"v"_s] = QString::fromStdString(version);
    }
    installed.append(entry);
  }

  return QJsonObject{
      {u"report_id"_s, day.report_id},
      {u"day"_s, day.day.toString(Qt::ISODate)},
      {u"period_id"_s, periodId(day.day)},
      {u"version"_s, day.app_version},
      {u"os"_s, QSysInfo::productType()},
      {u"arch"_s, QSysInfo::currentCpuArchitecture()},
      {u"installation"_s, installation.trimmed()},
      {u"features"_s, features},
      {u"plugins"_s, plugins},
      {u"installed"_s, installed},
  };
}
}  // namespace

UsageReporter::UsageReporter(UsageCounters& counters, QObject* parent)
    : QObject(parent),
      counters_(counters),
      network_(new QNetworkAccessManager(this)),
      endpoint_url_(QString::fromLatin1(kDefaultEndpointUrl)) {}

UsageReporter::~UsageReporter() = default;

void UsageReporter::setEndpointUrl(const QUrl& url) {
  endpoint_url_ = url;
}

QUrl UsageReporter::endpointUrl() const {
  return endpoint_url_;
}

QJsonObject UsageReporter::buildBody(
    const std::vector<DayUsage>& days, const QString& installation, const std::set<std::string>& public_plugin_ids) {
  QJsonArray reports;
  for (const DayUsage& day : days) {
    reports.append(buildReport(day, installation, public_plugin_ids));
  }
  return QJsonObject{{u"schema"_s, kSchemaVersion}, {u"reports"_s, reports}};
}

void UsageReporter::send(const QString& installation, const std::set<std::string>& public_plugin_ids) {
  const auto finish_later = [this](bool ok) {
    QMetaObject::invokeMethod(this, [this, ok]() { emit finished(ok); }, Qt::QueuedConnection);
  };
  if (in_flight_ || !telemetry::isReportingInstallation(installation)) {
    finish_later(false);
    return;
  }
  // Persist first: what was counted since the last flush must survive a crash during the upload.
  counters_.flush();
  const std::vector<DayUsage> days = counters_.completedDays();
  if (days.empty()) {
    finish_later(true);
    return;
  }
  std::vector<QString> report_ids;
  report_ids.reserve(days.size());
  for (const DayUsage& day : days) {
    report_ids.push_back(day.report_id);
  }

  QNetworkRequest request(endpoint_url_);
  request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
  request.setHeader(QNetworkRequest::UserAgentHeader, u"PlotJuggler"_s);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  const QByteArray body =
      QJsonDocument(buildBody(days, installation, public_plugin_ids)).toJson(QJsonDocument::Compact);
  in_flight_ = true;
  httpPostWithTimeout(
      *network_, std::move(request), body, std::chrono::milliseconds(kTransferTimeoutMs), this,
      [this, report_ids = std::move(report_ids)](QNetworkReply& reply) {
        in_flight_ = false;
        const bool ok = reply.error() == QNetworkReply::NoError;
        if (ok) {
          counters_.acknowledge(report_ids);
        }
        emit finished(ok);
      });
}

}  // namespace PJ
