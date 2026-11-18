#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QJsonObject>
#include <QObject>
#include <QUrl>
#include <set>
#include <string>
#include <vector>

#include "pj_runtime/UsageCounters.h"

class QNetworkAccessManager;

namespace PJ {

/// Uploads the completed days of a UsageCounters as anonymous daily usage reports
/// (docs/TELEMETRY.md is the user-facing contract; usage_reporter_test pins the keys).
///
/// A plugin is named in a report only when its id is in `public_plugin_ids` (the shell passes the
/// bundled, compiled-in and public-registry ids); every other plugin is folded into `_private` per
/// family, so an in-house plugin's id never leaves the machine. Each report carries a month-scoped
/// machine hash, salted apart from the launch ping's user id, so it can count distinct machines in a
/// month without linking months or linking to the ping.
///
/// Days are forgotten only after a 2xx; any failure leaves them for the next launch. Shares the launch
/// ping's channel gate: an unpackaged build never uploads. Widget-free; lives on the GUI thread.
class UsageReporter : public QObject {
  Q_OBJECT

 public:
  explicit UsageReporter(UsageCounters& counters, QObject* parent = nullptr);

  /// POSTs every completed day in one request. Emits finished(ok) exactly once per call: true on a 2xx
  /// or when there is nothing to send (no request is made), false on a refused channel, a redirect or a
  /// transfer failure. Call it once per reporter.
  void send(const QString& installation, const std::set<std::string>& public_plugin_ids);

  /// False for a build whose PJ_INSTALLATION stamp never reports (an unpackaged "source" tree), the gate
  /// send() applies. The shell checks it first, so such a build neither counts nor fetches the registry.
  [[nodiscard]] static bool canUpload(const QString& installation);

  /// The JSON body send() posts for `days`.
  [[nodiscard]] static QJsonObject buildBody(
      const std::vector<DayUsage>& days, const QString& installation, const std::set<std::string>& public_plugin_ids);

  /// Override the endpoint (tests point this at a local server).
  void setEndpointUrl(const QUrl& url);
  [[nodiscard]] QUrl endpointUrl() const;

 signals:
  void finished(bool ok);

 private:
  UsageCounters& counters_;
  QNetworkAccessManager* network_ = nullptr;
  QUrl endpoint_url_;
};

}  // namespace PJ
