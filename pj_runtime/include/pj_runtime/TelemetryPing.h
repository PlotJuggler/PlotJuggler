#pragma once
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace PJ {

// Fire-and-forget anonymous launch ping — the daily-unique-user beacon
// (docs/TELEMETRY.md is the user-facing contract).
//
// send() builds the payload internally (environment facts only — salted-hash
// user id, os, version, arch; the privacy contract is enforced on the wire by
// telemetry_ping_test's allowed-key set) and POSTs it to the endpoint as
// JSON, emitting finished(ok) exactly once per send. The reply body is
// ignored and nothing is ever surfaced to the user; failures are silent by
// design (startup path).
//
// Widget-free (lives in pj_runtime). The shell owns the user-facing opt-out
// and the automation guards; this class owns which builds may report at all,
// so an unpackaged build cannot reach the endpoint from any call site.
class TelemetryPing : public QObject {
  Q_OBJECT

 public:
  explicit TelemetryPing(QObject* parent = nullptr);
  ~TelemetryPing() override;

  // POST the ping now, fire-and-forget. `installation` is the PJ_INSTALLATION
  // build stamp, passed in by the shell so pj_runtime never includes the
  // generated pj_version.h. Each call emits finished(ok) exactly once (a
  // transfer timeout counts as a failure).
  //
  // An unstamped build never reports: "source" (the CMake default, i.e. any
  // self-built tree, matched case-insensitively and trimmed) and an empty
  // stamp send nothing and report finished(false), like any other silent
  // failure. Every other stamp is sent as-is, so a downstream packager's own
  // channel name reaches the dashboard rather than being silently dropped.
  void send(const QString& installation);

  // Override the endpoint (tests point this at a local server).
  void setEndpointUrl(const QUrl& url);
  QUrl endpointUrl() const;

 signals:
  // Terminal outcome of a send; `ok` == the POST got a 2xx reply. The shell
  // deliberately ignores this (failures are silent by design) — it exists for
  // test observability and future diagnostics.
  void finished(bool ok);

 private:
  QNetworkAccessManager* network_ = nullptr;
  QUrl endpoint_url_;
};

}  // namespace PJ
